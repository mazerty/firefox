/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

ChromeUtils.defineESModuleGetters(this, {
  actionTypes: "resource://newtab/common/Actions.mjs",
  TimerFeed: "resource://newtab/lib/Widgets/TimerFeed.sys.mjs",
  sinon: "resource://testing-common/Sinon.sys.mjs",
});

const PREF_TIMER_ENABLED = "widgets.timer.enabled";
const PREF_SYSTEM_TIMER_ENABLED = "widgets.system.timer.enabled";
const PREF_TIMER_SHOW_NOTIFICATIONS =
  "widgets.focusTimer.showSystemNotifications";

add_task(async function test_construction() {
  let feed = new TimerFeed();

  feed.store = {
    getState() {
      return this.state;
    },
    dispatch: sinon.spy(),
    state: {
      Prefs: {
        values: {
          [PREF_SYSTEM_TIMER_ENABLED]: false,
        },
      },
    },
  };

  info("TimerFeed constructor should create initial values");

  Assert.ok(feed, "Could construct a TimerFeed");
  Assert.ok(!feed.loaded, "TimerFeed is not loaded");
  Assert.ok(!feed.enabled);
});

add_task(async function test_onAction_INIT() {
  let feed = new TimerFeed();

  feed.store = {
    getState() {
      return this.state;
    },
    dispatch: sinon.spy(),
    state: {
      Prefs: {
        values: {
          [PREF_TIMER_ENABLED]: true,
          [PREF_SYSTEM_TIMER_ENABLED]: true,
        },
      },
    },
  };

  info("TimerFeed.onAction INIT should set initialized");

  await feed.onAction({
    type: actionTypes.INIT,
  });

  Assert.ok(feed.initialized);
});

add_task(async function test_isEnabled() {
  let feed = new TimerFeed();

  feed.store = {
    getState() {
      return this.state;
    },
    dispatch: sinon.spy(),
    state: {
      Prefs: {
        values: {
          [PREF_TIMER_ENABLED]: true,
          [PREF_SYSTEM_TIMER_ENABLED]: true,
        },
      },
    },
  };

  info("TimerFeed should be enabled");
  Assert.ok(feed.enabled);
});

add_task(async function test_system_notification_on_timer_end() {
  const feed = new TimerFeed();

  // TODO: This will change as actual strings are finalized
  const TIMER_END_NOTIFICATION_TITLE = "Timer Finished";
  const TIMER_END_NOTIFICATION_TEXT = "Time's up! Take a break.";

  feed.store = {
    getState() {
      return this.state;
    },
    dispatch: sinon.spy(),
    state: {
      Prefs: {
        values: {
          [PREF_TIMER_ENABLED]: true,
          [PREF_SYSTEM_TIMER_ENABLED]: true,
          [PREF_TIMER_SHOW_NOTIFICATIONS]: true,
        },
      },
      TimerWidget: {},
    },
  };

  feed.showSystemNotification = sinon.spy();

  await feed.onAction({
    type: actionTypes.WIDGETS_TIMER_END,
    data: { duration: 1500 },
  });

  Assert.ok(
    feed.showSystemNotification.calledOnce,
    "TimerFeed WIDGETS_TIMER_END event should show notification"
  );

  const [title, text] = feed.showSystemNotification.firstCall.args;
  Assert.equal(title, TIMER_END_NOTIFICATION_TITLE, "Title is correct");
  Assert.equal(text, TIMER_END_NOTIFICATION_TEXT, "Text is correct");
});
