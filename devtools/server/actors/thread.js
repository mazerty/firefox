/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// protocol.js uses objects as exceptions in order to define
// error packets.
/* eslint-disable no-throw-literal */

const { Actor } = require("resource://devtools/shared/protocol/Actor.js");
const { Pool } = require("resource://devtools/shared/protocol/Pool.js");
const { threadSpec } = require("resource://devtools/shared/specs/thread.js");

const {
  createValueGrip,
} = require("resource://devtools/server/actors/object/utils.js");
const {
  ObjectActorPool,
} = require("resource://devtools/server/actors/object/ObjectActorPool.js");
const DevToolsUtils = require("resource://devtools/shared/DevToolsUtils.js");
const Debugger = require("Debugger");
const { assert, dumpn, reportException } = DevToolsUtils;
const {
  getAvailableEventBreakpoints,
  eventBreakpointForNotification,
  eventsRequireNotifications,
  firstStatementBreakpointId,
  makeEventBreakpointMessage,
} = require("resource://devtools/server/actors/utils/event-breakpoints.js");
const {
  WatchpointMap,
} = require("resource://devtools/server/actors/utils/watchpoint-map.js");

const Targets = require("devtools/server/actors/targets/index");

loader.lazyRequireGetter(
  this,
  "logEvent",
  "resource://devtools/server/actors/utils/logEvent.js",
  true
);
loader.lazyRequireGetter(
  this,
  "EnvironmentActor",
  "resource://devtools/server/actors/environment.js",
  true
);
loader.lazyRequireGetter(
  this,
  "BreakpointActorMap",
  "resource://devtools/server/actors/utils/breakpoint-actor-map.js",
  true
);
loader.lazyRequireGetter(
  this,
  "EventLoop",
  "resource://devtools/server/actors/utils/event-loop.js",
  true
);
loader.lazyRequireGetter(
  this,
  ["FrameActor", "getSavedFrameParent", "isValidSavedFrame"],
  "resource://devtools/server/actors/frame.js",
  true
);
loader.lazyRequireGetter(
  this,
  "HighlighterEnvironment",
  "resource://devtools/server/actors/highlighters.js",
  true
);
loader.lazyRequireGetter(
  this,
  "PausedDebuggerOverlay",
  "resource://devtools/server/actors/highlighters/paused-debugger.js",
  true
);

const PROMISE_REACTIONS = new WeakMap();
function cacheReactionsForFrame(frame) {
  if (frame.asyncPromise) {
    const reactions = frame.asyncPromise.getPromiseReactions();
    const existingReactions = PROMISE_REACTIONS.get(frame.asyncPromise);
    if (
      reactions.length &&
      (!existingReactions || reactions.length > existingReactions.length)
    ) {
      PROMISE_REACTIONS.set(frame.asyncPromise, reactions);
    }
  }
}

function createStepForReactionTracking(onStep) {
  return function () {
    cacheReactionsForFrame(this);
    return onStep ? onStep.apply(this, arguments) : undefined;
  };
}

const getAsyncParentFrame = frame => {
  if (!frame.asyncPromise) {
    return null;
  }

  // We support returning Frame actors for frames that are suspended
  // at an 'await', and here we want to walk upward to look for the first
  // frame that will be resumed when the current frame's promise resolves.
  let reactions =
    PROMISE_REACTIONS.get(frame.asyncPromise) ||
    frame.asyncPromise.getPromiseReactions();

  // eslint-disable-next-line no-constant-condition
  while (true) {
    // We loop here because we may have code like:
    //
    //   async function inner(){ debugger; }
    //
    //   async function outer() {
    //     await Promise.resolve().then(() => inner());
    //   }
    //
    // where we can see that when `inner` resolves, we will resume from
    // `outer`, even though there is a layer of promises between, and
    // that layer could be any number of promises deep.
    if (!(reactions[0] instanceof Debugger.Object)) {
      break;
    }

    reactions = reactions[0].getPromiseReactions();
  }

  if (reactions[0] instanceof Debugger.Frame) {
    return reactions[0];
  }
  return null;
};
const RESTARTED_FRAMES = new WeakSet();

// Thread actor possible states:
const STATES = {
  //  Before ThreadActor.attach is called:
  DETACHED: "detached",
  //  After the actor is destroyed:
  EXITED: "exited",

  // States possible in between DETACHED AND EXITED:
  // Default state, when the thread isn't paused,
  RUNNING: "running",
  // When paused on any type of breakpoint, or, when the client requested an interrupt.
  PAUSED: "paused",
};
exports.STATES = STATES;

// Possible values for the `why.type` attribute in "paused" event
const PAUSE_REASONS = {
  ALREADY_PAUSED: "alreadyPaused",
  INTERRUPTED: "interrupted", // Associated with why.onNext attribute
  MUTATION_BREAKPOINT: "mutationBreakpoint", // Associated with why.mutationType and why.message attributes
  DEBUGGER_STATEMENT: "debuggerStatement",
  EXCEPTION: "exception",
  XHR: "XHR",
  EVENT_BREAKPOINT: "eventBreakpoint",
  RESUME_LIMIT: "resumeLimit",
};
exports.PAUSE_REASONS = PAUSE_REASONS;

class ThreadActor extends Actor {
  /**
   * Creates a ThreadActor.
   *
   * ThreadActors manage execution/inspection of debuggees.
   *
   * @param {TargetActor} targetActor
   *        This `ThreadActor`'s parent actor. i.e. one of the many Target actors.
   */
  constructor(targetActor) {
    super(targetActor.conn, threadSpec);

    // This attribute is used by various other actors to find the target actor
    this.targetActor = targetActor;

    this._state = STATES.DETACHED;
    this._options = {
      skipBreakpoints: false,
    };
    this._gripDepth = 0;
    this._targetActorClosed = false;
    this._observingNetwork = false;
    this._shouldShowPauseOverlay = true;
    this._frameActors = [];
    this._xhrBreakpoints = [];

    this._dbg = null;
    this._threadLifetimePool = null;
    this._activeEventPause = null;
    this._pauseOverlay = null;
    this._priorPause = null;

    this._activeEventBreakpoints = new Set();
    this._frameActorMap = new WeakMap();
    this._debuggerSourcesSeen = new WeakSet();

    // A Set of URLs string to watch for when new sources are found by
    // the debugger instance.
    this._onLoadBreakpointURLs = new Set();

    // A WeakMap from Debugger.Frame to an exception value which will be ignored
    // when deciding to pause if the value is thrown by the frame. When we are
    // pausing on exceptions then we only want to pause when the youngest frame
    // throws a particular exception, instead of for all older frames as well.
    this._handledFrameExceptions = new WeakMap();

    this._watchpointsMap = new WatchpointMap(this);

    this.breakpointActorMap = new BreakpointActorMap(this);

    this._nestedEventLoop = new EventLoop({
      thread: this,
    });

    this.onNewSourceEvent = this.onNewSourceEvent.bind(this);

    this.createCompletionGrip = this.createCompletionGrip.bind(this);
    this.onDebuggerStatement = this.onDebuggerStatement.bind(this);
    this.onNewScript = this.onNewScript.bind(this);
    this._onOpeningRequest = this._onOpeningRequest.bind(this);
    this._onNewDebuggee = this._onNewDebuggee.bind(this);
    this._onExceptionUnwind = this._onExceptionUnwind.bind(this);
    this._eventBreakpointListener = this._eventBreakpointListener.bind(this);
    this._onWindowReady = this._onWindowReady.bind(this);
    this._onWillNavigate = this._onWillNavigate.bind(this);
    this._onNavigate = this._onNavigate.bind(this);

    this.targetActor.on("window-ready", this._onWindowReady);
    this.targetActor.on("will-navigate", this._onWillNavigate);
    this.targetActor.on("navigate", this._onNavigate);

    this._firstStatementBreakpoint = null;
    this._debuggerNotificationObserver = new DebuggerNotificationObserver();
  }

  // Used by the ObjectActor to keep track of the depth of grip() calls.
  _gripDepth = null;

  get dbg() {
    if (!this._dbg) {
      this._dbg = this.targetActor.dbg;
      // Keep the debugger disabled until a client attaches.
      if (this._state === STATES.DETACHED) {
        this._dbg.disable();
      } else {
        this._dbg.enable();
      }
    }
    return this._dbg;
  }

  // Current state of the thread actor:
  //  - detached: state, before ThreadActor.attach is called,
  //  - exited: state, after the actor is destroyed,
  // States possible in between these two states:
  //  - running: default state, when the thread isn't paused,
  //  - paused: state, when paused on any type of breakpoint, or, when the client requested an interrupt.
  get state() {
    return this._state;
  }

  // XXX: soon to be equivalent to !isDestroyed once the thread actor is initialized on target creation.
  get attached() {
    return this.state == STATES.RUNNING || this.state == STATES.PAUSED;
  }

  get pauseLifetimePool() {
    return this._pausePool;
  }

  get threadLifetimePool() {
    if (!this._threadLifetimePool) {
      this._threadLifetimePool = new ObjectActorPool(this, "thread", true);
      this._threadLifetimePool.objectActors = new WeakMap();
    }
    return this._threadLifetimePool;
  }

  getThreadLifetimeObject(raw) {
    return this.threadLifetimePool.objectActors.get(raw);
  }

  promoteObjectToThreadLifetime(objectActor) {
    this.threadLifetimePool.manage(objectActor);
    this.threadLifetimePool.objectActors.set(objectActor.obj, objectActor);
  }

  get sourcesManager() {
    return this.targetActor.sourcesManager;
  }

  get breakpoints() {
    return this.targetActor.breakpoints;
  }

  get youngestFrame() {
    if (this.state != STATES.PAUSED) {
      return null;
    }
    return this.dbg.getNewestFrame();
  }

  get shouldSkipAnyBreakpoint() {
    return (
      // Disable all types of breakpoints if:
      // - the user explicitly requested it via the option
      this._options.skipBreakpoints ||
      // - or when we are evaluating some javascript via the console actor and disableBreaks
      //   has been set to true (which happens for most evaluating except the console input)
      this.insideClientEvaluation?.disableBreaks
    );
  }

  isPaused() {
    return this._state === STATES.PAUSED;
  }

  lastPausedPacket() {
    return this._priorPause;
  }

  /**
   * Remove all debuggees and clear out the thread's sources.
   */
  clearDebuggees() {
    if (this._dbg) {
      this.dbg.removeAllDebuggees();
    }
  }

  /**
   * Destroy the debugger and put the actor in the exited state.
   *
   * As part of destroy, we: clean up listeners, debuggees and
   * clear actor pools associated with the lifetime of this actor.
   */
  destroy() {
    dumpn("in ThreadActor.prototype.destroy");
    if (this._state == STATES.PAUSED) {
      this.doResume();
    }

    this.removeAllWatchpoints();
    this._xhrBreakpoints = [];
    this._updateNetworkObserver();

    this._activeEventBreakpoints = new Set();
    this._debuggerNotificationObserver.removeListener(
      this._eventBreakpointListener
    );

    for (const global of this.dbg.getDebuggees()) {
      try {
        this._debuggerNotificationObserver.disconnect(
          global.unsafeDereference()
        );
      } catch (e) {}
    }

    this.targetActor.off("window-ready", this._onWindowReady);
    this.targetActor.off("will-navigate", this._onWillNavigate);
    this.targetActor.off("navigate", this._onNavigate);

    this.sourcesManager.off("newSource", this.onNewSourceEvent);
    this.clearDebuggees();
    this._threadLifetimePool.destroy();
    this._threadLifetimePool = null;
    this._dbg = null;
    this._state = STATES.EXITED;

    super.destroy();
  }

  /**
   * Tells if the thread actor has been initialized/attached on target creation
   * by the server codebase. (And not late, from the frontend, by the TargetMixinFront class)
   */
  isAttached() {
    return !!this.alreadyAttached;
  }

  // Request handlers
  attach(options) {
    // Note that the client avoids trying to call attach if already attached.
    // But just in case, avoid any possible duplicate call to attach.
    if (this.alreadyAttached) {
      return;
    }

    if (this.state === STATES.EXITED) {
      throw {
        error: "exited",
        message: "threadActor has exited",
      };
    }

    if (this.state !== STATES.DETACHED) {
      throw {
        error: "wrongState",
        message: "Current state is " + this.state,
      };
    }

    this.dbg.onDebuggerStatement = this.onDebuggerStatement;
    this.dbg.onNewScript = this.onNewScript;
    this.dbg.onNewDebuggee = this._onNewDebuggee;

    this.sourcesManager.on("newSource", this.onNewSourceEvent);

    this.reconfigure(options);

    // Switch state from DETACHED to RUNNING
    this._state = STATES.RUNNING;

    this.alreadyAttached = true;
    this.dbg.enable();

    if (Services.obs) {
      // Set a wrappedJSObject property so |this| can be sent via the observer service
      // for the xpcshell harness.
      this.wrappedJSObject = this;
      Services.obs.notifyObservers(this, "devtools-thread-ready");
    }
  }

  toggleEventLogging(logEventBreakpoints) {
    this._options.logEventBreakpoints = logEventBreakpoints;
    return this._options.logEventBreakpoints;
  }

  get pauseOverlay() {
    if (this._pauseOverlay) {
      return this._pauseOverlay;
    }

    const env = new HighlighterEnvironment();
    env.initFromTargetActor(this.targetActor);
    const highlighter = new PausedDebuggerOverlay(env, {
      resume: () => this.resume(null),
      stepOver: () => this.resume({ type: "next" }),
    });
    this._pauseOverlay = highlighter;
    return highlighter;
  }

  _canShowOverlay() {
    // Only attempt to show on overlay on WindowGlobal targets, which displays a document.
    // Workers and content processes can't display any overlay.
    if (this.targetActor.targetType != Targets.TYPES.FRAME) {
      return false;
    }

    const { window } = this.targetActor;

    // The CanvasFrameAnonymousContentHelper class we're using to create the paused overlay
    // need to have access to a documentElement.
    // We might have access to a non-chrome window getter that is a Sandox (e.g. in the
    // case of ContentProcessTargetActor).
    if (!window?.document?.documentElement) {
      return false;
    }

    // Ignore privileged document (top level window, special about:* pages, …).
    if (window.isChromeWindow) {
      return false;
    }

    return true;
  }

  async showOverlay() {
    if (
      !this._shouldShowPauseOverlay ||
      !this.isPaused() ||
      !this._canShowOverlay()
    ) {
      return;
    }

    const reason = this._priorPause.why.type;
    await this.pauseOverlay.isReady;

    // we might not be paused anymore.
    if (!this.isPaused()) {
      return;
    }

    this.pauseOverlay.show(reason);
  }

  hideOverlay() {
    if (this._canShowOverlay() && this._pauseOverlay) {
      this.pauseOverlay.hide();
    }
  }

  /**
   * Tell the thread to automatically add a breakpoint on the first line of
   * a given file, when it is first loaded.
   *
   * This is currently only used by the xpcshell test harness, and unless
   * we decide to expand the scope of this feature, we should keep it that way.
   */
  setBreakpointOnLoad(urls) {
    this._onLoadBreakpointURLs = new Set(urls);
  }

  _findXHRBreakpointIndex(p, m) {
    return this._xhrBreakpoints.findIndex(
      ({ path, method }) => path === p && method === m
    );
  }

  // We clear the priorPause field when a breakpoint is added or removed
  // at the same location because we are no longer worried about pausing twice
  // at that location (e.g. debugger statement, stepping).
  _maybeClearPriorPause(location) {
    if (!this._priorPause) {
      return;
    }

    const { where } = this._priorPause.frame;
    if (where.line === location.line && where.column === location.column) {
      this._priorPause = null;
    }
  }

  async setBreakpoint(location, options) {
    // Automatically initialize the thread actor if it wasn't yet done.
    // Note that ideally, it should rather be done via reconfigure/thread configuration.
    if (this._state === STATES.DETACHED) {
      this.attach({});
      this.addAllSources();
    }

    let actor = this.breakpointActorMap.get(location);
    // Avoid resetting the exact same breakpoint twice
    if (actor && JSON.stringify(actor.options) == JSON.stringify(options)) {
      return;
    }
    if (!actor) {
      actor = this.breakpointActorMap.getOrCreateBreakpointActor(location);
    }
    actor.setOptions(options);
    this._maybeClearPriorPause(location);

    if (location.sourceUrl) {
      // There can be multiple source actors for a URL if there are multiple
      // inline sources on an HTML page.
      const sourceActors = this.sourcesManager.getSourceActorsByURL(
        location.sourceUrl
      );
      for (const sourceActor of sourceActors) {
        await sourceActor.applyBreakpoint(actor);
      }
    } else {
      const sourceActor = this.sourcesManager.getSourceActorById(
        location.sourceId
      );
      if (sourceActor) {
        await sourceActor.applyBreakpoint(actor);
      }
    }
  }

  removeBreakpoint(location) {
    const actor = this.breakpointActorMap.getOrCreateBreakpointActor(location);
    this._maybeClearPriorPause(location);
    actor.delete();
  }

  removeAllXHRBreakpoints() {
    this._xhrBreakpoints = [];
    return this._updateNetworkObserver();
  }

  removeXHRBreakpoint(path, method) {
    const index = this._findXHRBreakpointIndex(path, method);

    if (index >= 0) {
      this._xhrBreakpoints.splice(index, 1);
    }
    return this._updateNetworkObserver();
  }

  setXHRBreakpoint(path, method) {
    // request.path is a string,
    // If requested url contains the path, then we pause.
    const index = this._findXHRBreakpointIndex(path, method);

    if (index === -1) {
      this._xhrBreakpoints.push({ path, method });
    }
    return this._updateNetworkObserver();
  }

  getAvailableEventBreakpoints() {
    return getAvailableEventBreakpoints(this.targetActor.targetGlobal);
  }
  getActiveEventBreakpoints() {
    return Array.from(this._activeEventBreakpoints);
  }

  /**
   * Add event breakpoints to the list of active event breakpoints
   *
   * @param {Array<String>} ids: events to add (e.g. ["event.mouse.click","event.mouse.mousedown"])
   */
  addEventBreakpoints(ids) {
    this.setActiveEventBreakpoints(
      this.getActiveEventBreakpoints().concat(ids)
    );
  }

  /**
   * Remove event breakpoints from the list of active event breakpoints
   *
   * @param {Array<String>} ids: events to remove (e.g. ["event.mouse.click","event.mouse.mousedown"])
   */
  removeEventBreakpoints(ids) {
    this.setActiveEventBreakpoints(
      this.getActiveEventBreakpoints().filter(eventBp => !ids.includes(eventBp))
    );
  }

  /**
   * Set the the list of active event breakpoints
   *
   * @param {Array<String>} ids: events to add breakpoint for (e.g. ["event.mouse.click","event.mouse.mousedown"])
   */
  setActiveEventBreakpoints(ids) {
    this._activeEventBreakpoints = new Set(ids);

    if (eventsRequireNotifications(ids)) {
      this._debuggerNotificationObserver.addListener(
        this._eventBreakpointListener
      );
    } else {
      this._debuggerNotificationObserver.removeListener(
        this._eventBreakpointListener
      );
    }

    if (this._activeEventBreakpoints.has(firstStatementBreakpointId())) {
      this._ensureFirstStatementBreakpointInitialized();

      this._firstStatementBreakpoint.hit = frame =>
        this._pauseAndRespondEventBreakpoint(
          frame,
          firstStatementBreakpointId()
        );
    } else if (this._firstStatementBreakpoint) {
      // Disabling the breakpoint disables the feature as much as we need it
      // to. We do not bother removing breakpoints from the scripts themselves
      // here because the breakpoints will be a no-op if `hit` is `null`, and
      // if we wanted to remove them, we'd need a way to iterate through them
      // all, which would require us to hold strong references to them, which
      // just isn't needed. Plus, if the user disables and then re-enables the
      // feature again later, the breakpoints will still be there to work.
      this._firstStatementBreakpoint.hit = null;
    }
  }

  _ensureFirstStatementBreakpointInitialized() {
    if (this._firstStatementBreakpoint) {
      return;
    }

    this._firstStatementBreakpoint = { hit: null };
    for (const script of this.dbg.findScripts()) {
      this._maybeTrackFirstStatementBreakpoint(script);
    }
  }

  _maybeTrackFirstStatementBreakpointForNewGlobal(global) {
    if (this._firstStatementBreakpoint) {
      for (const script of this.dbg.findScripts({ global })) {
        this._maybeTrackFirstStatementBreakpoint(script);
      }
    }
  }

  _maybeTrackFirstStatementBreakpoint(script) {
    if (
      // If the feature is not enabled yet, there is nothing to do.
      !this._firstStatementBreakpoint ||
      // WASM files don't have a first statement.
      script.format !== "js" ||
      // All "top-level" scripts are non-functions, whether that's because
      // the script is a module, a global script, or an eval or what.
      script.isFunction
    ) {
      return;
    }

    const bps = script.getPossibleBreakpoints();

    // Scripts aren't guaranteed to have a step start if for instance the
    // file contains only function declarations, so in that case we try to
    // fall back to whatever we can find.
    let meta = bps.find(bp => bp.isStepStart) || bps[0];
    if (!meta) {
      // We've tried to avoid using `getAllColumnOffsets()` because the set of
      // locations included in this list is very under-defined, but for this
      // usecase it's not the end of the world. Maybe one day we could have an
      // "onEnterFrame" that was scoped to a specific script to avoid this.
      meta = script.getAllColumnOffsets()[0];
    }

    if (!meta) {
      // Not certain that this is actually possible, but including for sanity
      // so that we don't throw unexpectedly.
      return;
    }
    script.setBreakpoint(meta.offset, this._firstStatementBreakpoint);
  }

  _onNewDebuggee(global) {
    this._maybeTrackFirstStatementBreakpointForNewGlobal(global);
    try {
      this._debuggerNotificationObserver.connect(global.unsafeDereference());
    } catch (e) {}
  }

  _updateNetworkObserver() {
    // Workers don't have access to `Services` and even if they did, network
    // requests are all dispatched to the main thread, so there would be
    // nothing here to listen for. We'll need to revisit implementing
    // XHR breakpoints for workers.
    if (isWorker) {
      return false;
    }

    if (this._xhrBreakpoints.length && !this._observingNetwork) {
      this._observingNetwork = true;
      Services.obs.addObserver(
        this._onOpeningRequest,
        "http-on-opening-request"
      );
    } else if (this._xhrBreakpoints.length === 0 && this._observingNetwork) {
      this._observingNetwork = false;
      Services.obs.removeObserver(
        this._onOpeningRequest,
        "http-on-opening-request"
      );
    }

    return true;
  }

  _onOpeningRequest(subject) {
    if (this.shouldSkipAnyBreakpoint) {
      return;
    }

    const channel = subject.QueryInterface(Ci.nsIHttpChannel);
    const url = channel.URI.asciiSpec;
    const requestMethod = channel.requestMethod;

    let causeType = Ci.nsIContentPolicy.TYPE_OTHER;
    if (channel.loadInfo) {
      causeType = channel.loadInfo.externalContentPolicyType;
    }

    const isXHR =
      causeType === Ci.nsIContentPolicy.TYPE_XMLHTTPREQUEST ||
      causeType === Ci.nsIContentPolicy.TYPE_FETCH;

    if (!isXHR) {
      // We currently break only if the request is either fetch or xhr
      return;
    }

    let shouldPause = false;
    for (const { path, method } of this._xhrBreakpoints) {
      if (method !== "ANY" && method !== requestMethod) {
        continue;
      }
      if (url.includes(path)) {
        shouldPause = true;
        break;
      }
    }

    if (shouldPause) {
      const frame = this.dbg.getNewestFrame();

      // If there is no frame, this request was dispatched by logic that isn't
      // primarily JS, so pausing the event loop wouldn't make sense.
      // This covers background requests like loading the initial page document,
      // or loading favicons. This also includes requests dispatched indirectly
      // from workers. We'll need to handle them separately in the future.
      if (frame) {
        this._pauseAndRespond(frame, { type: PAUSE_REASONS.XHR });
      }
    }
  }

  reconfigure(options = {}) {
    if (this.state == STATES.EXITED) {
      throw {
        error: "wrongState",
      };
    }
    this._options = { ...this._options, ...options };

    if ("observeAsmJS" in options) {
      this.dbg.allowUnobservedAsmJS = !options.observeAsmJS;
    }
    if ("observeWasm" in options) {
      this.dbg.allowUnobservedWasm = !options.observeWasm;
    }
    if ("pauseOverlay" in options) {
      this._shouldShowPauseOverlay = !!options.pauseOverlay;
      if (this.isPaused()) {
        if (!this._shouldShowPauseOverlay) {
          this.hideOverlay();
        } else {
          this.showOverlay();
        }
      }
    }

    if (
      "pauseWorkersUntilAttach" in options &&
      this.targetActor.pauseWorkersUntilAttach
    ) {
      this.targetActor.pauseWorkersUntilAttach(options.pauseWorkersUntilAttach);
    }

    if (options.breakpoints) {
      for (const breakpoint of Object.values(options.breakpoints)) {
        this.setBreakpoint(breakpoint.location, breakpoint.options);
      }
    }

    if (options.eventBreakpoints) {
      this.setActiveEventBreakpoints(options.eventBreakpoints);
    }

    // Only consider this options if an explicit boolean value is passed.
    if (typeof this._options.shouldPauseOnDebuggerStatement == "boolean") {
      this.setPauseOnDebuggerStatement(
        this._options.shouldPauseOnDebuggerStatement
      );
    }
    this.setPauseOnExceptions(this._options.pauseOnExceptions);
  }

  _eventBreakpointListener(notification) {
    if (this._state === STATES.PAUSED || this._state === STATES.DETACHED) {
      return;
    }

    const eventBreakpoint = eventBreakpointForNotification(
      this.dbg,
      notification
    );

    if (!this._activeEventBreakpoints.has(eventBreakpoint)) {
      return;
    }

    if (notification.phase === "pre" && !this._activeEventPause) {
      this._activeEventPause = this._captureDebuggerHooks();

      this.dbg.onEnterFrame =
        this._makeEventBreakpointEnterFrame(eventBreakpoint);
    } else if (notification.phase === "post" && this._activeEventPause) {
      this._restoreDebuggerHooks(this._activeEventPause);
      this._activeEventPause = null;
    } else if (!notification.phase && !this._activeEventPause) {
      const frame = this.dbg.getNewestFrame();
      if (frame) {
        if (this.sourcesManager.isFrameBlackBoxed(frame)) {
          return;
        }

        this._pauseAndRespondEventBreakpoint(frame, eventBreakpoint);
      }
    }
  }

  _makeEventBreakpointEnterFrame(eventBreakpoint) {
    return frame => {
      if (this.sourcesManager.isFrameBlackBoxed(frame)) {
        return undefined;
      }

      this._restoreDebuggerHooks(this._activeEventPause);
      this._activeEventPause = null;

      return this._pauseAndRespondEventBreakpoint(frame, eventBreakpoint);
    };
  }

  _pauseAndRespondEventBreakpoint(frame, eventBreakpoint) {
    if (this.shouldSkipAnyBreakpoint) {
      return undefined;
    }

    if (this._options.logEventBreakpoints) {
      return logEvent({ threadActor: this, frame });
    }

    return this._pauseAndRespond(frame, {
      type: PAUSE_REASONS.EVENT_BREAKPOINT,
      breakpoint: eventBreakpoint,
      message: makeEventBreakpointMessage(eventBreakpoint),
    });
  }

  _captureDebuggerHooks() {
    return {
      onEnterFrame: this.dbg.onEnterFrame,
      onStep: this.dbg.onStep,
      onPop: this.dbg.onPop,
    };
  }

  _restoreDebuggerHooks(hooks) {
    this.dbg.onEnterFrame = hooks.onEnterFrame;
    this.dbg.onStep = hooks.onStep;
    this.dbg.onPop = hooks.onPop;
  }

  /**
   * Pause the debuggee, by entering a nested event loop, and return a 'paused'
   * packet to the client.
   *
   * @param Debugger.Frame frame
   *        The newest debuggee frame in the stack.
   * @param object reason
   *        An object with a 'type' property containing the reason for the pause.
   * @param function onPacket
   *        Hook to modify the packet before it is sent. Feel free to return a
   *        promise.
   */
  _pauseAndRespond(frame, reason, onPacket = k => k) {
    try {
      const packet = this._paused(frame);
      if (!packet) {
        return undefined;
      }

      const { sourceActor, line, column } =
        this.sourcesManager.getFrameLocation(frame);

      packet.why = reason;

      if (!sourceActor) {
        // If the frame location is in a source that not pass the 'isHiddenSource'
        // check and thus has no actor, we do not bother pausing.
        return undefined;
      }

      packet.frame.where = {
        actor: sourceActor.actorID,
        line,
        column,
      };
      const pkt = onPacket(packet);

      this._priorPause = pkt;
      this.emit("paused", pkt);
      this.showOverlay();
    } catch (error) {
      reportException("DBG-SERVER", error);
      this.conn.send({
        error: "unknownError",
        message: error.message + "\n" + error.stack,
      });
      return undefined;
    }

    try {
      this._nestedEventLoop.enter();
    } catch (e) {
      reportException("TA__pauseAndRespond", e);
    }

    if (this._requestedFrameRestart) {
      return null;
    }

    // If the parent actor has been closed, terminate the debuggee script
    // instead of continuing. Executing JS after the content window is gone is
    // a bad idea.
    return this._targetActorClosed ? null : undefined;
  }

  _makeOnEnterFrame() {
    return frame => {
      if (this._requestedFrameRestart) {
        return null;
      }

      // Continue forward until we get to a valid step target.
      const { onStep, onPop } = this._makeSteppingHooks({
        steppingType: "next",
      });

      if (this.sourcesManager.isFrameBlackBoxed(frame)) {
        return undefined;
      }

      frame.onStep = onStep;
      frame.onPop = onPop;
      return undefined;
    };
  }

  _makeOnPop({ pauseAndRespond, steppingType }) {
    const thread = this;
    return function (completion) {
      if (thread._requestedFrameRestart === this) {
        return thread.restartFrame(this);
      }

      // onPop is called when we temporarily leave an async/generator
      if (steppingType != "finish" && (completion.await || completion.yield)) {
        thread.suspendedFrame = this;
        thread.dbg.onEnterFrame = undefined;
        return undefined;
      }

      // Note that we're popping this frame; we need to watch for
      // subsequent step events on its caller.
      this.reportedPop = true;

      // Cache the frame so that the onPop and onStep hooks are cleared
      // on the next pause.
      thread.suspendedFrame = this;

      if (
        steppingType != "finish" &&
        !thread.sourcesManager.isFrameBlackBoxed(this)
      ) {
        const pauseAndRespValue = pauseAndRespond(this, packet =>
          thread.createCompletionGrip(packet, completion)
        );

        // If the requested frame to restart differs from this frame, we don't
        // need to restart it at this point.
        if (thread._requestedFrameRestart === this) {
          return thread.restartFrame(this);
        }

        return pauseAndRespValue;
      }

      thread._attachSteppingHooks(this, "next", completion);
      return undefined;
    };
  }

  restartFrame(frame) {
    this._requestedFrameRestart = null;
    this._priorPause = null;

    if (
      frame.type !== "call" ||
      frame.script.isGeneratorFunction ||
      frame.script.isAsyncFunction
    ) {
      return undefined;
    }
    RESTARTED_FRAMES.add(frame);

    const completion = frame.callee.apply(frame.this, frame.arguments);

    return completion;
  }

  hasMoved(frame, newType) {
    const newLocation = this.sourcesManager.getFrameLocation(frame);

    if (!this._priorPause) {
      return true;
    }

    // Recursion/Loops makes it okay to resume and land at
    // the same breakpoint or debugger statement.
    // It is not okay to transition from a breakpoint to debugger statement
    // or a step to a debugger statement.
    const { type } = this._priorPause.why;

    // Conditional breakpoint are doing something weird as they are using "breakpoint" type
    // unless they throw in which case they will be "breakpointConditionThrown".
    if (
      type == newType ||
      (type == "breakpointConditionThrown" && newType == "breakpoint")
    ) {
      return true;
    }

    const { line, column } = this._priorPause.frame.where;
    return line !== newLocation.line || column !== newLocation.column;
  }

  _makeOnStep({ pauseAndRespond, startFrame, completion }) {
    const thread = this;
    return function () {
      if (thread._validFrameStepOffset(this, startFrame, this.offset)) {
        return pauseAndRespond(this, packet =>
          thread.createCompletionGrip(packet, completion)
        );
      }

      return undefined;
    };
  }

  _validFrameStepOffset(frame, startFrame, offset) {
    const meta = frame.script.getOffsetMetadata(offset);

    // Continue if:
    // 1. the location is not a valid breakpoint position
    // 2. the source is blackboxed
    // 3. we have not moved since the last pause
    if (
      !meta.isBreakpoint ||
      this.sourcesManager.isFrameBlackBoxed(frame) ||
      !this.hasMoved(frame)
    ) {
      return false;
    }

    // Pause if:
    // 1. the frame has changed
    // 2. the location is a step position.
    return frame !== startFrame || meta.isStepStart;
  }

  atBreakpointLocation(frame) {
    const location = this.sourcesManager.getFrameLocation(frame);
    return !!this.breakpointActorMap.get(location);
  }

  createCompletionGrip(packet, completion) {
    if (!completion) {
      return packet;
    }

    packet.why.frameFinished = {};

    if (completion.hasOwnProperty("return")) {
      packet.why.frameFinished.return = this.createValueGrip(completion.return);
    } else if (completion.hasOwnProperty("yield")) {
      packet.why.frameFinished.return = this.createValueGrip(completion.yield);
    } else if (completion.hasOwnProperty("throw")) {
      packet.why.frameFinished.throw = this.createValueGrip(completion.throw);
    }

    return packet;
  }

  /**
   * Define the JS hook functions for stepping.
   */
  _makeSteppingHooks({ steppingType, startFrame, completion }) {
    // Bind these methods and state because some of the hooks are called
    // with 'this' set to the current frame. Rather than repeating the
    // binding in each _makeOnX method, just do it once here and pass it
    // in to each function.
    const steppingHookState = {
      pauseAndRespond: (frame, onPacket = k => k) =>
        this._pauseAndRespond(
          frame,
          { type: PAUSE_REASONS.RESUME_LIMIT },
          onPacket
        ),
      startFrame: startFrame || this.youngestFrame,
      steppingType,
      completion,
    };

    return {
      onEnterFrame: this._makeOnEnterFrame(steppingHookState),
      onPop: this._makeOnPop(steppingHookState),
      onStep: this._makeOnStep(steppingHookState),
    };
  }

  /**
   * Handle attaching the various stepping hooks we need to attach when we
   * receive a resume request with a resumeLimit property.
   *
   * @param Object { resumeLimit }
   *        The values received over the RDP.
   * @returns A promise that resolves to true once the hooks are attached, or is
   *          rejected with an error packet.
   */
  async _handleResumeLimit({ resumeLimit, frameActorID }) {
    const steppingType = resumeLimit.type;
    if (
      !["break", "step", "next", "finish", "restart"].includes(steppingType)
    ) {
      return Promise.reject({
        error: "badParameterType",
        message: "Unknown resumeLimit type",
      });
    }

    let frame = this.youngestFrame;

    if (frameActorID) {
      frame = this._framesPool.getActorByID(frameActorID).frame;
      if (!frame) {
        throw new Error("Frame should exist in the frames pool.");
      }
    }

    if (steppingType === "restart") {
      if (
        frame.type !== "call" ||
        frame.script.isGeneratorFunction ||
        frame.script.isAsyncFunction
      ) {
        return undefined;
      }
      this._requestedFrameRestart = frame;
    }

    return this._attachSteppingHooks(frame, steppingType, undefined);
  }

  _attachSteppingHooks(frame, steppingType, completion) {
    // If we are stepping out of the onPop handler, we want to use "next" mode
    // so that the parent frame's handlers behave consistently.
    if (steppingType === "finish" && frame.reportedPop) {
      steppingType = "next";
    }

    // If there are no more frames on the stack, use "step" mode so that we will
    // pause on the next script to execute.
    const stepFrame = this._getNextStepFrame(frame);
    if (!stepFrame) {
      steppingType = "step";
    }

    const { onEnterFrame, onPop, onStep } = this._makeSteppingHooks({
      steppingType,
      completion,
      startFrame: frame,
    });

    if (steppingType === "step" || steppingType === "restart") {
      this.dbg.onEnterFrame = onEnterFrame;
    }

    if (stepFrame) {
      switch (steppingType) {
        case "step":
        case "break":
        case "next":
          if (stepFrame.script) {
            if (!this.sourcesManager.isFrameBlackBoxed(stepFrame)) {
              stepFrame.onStep = onStep;
            }
          }
        // eslint-disable-next-line no-fallthrough
        case "finish":
          stepFrame.onStep = createStepForReactionTracking(stepFrame.onStep);
        // eslint-disable-next-line no-fallthrough
        case "restart":
          stepFrame.onPop = onPop;
          break;
      }
    }

    return true;
  }

  /**
   * Clear the onStep and onPop hooks for all frames on the stack.
   */
  _clearSteppingHooks() {
    if (this.suspendedFrame) {
      this.suspendedFrame.onStep = undefined;
      this.suspendedFrame.onPop = undefined;
      this.suspendedFrame = undefined;
    }

    let frame = this.youngestFrame;
    if (frame?.onStack) {
      while (frame) {
        frame.onStep = undefined;
        frame.onPop = undefined;
        frame = frame.older;
      }
    }
  }

  /**
   * Handle a protocol request to resume execution of the debuggee.
   */
  async resume(resumeLimit, frameActorID) {
    if (this._state !== STATES.PAUSED) {
      return {
        error: "wrongState",
        message:
          "Can't resume when debuggee isn't paused. Current state is '" +
          this._state +
          "'",
        state: this._state,
      };
    }

    // In case of multiple nested event loops (due to multiple debuggers open in
    // different tabs or multiple devtools clients connected to the same tab)
    // only allow resumption in a LIFO order.
    if (!this._nestedEventLoop.isTheLastPausedThreadActor()) {
      return {
        error: "wrongOrder",
        message: "trying to resume in the wrong order.",
      };
    }

    try {
      if (resumeLimit) {
        await this._handleResumeLimit({ resumeLimit, frameActorID });
      } else {
        this._clearSteppingHooks();
      }

      this.doResume({ resumeLimit });
      return {};
    } catch (error) {
      return error instanceof Error
        ? {
            error: "unknownError",
            message: DevToolsUtils.safeErrorString(error),
          }
        : // It is a known error, and the promise was rejected with an error
          // packet.
          error;
    }
  }

  /**
   * Only resume and notify necessary observers. This should be used in cases
   * when we do not want to notify the front end of a resume, for example when
   * we are shutting down.
   */
  doResume() {
    this._state = STATES.RUNNING;

    // Drop the actors in the pause actor pool.
    this._pausePool.destroy();
    this._pausePool = null;

    this._pauseActor = null;
    this._nestedEventLoop.exit();

    // Tell anyone who cares of the resume (as of now, that's the xpcshell harness and
    // devtools-startup.js when handling the --wait-for-jsdebugger flag)
    this.emit("resumed");
    this.hideOverlay();
  }

  /**
   * Set the debugging hook to pause on exceptions if configured to do so.
   *
   * Note that this is also called when evaluating conditional breakpoints.
   *
   * @param {Boolean} doPause
   *        Should watch for pause or not. `_onExceptionUnwind` function will
   *        then be notified about new caught or uncaught exception being fired.
   */
  setPauseOnExceptions(doPause) {
    if (doPause) {
      this.dbg.onExceptionUnwind = this._onExceptionUnwind;
    } else {
      this.dbg.onExceptionUnwind = undefined;
    }
  }

  /**
   * Set the debugging hook to pause on debugger statement if configured to do so.
   *
   * Note that the thread actor will pause on exception by default.
   * This method has to be called with a falsy value to disable it.
   *
   * @param {Boolean} doPause
   *        Controls whether we should or should not pause on debugger statement.
   */
  setPauseOnDebuggerStatement(doPause) {
    this.dbg.onDebuggerStatement = doPause
      ? this.onDebuggerStatement
      : undefined;
  }

  isPauseOnExceptionsEnabled() {
    return this.dbg.onExceptionUnwind == this._onExceptionUnwind;
  }

  /**
   * Helper method that returns the next frame when stepping.
   */
  _getNextStepFrame(frame) {
    const endOfFrame = frame.reportedPop;
    const stepFrame = endOfFrame
      ? frame.older || getAsyncParentFrame(frame)
      : frame;
    if (!stepFrame || !stepFrame.script) {
      return null;
    }

    // Skips a frame that has been restarted.
    if (RESTARTED_FRAMES.has(stepFrame)) {
      return this._getNextStepFrame(stepFrame.older);
    }

    return stepFrame;
  }

  frames(start, count) {
    if (this.state !== STATES.PAUSED) {
      return {
        error: "wrongState",
        message:
          "Stack frames are only available while the debuggee is paused.",
      };
    }

    // Find the starting frame...
    let frame = this.youngestFrame;

    const walkToParentFrame = () => {
      if (!frame) {
        return;
      }

      const currentFrame = frame;
      frame = null;

      if (!(currentFrame instanceof Debugger.Frame)) {
        frame = getSavedFrameParent(this, currentFrame);
      } else if (currentFrame.older) {
        frame = currentFrame.older;
      } else if (
        this._options.shouldIncludeSavedFrames &&
        currentFrame.olderSavedFrame
      ) {
        frame = currentFrame.olderSavedFrame;
        if (frame && !isValidSavedFrame(this, frame)) {
          frame = null;
        }
      } else if (
        this._options.shouldIncludeAsyncLiveFrames &&
        currentFrame.asyncPromise
      ) {
        const asyncFrame = getAsyncParentFrame(currentFrame);
        if (asyncFrame) {
          frame = asyncFrame;
        }
      }
    };

    let i = 0;
    while (frame && i < start) {
      walkToParentFrame();
      i++;
    }

    // Return count frames, or all remaining frames if count is not defined.
    const frames = [];
    for (; frame && (!count || i < start + count); i++, walkToParentFrame()) {
      // SavedFrame instances don't have direct Debugger.Source object. If
      // there is an active Debugger.Source that represents the SaveFrame's
      // source, it will have already been created in the server.
      if (frame instanceof Debugger.Frame) {
        this.sourcesManager.createSourceActor(frame.script.source);
      }

      if (RESTARTED_FRAMES.has(frame)) {
        continue;
      }

      const frameActor = this._createFrameActor(frame, i);
      frames.push(frameActor);
    }

    return { frames };
  }

  addAllSources() {
    // This method aims at instantiating Source Actors for all already existing
    // sources (via `_addSource()`).
    // This is called on each new target instantiation:
    //   * when a new document or debugging context is instantiated. This
    //     method should be a no-op as there should be no pre-existing sources.
    //   * when devtools open. This time there might be pre-existing sources.
    //
    // We are using Debugger API `findSources()` for instantating source actors
    // of all still-active sources. But we want to also "resurrect" sources
    // which ran before DevTools were opened and were garbaged collected.
    // `findSources()` won't return them.
    // Debugger API `findSourceURLs()` will return the source URLs of all the
    // sources, GC-ed and still active ones.
    //
    // We are using `urlMap` to identify the GC-ed sources.
    //
    // We have two special edgecases:
    //
    // # HTML sources and inline <script> tags
    //
    // HTML sources will be specific to a given URL, but may relate to multiple
    // inline <script> tag. Each script will be related to a given Debugger API
    // source and a given DevTools Source Actor.
    // We collect all active sources in `urlMap`'s `sources` array so that we
    // only resurrect the GC-ed inline <script> and not the one which are still
    // active.
    //
    // # asm.js / wasm
    //
    // DevTools toggles Debugger API `allowUnobservedAsmJS` and
    // `allowUnobservedWasm` to false on opening. This changes how asm.js and
    // Wasm sources are compiled. But only to sources created after DevTools
    // are opened. This typically requires to reload the page.
    //
    // Before DevTools are opened, the asm.js functions are compiled into wasm
    // instances, and they are visible as "wasm" sources in `findSources()`.
    // The wasm instance doesn't keep the top-level normal JS script and the
    // corresponding JS source alive. If only the "wasm" source is found for
    // certain URL, the source needs to be re-compiled.
    //
    // Here, we should be careful to re-compile these sources the way they were
    // compiled before DevTools opening. Otherwise the re-compilation will
    // create Debugger.Script instances backed by normal JS functions for those
    // asm.js functions, which results in an inconsistency between what's
    // running in the debuggee and what's shown in DevTools.
    //
    // We are using `urlMap`'s `hasWasm` to flag them and instruct
    // `resurrectSource()` to re-compile the sources as if DevTools was off and
    // without debugging ability.
    const urlMap = {};
    for (const url of this.dbg.findSourceURLs()) {
      if (url !== "self-hosted") {
        if (!urlMap[url]) {
          urlMap[url] = { count: 0, sources: [], hasWasm: false };
        }
        urlMap[url].count++;
      }
    }

    const sources = this.dbg.findSources();

    for (const source of sources) {
      this._addSource(source);

      if (source.introductionType === "wasm") {
        const origURL = source.url.replace(/^wasm:/, "");
        if (urlMap[origURL]) {
          urlMap[origURL].hasWasm = true;
        }
      }

      // The following check should match the filtering done by `findSourceURLs`:
      // https://searchfox.org/mozilla-central/rev/ac7a567f036e1954542763f4722fbfce041fb752/js/src/debugger/Debugger.cpp#2406-2409
      // Otherwise we may populate `urlMap` incorrectly and resurrect sources that weren't GCed,
      // and spawn duplicated SourceActors/Debugger.Source for the same actual source.
      // `findSourceURLs` uses !introductionScript check as that allows to identify <script>'s
      // loaded from the HTML page. This boolean will be defined only when the <script> tag
      // is added by Javascript code at runtime.
      // https://searchfox.org/mozilla-central/rev/3d03a3ca09f03f06ef46a511446537563f62a0c6/devtools/docs/user/debugger-api/debugger.source/index.rst#113
      if (!source.introductionScript && urlMap[source.url]) {
        urlMap[source.url].count--;
        urlMap[source.url].sources.push(source);
      }
    }

    // Resurrect any URLs for which not all sources are accounted for.
    for (const [url, data] of Object.entries(urlMap)) {
      if (data.count > 0) {
        this._resurrectSource(url, data.sources, data.hasWasm);
      }
    }
  }

  sources() {
    this.addAllSources();

    // No need to flush the new source packets here, as we are sending the
    // list of sources out immediately and we don't need to invoke the
    // overhead of an RDP packet for every source right now. Let the default
    // timeout flush the buffered packets.

    const forms = [];
    for (const source of this.sourcesManager.iter()) {
      forms.push(source.form());
    }
    return forms;
  }

  /**
   * Disassociate all breakpoint actors from their scripts and clear the
   * breakpoint handlers. This method can be used when the thread actor intends
   * to keep the breakpoint store, but needs to clear any actual breakpoints,
   * e.g. due to a page navigation. This way the breakpoint actors' script
   * caches won't hold on to the Debugger.Script objects leaking memory.
   */
  disableAllBreakpoints() {
    for (const bpActor of this.breakpointActorMap.findActors()) {
      bpActor.removeScripts();
    }
  }

  removeAllBreakpoints() {
    this.breakpointActorMap.removeAllBreakpoints();
  }

  removeAllWatchpoints() {
    for (const actor of this.threadLifetimePool.poolChildren()) {
      if (actor.typeName == "obj") {
        actor.removeWatchpoints();
      }
    }
  }

  addWatchpoint(objActor, data) {
    this._watchpointsMap.add(objActor, data);
  }

  removeWatchpoint(objActor, property) {
    this._watchpointsMap.remove(objActor, property);
  }

  getWatchpoint(obj, property) {
    return this._watchpointsMap.get(obj, property);
  }

  /**
   * Handle a protocol request to pause the debuggee.
   */
  interrupt(when) {
    if (this.state == STATES.EXITED) {
      return { type: "exited" };
    } else if (this.state == STATES.PAUSED) {
      // TODO: return the actual reason for the existing pause.
      this.emit("paused", {
        why: { type: PAUSE_REASONS.ALREADY_PAUSED },
      });
      return {};
    } else if (this.state != STATES.RUNNING) {
      return {
        error: "wrongState",
        message: "Received interrupt request in " + this.state + " state.",
      };
    }
    try {
      // If execution should pause just before the next JavaScript bytecode is
      // executed, just set an onEnterFrame handler.
      if (when == "onNext") {
        const onEnterFrame = frame => {
          this._pauseAndRespond(frame, {
            type: PAUSE_REASONS.INTERRUPTED,
            onNext: true,
          });
        };
        this.dbg.onEnterFrame = onEnterFrame;
        return {};
      }

      // If execution should pause immediately, just put ourselves in the paused
      // state.
      const packet = this._paused();
      if (!packet) {
        return { error: "notInterrupted" };
      }
      packet.why = { type: PAUSE_REASONS.INTERRUPTED, onNext: false };

      // Send the response to the interrupt request now (rather than
      // returning it), because we're going to start a nested event loop
      // here.
      this.conn.send({ from: this.actorID, type: "interrupt" });
      this.emit("paused", packet);

      // Start a nested event loop.
      this._nestedEventLoop.enter();

      // We already sent a response to this request, don't send one
      // now.
      return null;
    } catch (e) {
      reportException("DBG-SERVER", e);
      return { error: "notInterrupted", message: e.toString() };
    }
  }

  _paused(frame) {
    // We don't handle nested pauses correctly.  Don't try - if we're
    // paused, just continue running whatever code triggered the pause.
    // We don't want to actually have nested pauses (although we
    // have nested event loops).  If code runs in the debuggee during
    // a pause, it should cause the actor to resume (dropping
    // pause-lifetime actors etc) and then repause when complete.

    if (this.state === STATES.PAUSED) {
      return undefined;
    }

    this._state = STATES.PAUSED;

    // Clear stepping hooks.
    this.dbg.onEnterFrame = undefined;
    this._requestedFrameRestart = null;
    this._clearSteppingHooks();

    // Create the actor pool that will hold the pause actor and its
    // children.
    assert(!this._pausePool, "No pause pool should exist yet");
    this._pausePool = new ObjectActorPool(this, "pause", true);

    // Give children of the pause pool a quick link back to the
    // thread...
    this._pausePool.threadActor = this;

    // Create the pause actor itself...
    assert(!this._pauseActor, "No pause actor should exist yet");
    this._pauseActor = new PauseActor(this._pausePool);
    this._pausePool.manage(this._pauseActor);

    // Update the list of frames.
    this._updateFrames();

    // Send off the paused packet and spin an event loop.
    const packet = {
      actor: this._pauseActor.actorID,
    };

    if (frame) {
      packet.frame = this._createFrameActor(frame);
    }

    return packet;
  }

  /**
   * Expire frame actors for frames that are no longer on the current stack.
   */
  _updateFrames() {
    // Create the actor pool that will hold the still-living frames.
    const framesPool = new Pool(this.conn, "frames");
    const frameList = [];

    for (const frameActor of this._frameActors) {
      if (frameActor.frame.onStack) {
        framesPool.manage(frameActor);
        frameList.push(frameActor);
      }
    }

    // Remove the old frame actor pool, this will expire
    // any actors that weren't added to the new pool.
    if (this._framesPool) {
      this._framesPool.destroy();
    }

    this._frameActors = frameList;
    this._framesPool = framesPool;
  }

  _createFrameActor(frame, depth) {
    let actor = this._frameActorMap.get(frame);
    if (!actor || actor.isDestroyed()) {
      actor = new FrameActor(frame, this, depth);
      this._frameActors.push(actor);
      this._framesPool.manage(actor);

      this._frameActorMap.set(frame, actor);
    }
    return actor;
  }

  /**
   * Create and return an environment actor that corresponds to the provided
   * Debugger.Environment.
   * @param Debugger.Environment environment
   *        The lexical environment we want to extract.
   * @param object pool
   *        The pool where the newly-created actor will be placed.
   * @return The EnvironmentActor for environment or undefined for host
   *         functions or functions scoped to a non-debuggee global.
   */
  createEnvironmentActor(environment, pool) {
    if (!environment) {
      return undefined;
    }

    if (environment.actor) {
      return environment.actor;
    }

    const actor = new EnvironmentActor(environment, this);
    pool.manage(actor);
    environment.actor = actor;

    return actor;
  }

  /**
   * Create a grip for the given debuggee value.
   * Depdending on if the thread is paused, the object actor may have a different lifetime:
   *  - when thread is paused, the object actor will be kept alive until the thread is resumed
   *    (which also happens when we step)
   *  - when thread is not paused, the object actor will be kept alive until the related target
   *    is destroyed (thread stops or devtools closes)
   *
   * @param value Debugger.Object|any
   *        A Debugger.Object for all JS objects, or any primitive JS type.
   * @return The value's grip
   *        Primitive JS type, Object actor Form JSON object, or a JSON object to describe the value.
   */
  createValueGrip(value) {
    // When the thread is paused, all objects are stored in a transient pool
    // which will be cleared on resume (which also happens when we step).
    const pool = this._pausePool || this.threadLifetimePool;

    return createValueGrip(this, value, pool);
  }

  _onWindowReady({ isTopLevel, isBFCache }) {
    // Note that this code relates to the disabling of Debugger API from will-navigate listener.
    // And should only be triggered when the target actor doesn't follow WindowGlobal lifecycle.
    // i.e. when the Thread Actor manages more than one top level WindowGlobal.
    if (isTopLevel && this.state != STATES.DETACHED) {
      this.sourcesManager.reset();
      this.clearDebuggees();
      this.dbg.enable();
    }

    // Refresh the debuggee list when a new window object appears (top window or
    // iframe).
    if (this.attached) {
      this.dbg.addDebuggees();
    }

    // BFCache navigations reuse old sources, so send existing sources to the
    // client instead of waiting for onNewScript debugger notifications.
    if (isBFCache) {
      this.addAllSources();
    }
  }

  _onWillNavigate({ isTopLevel }) {
    if (!isTopLevel) {
      return;
    }

    // Proceed normally only if the debuggee is not paused.
    if (this.state == STATES.PAUSED) {
      // If we were paused while navigating to a new page,
      // we resume previous page execution, so that the document can be sucessfully unloaded.
      // And we disable the Debugger API, so that we do not hit any breakpoint or trigger any
      // thread actor feature. We will re-enable it just before the next page starts loading,
      // from window-ready listener. That's for when the target doesn't follow WindowGlobal
      // lifecycle.
      // When the target follows the WindowGlobal lifecycle, we will stiff resume and disable
      // this thread actor. It will soon be destroyed. And a new target will pick up
      // the next WindowGlobal and spawn a new Debugger API, via ThreadActor.attach().
      this.doResume();
      this.dbg.disable();
    }

    this.removeAllWatchpoints();
    this.disableAllBreakpoints();
    this.dbg.onEnterFrame = undefined;
  }

  _onNavigate() {
    if (this.state == STATES.RUNNING) {
      this.dbg.enable();
    }
  }

  // JS Debugger API hooks.
  pauseForMutationBreakpoint(
    mutationType,
    targetNode,
    ancestorNode,
    action = "" // "add" or "remove"
  ) {
    if (
      !["subtreeModified", "nodeRemoved", "attributeModified"].includes(
        mutationType
      )
    ) {
      throw new Error("Unexpected mutation breakpoint type");
    }

    if (this.shouldSkipAnyBreakpoint) {
      return undefined;
    }

    const frame = this.dbg.getNewestFrame();
    if (!frame) {
      return undefined;
    }

    if (this.sourcesManager.isFrameBlackBoxed(frame)) {
      return undefined;
    }

    const global = (targetNode.ownerDocument || targetNode).defaultView;
    assert(global && this.dbg.hasDebuggee(global));

    const targetObj = this.dbg
      .makeGlobalObjectReference(global)
      .makeDebuggeeValue(targetNode);

    let ancestorObj = null;
    if (ancestorNode) {
      ancestorObj = this.dbg
        .makeGlobalObjectReference(global)
        .makeDebuggeeValue(ancestorNode);
    }

    return this._pauseAndRespond(
      frame,
      {
        type: PAUSE_REASONS.MUTATION_BREAKPOINT,
        mutationType,
        message: `DOM Mutation: '${mutationType}'`,
      },
      pkt => {
        // We have to create the object actors late, from here because `_pausePool` is `null` beforehand,
        // and the actors created by createValueGrip would otherwise be registered in the thread lifetime pool
        pkt.why.nodeGrip = this.createValueGrip(targetObj);
        pkt.why.ancestorGrip = ancestorObj
          ? this.createValueGrip(ancestorObj)
          : null;
        pkt.why.action = action;
        return pkt;
      }
    );
  }

  /**
   * A function that the engine calls when a debugger statement has been
   * executed in the specified frame.
   *
   * @param frame Debugger.Frame
   *        The stack frame that contained the debugger statement.
   */
  onDebuggerStatement(frame) {
    // Don't pause if:
    // 1. breakpoints are disabled
    // 2. we have not moved since the last pause
    // 3. the source is blackboxed
    // 4. there is a breakpoint at the same location
    if (
      this.shouldSkipAnyBreakpoint ||
      !this.hasMoved(frame, "debuggerStatement") ||
      this.sourcesManager.isFrameBlackBoxed(frame) ||
      this.atBreakpointLocation(frame)
    ) {
      return undefined;
    }

    return this._pauseAndRespond(frame, {
      type: PAUSE_REASONS.DEBUGGER_STATEMENT,
    });
  }

  skipBreakpoints(skip) {
    this._options.skipBreakpoints = skip;
    return { skip };
  }

  // Bug 1686485 is meant to remove usages of this request
  // in favor direct call to `reconfigure`
  pauseOnExceptions(pauseOnExceptions, ignoreCaughtExceptions) {
    this.reconfigure({
      pauseOnExceptions,
      ignoreCaughtExceptions,
    });
    return {};
  }

  /**
   * A function that the engine calls when an exception has been thrown and has
   * propagated to the specified frame.
   *
   * @param youngestFrame Debugger.Frame
   *        The youngest remaining stack frame.
   * @param value object
   *        The exception that was thrown.
   */
  _onExceptionUnwind(youngestFrame, value) {
    // Ignore any reported exception if we are already paused
    if (this.isPaused()) {
      return undefined;
    }

    // Ignore shouldSkipAnyBreakpoint if we are explicitly requested to do so.
    // Typically, when we are evaluating conditional breakpoints, we want to report any exception.
    if (
      this.shouldSkipAnyBreakpoint &&
      !this.insideClientEvaluation?.reportExceptionsWhenBreaksAreDisabled
    ) {
      return undefined;
    }

    let willBeCaught = false;
    for (let frame = youngestFrame; frame != null; frame = frame.older) {
      if (frame.script.isInCatchScope(frame.offset)) {
        willBeCaught = true;
        break;
      }
    }

    if (willBeCaught && this._options.ignoreCaughtExceptions) {
      return undefined;
    }

    if (
      this._handledFrameExceptions.has(youngestFrame) &&
      this._handledFrameExceptions.get(youngestFrame) === value
    ) {
      return undefined;
    }

    // NS_ERROR_NO_INTERFACE exceptions are a special case in browser code,
    // since they're almost always thrown by QueryInterface functions, and
    // handled cleanly by native code.
    if (!isWorker && value == Cr.NS_ERROR_NO_INTERFACE) {
      return undefined;
    }

    // Don't pause on exceptions thrown while inside an evaluation being done on
    // behalf of the client.
    if (this.insideClientEvaluation) {
      return undefined;
    }

    if (this.sourcesManager.isFrameBlackBoxed(youngestFrame)) {
      return undefined;
    }

    // Now that we've decided to pause, ignore this exception if it's thrown by
    // any older frames.
    for (let frame = youngestFrame.older; frame != null; frame = frame.older) {
      this._handledFrameExceptions.set(frame, value);
    }

    try {
      const packet = this._paused(youngestFrame);
      if (!packet) {
        return undefined;
      }

      packet.why = {
        type: PAUSE_REASONS.EXCEPTION,
        exception: this.createValueGrip(value),
      };
      this.emit("paused", packet);

      this._nestedEventLoop.enter();
    } catch (e) {
      reportException("TA_onExceptionUnwind", e);
    }

    return undefined;
  }

  /**
   * A function that the engine calls when a new script has been loaded.
   *
   * @param script Debugger.Script
   *        The source script that has been loaded into a debuggee compartment.
   */
  onNewScript(script) {
    this._addSource(script.source);

    this._maybeTrackFirstStatementBreakpoint(script);
  }

  /**
   * A function called when there's a new source from a thread actor's sources.
   * Emits `newSource` on the thread actor.
   *
   * @param {SourceActor} source
   */
  onNewSourceEvent(source) {
    // When this target is supported by the Watcher Actor,
    // and we listen to SOURCE, we avoid emitting the newSource RDP event
    // as it would be duplicated with the Resource/watchResources API.
    // Could probably be removed once bug 1680280 is fixed.
    if (!this._shouldEmitNewSource) {
      return;
    }

    // Bug 1516197: New sources are likely detected due to either user
    // interaction on the page, or devtools requests sent to the server.
    // We use executeSoon because we don't want to block those operations
    // by sending packets in the middle of them.
    DevToolsUtils.executeSoon(() => {
      if (this.isDestroyed()) {
        return;
      }
      this.emit("newSource", {
        source: source.form(),
      });
    });
  }

  // API used by the Watcher Actor to disable the newSource events
  // Could probably be removed once bug 1680280 is fixed.
  _shouldEmitNewSource = true;
  disableNewSourceEvents() {
    this._shouldEmitNewSource = false;
  }

  /**
   * Filtering function to filter out sources for which we don't want to notify/create
   * source actors
   *
   * @param {Debugger.Source} source
   *        The source to accept or ignore
   * @param Boolean
   *        True, if we want to create a source actor.
   */
  _acceptSource(source) {
    // We have some spurious source created by ExtensionContent.sys.mjs when debugging tabs.
    // These sources are internal stuff injected by WebExt codebase to implement content
    // scripts. We can't easily ignore them from Debugger API, so ignore them
    // when debugging a tab (i.e. browser-element). As we still want to debug them
    // from the browser toolbox.
    if (
      this.targetActor.sessionContext.type == "browser-element" &&
      source.url.endsWith("ExtensionContent.sys.mjs")
    ) {
      return false;
    }

    return true;
  }

  /**
   * Add the provided source to the server cache.
   *
   * @param aSource Debugger.Source
   *        The source that will be stored.
   */
  _addSource(source) {
    if (!this._acceptSource(source)) {
      return;
    }

    // Preloaded WebExtension content scripts may be cached internally by
    // ExtensionContent.jsm and ThreadActor would ignore them on a page reload
    // because it finds them in the _debuggerSourcesSeen WeakSet,
    // and so we also need to be sure that there is still a source actor for the source.
    let sourceActor;
    if (
      this._debuggerSourcesSeen.has(source) &&
      this.sourcesManager.hasSourceActor(source)
    ) {
      sourceActor = this.sourcesManager.getSourceActor(source);
      sourceActor.resetDebuggeeScripts();
    } else {
      sourceActor = this.sourcesManager.createSourceActor(source);
    }

    const sourceUrl = sourceActor.url;
    if (this._onLoadBreakpointURLs.has(sourceUrl)) {
      // Immediately set a breakpoint on first line
      // (note that this is only used by `./mach xpcshell-test --jsdebugger`)
      this.setBreakpoint({ sourceUrl, line: 1 }, {});
      // But also query asynchronously the first really breakable line
      // as the first may not be valid and won't break.
      (async () => {
        const [firstLine] = await sourceActor.getBreakableLines();
        if (firstLine != 1) {
          this.setBreakpoint({ sourceUrl, line: firstLine }, {});
        }
      })();
    }

    const bpActors = this.breakpointActorMap
      .findActors()
      .filter(
        actor =>
          actor.location.sourceUrl && actor.location.sourceUrl == sourceUrl
      );

    for (const actor of bpActors) {
      sourceActor.applyBreakpoint(actor);
    }

    this._debuggerSourcesSeen.add(source);
  }

  /**
   * Create a new source by refetching the specified URL and instantiating all
   * sources that were found in the result.
   *
   * @param url The URL string to fetch.
   * @param existingInlineSources The inline sources for the URL the debugger knows about
   *                              already, and that we shouldn't re-create (only used when
   *                              url content type is text/html).
   * @param forceEnableAsmJS A boolean to force enable the asm.js feature.
   *                         See the comment inside addAllSources for more
   *                         details.
   */
  async _resurrectSource(url, existingInlineSources, forceEnableAsmJS) {
    let { content, contentType, sourceMapURL } =
      await this.sourcesManager.urlContents(
        url,
        /* partial */ false,
        /* canUseCache */ true
      );

    // Newlines in all sources should be normalized. Do this with HTML content
    // to simplify the comparisons below.
    content = content.replace(/\r\n?|\u2028|\u2029/g, "\n");

    if (contentType == "text/html") {
      // HTML files can contain any number of inline sources. We have to find
      // all the inline sources and their start line without running any of the
      // scripts on the page. The approach used here is approximate.
      if (!this.targetActor.window) {
        return;
      }

      // Find the offsets in the HTML at which inline scripts might start.
      const scriptTagMatches = content.matchAll(/<script[^>]*>/gi);
      const scriptStartOffsets = [...scriptTagMatches].map(
        rv => rv.index + rv[0].length
      );

      // Find the script tags in this HTML page by parsing a new document from
      // the contentand looking for its script elements.
      const document = new DOMParser().parseFromString(content, "text/html");

      // For each inline source found, see if there is a start offset for what
      // appears to be a script tag, whose contents match the inline source.
      [...document.scripts].forEach(script => {
        const text = script.innerText;

        // We only want to handle inline scripts
        if (script.src) {
          return;
        }

        // Don't create source for empty script tag
        if (!text.trim()) {
          return;
        }

        const scriptStartOffsetIndex = scriptStartOffsets.findIndex(
          offset => content.substring(offset, offset + text.length) == text
        );
        // Bail if we couldn't find the start offset for the script
        if (scriptStartOffsetIndex == -1) {
          return;
        }

        const scriptStartOffset = scriptStartOffsets[scriptStartOffsetIndex];
        // Remove the offset from the array to mitigate any issue we might with scripts
        // sharing the same text content.
        scriptStartOffsets.splice(scriptStartOffsetIndex, 1);

        const allLineBreaks = [
          ...content.substring(0, scriptStartOffset).matchAll("\n"),
        ];
        const startLine = 1 + allLineBreaks.length;
        // NOTE: Debugger.Source.prototype.startColumn is 1-based.
        //       Create 1-based column here for the following comparison,
        //       and also the createSource call below.
        const startColumn =
          1 +
          scriptStartOffset -
          (allLineBreaks.length ? allLineBreaks.at(-1).index - 1 : 0);

        // Don't create a source if we already found one for this script
        if (
          existingInlineSources.find(
            source =>
              source.startLine == startLine && source.startColumn == startColumn
          )
        ) {
          return;
        }

        try {
          const global = this.dbg.getDebuggees()[0];
          // NOTE: Debugger.Object.prototype.createSource takes 1-based column.
          this._addSource(
            global.createSource({
              text,
              url,
              startLine,
              startColumn,
              isScriptElement: true,
              forceEnableAsmJS,
            })
          );
        } catch (e) {
          //  Ignore parse errors.
        }
      });

      // If no scripts were found, we might have an inaccurate content type and
      // the file is actually JavaScript. Fall through and add the entire file
      // as the source.
      if (document.scripts.length) {
        return;
      }
    }

    // Other files should only contain javascript, so add the file contents as
    // the source itself.
    try {
      const global = this.dbg.getDebuggees()[0];
      this._addSource(
        global.createSource({
          text: content,
          url,
          startLine: 1,
          sourceMapURL,
          forceEnableAsmJS,
        })
      );
    } catch (e) {
      // Ignore parse errors.
    }
  }

  dumpThread() {
    return {
      pauseOnExceptions: this._options.pauseOnExceptions,
      ignoreCaughtExceptions: this._options.ignoreCaughtExceptions,
      logEventBreakpoints: this._options.logEventBreakpoints,
      skipBreakpoints: this.shouldSkipAnyBreakpoint,
      breakpoints: this.breakpointActorMap.listKeys(),
    };
  }

  // NOTE: dumpPools is defined in the Thread actor to avoid
  // adding it to multiple target specs and actors.
  dumpPools() {
    return this.conn.dumpPools();
  }

  logLocation(prefix, frame) {
    const loc = this.sourcesManager.getFrameLocation(frame);
    dump(`${prefix} (${loc.line}, ${loc.column})\n`);
  }
}

exports.ThreadActor = ThreadActor;

/**
 * Creates a PauseActor.
 *
 * PauseActors exist for the lifetime of a given debuggee pause.  Used to
 * scope pause-lifetime grips.
 *
 * @param {Pool} pool: The actor pool created for this pause.
 */
function PauseActor(pool) {
  this.pool = pool;
}

PauseActor.prototype = {
  typeName: "pause",
};

// Utility functions.

/**
 * Unwrap a global that is wrapped in a |Debugger.Object|, or if the global has
 * become a dead object, return |undefined|.
 *
 * @param Debugger.Object wrappedGlobal
 *        The |Debugger.Object| which wraps a global.
 *
 * @returns {Object|undefined}
 *          Returns the unwrapped global object or |undefined| if unwrapping
 *          failed.
 */
exports.unwrapDebuggerObjectGlobal = wrappedGlobal => {
  try {
    // Because of bug 991399 we sometimes get nuked window references here. We
    // just bail out in that case.
    //
    // Note that addon sandboxes have a DOMWindow as their prototype. So make
    // sure that we can touch the prototype too (whatever it is), in case _it_
    // is it a nuked window reference. We force stringification to make sure
    // that any dead object proxies make themselves known.
    const global = wrappedGlobal.unsafeDereference();
    Object.getPrototypeOf(global) + "";
    return global;
  } catch (e) {
    return undefined;
  }
};
