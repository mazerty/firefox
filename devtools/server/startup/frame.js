/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* eslint-env mozilla/frame-script */

"use strict";

/* global addEventListener */

/*
 * Frame script that listens for requests to start a `DevToolsServer` for a frame in a
 * content process.  Loaded into content process frames by the main process during
 * frame-connector.js' connectToFrame.
 */

try {
  var chromeGlobal = this;

  // Encapsulate in its own scope to allows loading this frame script more than once.
  (function () {
    // In most cases, we are debugging a tab in content process, without chrome
    // privileges. But in some tests, we are attaching to privileged document.
    // Because the debugger can't be running in the same compartment than its debuggee,
    // we have to load the server in a dedicated Loader, loading modules in a distinct compartment.
    // That's what DistinctSystemPrincipalLoader does.
    let loader,
      customLoader = false;
    if (content.document.nodePrincipal.isSystemPrincipal) {
      const { useDistinctSystemPrincipalLoader } = ChromeUtils.importESModule(
        "resource://devtools/shared/loader/DistinctSystemPrincipalLoader.sys.mjs",
        { global: "shared" }
      );
      loader = useDistinctSystemPrincipalLoader(chromeGlobal);
      customLoader = true;
    } else {
      // Otherwise, use the shared loader.
      loader = ChromeUtils.importESModule(
        "resource://devtools/shared/loader/Loader.sys.mjs"
      );
    }
    const { require } = loader;

    const DevToolsUtils = require("resource://devtools/shared/DevToolsUtils.js");
    const {
      DevToolsServer,
    } = require("resource://devtools/server/devtools-server.js");

    DevToolsServer.init();
    // We want a special server without any root actor and only target-scoped actors.
    // We are going to spawn a WindowGlobalTargetActor instance in the next few lines,
    // it is going to act like a root actor without being one.
    DevToolsServer.registerActors({ target: true });

    const connections = new Map();

    const onConnect = DevToolsUtils.makeInfallible(function (msg) {
      const mm = msg.target;
      const prefix = msg.data.prefix;

      // If we try to create several frame targets simultaneously, the frame script will be loaded several times.
      // In this case a single "debug:connect" message might be received by all the already loaded frame scripts.
      // Check if the DevToolsServer already knows the provided connection prefix,
      // because it means that another framescript instance already handled this message.
      // Another "debug:connect" message is guaranteed to be emitted for another prefix,
      // so we keep the message listener and wait for this next message.
      if (DevToolsServer.hasConnectionForPrefix(prefix)) {
        return;
      }
      removeMessageListener("debug:connect", onConnect);

      const conn = DevToolsServer.connectToParent(prefix, mm);
      connections.set(prefix, conn);

      const {
        WindowGlobalTargetActor,
      } = require("resource://devtools/server/actors/targets/window-global.js");
      const {
        createBrowserElementSessionContext,
      } = require("resource://devtools/server/actors/watcher/session-context.js");

      const { docShell } = chromeGlobal;
      // For a script loaded via loadFrameScript, the global is the content
      // message manager.
      // All WindowGlobalTarget actors created via the framescript are top-level
      // targets. Non top-level WindowGlobalTarget actors are all created by the
      // DevToolsFrameChild actor.
      //
      // createBrowserElementSessionContext only reads browserId attribute
      const fakeBrowserElement = {
        browserId: docShell.browsingContext.browserId,
      };
      const actor = new WindowGlobalTargetActor(conn, {
        docShell,
        ignoreSubFrames: false,
        isTopLevelTarget: true,
        // This is only used when server target switching is off and we create
        // the target from TabDescriptor. So all config attributes are false.
        sessionContext: createBrowserElementSessionContext(
          fakeBrowserElement,
          {}
        ),
      });
      actor.manage(actor);

      sendAsyncMessage("debug:actor", { actor: actor.form(), prefix });
    });

    addMessageListener("debug:connect", onConnect);

    const onDisconnect = DevToolsUtils.makeInfallible(function (msg) {
      const prefix = msg.data.prefix;
      const conn = connections.get(prefix);
      if (!conn) {
        // Several copies of this frame script can be running for a single frame since it
        // is loaded once for each DevTools connection to the frame.  If this disconnect
        // request doesn't match a connection known here, ignore it.
        return;
      }

      removeMessageListener("debug:disconnect", onDisconnect);
      // Call DevToolsServerConnection.close to destroy all child actors. It should end up
      // calling DevToolsServerConnection.onTransportClosed that would actually cleanup all actor
      // pools.
      conn.close();
      connections.delete(prefix);
    });
    addMessageListener("debug:disconnect", onDisconnect);

    // In non-e10s mode, the "debug:disconnect" message isn't always received before the
    // messageManager connection goes away.  Watching for "unload" here ensures we close
    // any connections when the frame is unloaded.
    addEventListener("unload", () => {
      for (const conn of connections.values()) {
        conn.close();
      }
      connections.clear();
    });

    // Destroy the server once its last connection closes. Note that multiple frame
    // scripts may be running in parallel and reuse the same server.
    function destroyLoader() {
      // Only destroy the server if there is no more connections to it. It may be used
      // to debug another tab running in the same process.
      if (DevToolsServer.hasConnection() || DevToolsServer.keepAlive) {
        return;
      }
      DevToolsServer.off("connectionchange", destroyLoader);

      // When debugging chrome pages, we initialized a dedicated loader, also destroy it
      if (customLoader) {
        const { releaseDistinctSystemPrincipalLoader } =
          ChromeUtils.importESModule(
            "resource://devtools/shared/loader/DistinctSystemPrincipalLoader.sys.mjs",
            { global: "shared" }
          );
        releaseDistinctSystemPrincipalLoader(chromeGlobal);
      }
    }
    DevToolsServer.on("connectionchange", destroyLoader);
  })();
} catch (e) {
  dump(`Exception in DevTools frame startup: ${e}\n`);
}
