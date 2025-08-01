"use strict";

const server = createHttpServer({ hosts: ["example.com", "example.net"] });

// All tests in this file run user scripts at document_start that populates
// window.resultCollector. If no user script runs, window.resultCollector
// will be set to "NotRun" by the web page itself.
//
// runAt: "document_start" is chosen to ensure that the scripts run before the
// web page, and to ensure that all user scripts have run before the page load
// is completed - see collectResults.
//
// Note: test coverage for execution at document_end is provided by
// toolkit/components/extensions/test/xpcshell/test_ext_userScripts_mv3_csp.js.
server.registerPathHandler("/resultCollector", (request, response) => {
  response.setStatusLine(request.httpVersion, 200, "OK");
  // main world scripts will define resultCollector at document_start and append
  // to it. The page's scripts should run after user script's document_start,
  // and therefore we should not see NotRun, unless no scripts ran.
  response.write(`<script>globalThis.resultCollector ??= "NotRun";</script>`);
});

AddonTestUtils.init(this);
AddonTestUtils.overrideCertDB();

const { ExtensionPermissions } = ChromeUtils.importESModule(
  "resource://gre/modules/ExtensionPermissions.sys.mjs"
);

async function grantUserScriptsPermission(extensionId) {
  // userScripts is optional-only, and we must grant it. See comment at
  // grantUserScriptsPermission in test_ext_userScripts_mv3_availability.js.
  await ExtensionPermissions.add(extensionId, {
    permissions: ["userScripts"],
    origins: [],
  });
}

async function collectResults(contentPage) {
  return contentPage.spawn([], () => {
    // The results from user scripts, or the web page (if no user scripts
    // executed). The user scripts all run at document_start, which blocks the
    // load completion until user script execution completes. This ensures that
    // when we get here, that all user scripts (if any) have completed running.
    return this.content.wrappedJSObject.resultCollector;
  });
}

add_setup(async () => {
  await ExtensionTestUtils.startAddonManager();
});

// Tests that user scripts can run in the MAIN world, and only for new document
// loads (not existing ones).
add_task(async function userScript_runs_in_MAIN_world() {
  const extensionId = "@userScript_runs_in_MAIN_world";
  await grantUserScriptsPermission(extensionId);
  let extension = ExtensionTestUtils.loadExtension({
    useAddonManager: "permanent",
    manifest: {
      browser_specific_settings: { gecko: { id: extensionId } },
      manifest_version: 3,
      optional_permissions: ["userScripts"],
      host_permissions: ["*://example.com/*"],
    },
    files: {
      "3.file.js": "resultCollector.push('3.file');dump('3.file.js ran\\n');",
      "6.file.js": "resultCollector.push('6.file');dump('6.file.js ran\\n');",
    },
    async background() {
      browser.test.onMessage.addListener(async msg => {
        browser.test.assertEq("revoke_permission", msg, "Expected msg");
        await browser.permissions.remove({ permissions: ["userScripts"] });
        browser.test.assertEq(undefined, browser.userScripts, "API gone");
        browser.test.sendMessage("revoke_permission:done");
      });
      await browser.userScripts.register([
        {
          id: "basic",
          matches: ["*://example.com/resultCollector"],
          js: [
            { code: "window.resultCollector ??= [];" },
            { code: "resultCollector.push('1.code');dump('1.code ran\\n');" },
            { code: "resultCollector.push('2.code');dump('2.code ran\\n');" },
            { file: "3.file.js" },
            { code: "resultCollector.push('4.code');dump('4.code ran\\n');" },
            { code: "resultCollector.push('5.code');dump('5.code ran\\n');" },
            { file: "6.file.js" },
          ],
          runAt: "document_start",
          world: "MAIN",
        },
      ]);
      browser.test.sendMessage("registered");
    },
  });

  let contentPageBeforeExtStarted = await ExtensionTestUtils.loadContentPage(
    "http://example.com/resultCollector"
  );

  await extension.startup();
  await extension.awaitMessage("registered");

  let contentPageAfterRegister = await ExtensionTestUtils.loadContentPage(
    "http://example.com/resultCollector"
  );
  Assert.deepEqual(
    await collectResults(contentPageAfterRegister),
    ["1.code", "2.code", "3.file", "4.code", "5.code", "6.file"],
    "All MAIN world scripts executed in a new page after registration"
  );
  Assert.deepEqual(
    await collectResults(contentPageBeforeExtStarted),
    "NotRun",
    "User scripts did not execute in content that existed before registration"
  );

  await contentPageAfterRegister.close();
  await contentPageBeforeExtStarted.close();

  // Verify that when the "userScripts" permission is revoked, that scripts
  // won't be injected in new documents.
  extension.sendMessage("revoke_permission");
  await extension.awaitMessage("revoke_permission:done");
  let contentPageAfterRevoke = await ExtensionTestUtils.loadContentPage(
    "http://example.com/resultCollector"
  );
  Assert.deepEqual(
    await collectResults(contentPageAfterRevoke),
    "NotRun",
    "Should not execute after permission revocation"
  );
  await contentPageAfterRevoke.close();

  await extension.unload();
});

add_task(async function userScript_require_host_permissions() {
  const extensionId = "@userScript_require_host_permissions";
  await grantUserScriptsPermission(extensionId);
  let extension = ExtensionTestUtils.loadExtension({
    useAddonManager: "permanent",
    manifest: {
      browser_specific_settings: { gecko: { id: extensionId } },
      manifest_version: 3,
      optional_permissions: ["userScripts"],
      host_permissions: ["*://example.net/*"],
    },
    async background() {
      await browser.userScripts.register([
        {
          id: "basic",
          matches: ["*://*/resultCollector"],
          js: [
            {
              code: `window.resultCollector ??= [];
                     resultCollector.push(origin);`,
            },
          ],
          runAt: "document_start",
          world: "MAIN",
        },
        {
          // For extra coverage use includeGlobs without matches, since most
          // other tests do use "matches". The underlying userScripts-specific
          // matches+includeGlobs matching logic is extensively covered by
          // test_WebExtensionContentScript_isUserScript in
          // test_WebExtensionContentScript.js.
          id: "includeGlobs without matches",
          includeGlobs: ["*resultCollector"],
          js: [{ code: "resultCollector.push(origin)" }],
          runAt: "document_start",
          world: "MAIN",
        },
      ]);
      browser.test.sendMessage("registered");
    },
  });

  await extension.startup();
  await extension.awaitMessage("registered");

  {
    let contentPage = await ExtensionTestUtils.loadContentPage(
      "http://example.net/resultCollector"
    );
    Assert.deepEqual(
      await collectResults(contentPage),
      ["http://example.net", "http://example.net"],
      "Can execute with host permissions"
    );
    await contentPage.close();
  }

  {
    let contentPage = await ExtensionTestUtils.loadContentPage(
      "http://example.com/resultCollector"
    );
    Assert.deepEqual(
      await collectResults(contentPage),
      "NotRun",
      "Cannot execute without host permissions"
    );
    await contentPage.close();
  }

  await extension.unload();
});

// Tests that user scripts can run in the USER_SCRIPT world, and only for new
// document loads (not existing ones).
add_task(async function userScript_runs_in_USER_SCRIPT_world() {
  const extensionId = "@userScript_runs_in_USER_SCRIPT_world";
  await grantUserScriptsPermission(extensionId);
  let extension = ExtensionTestUtils.loadExtension({
    useAddonManager: "permanent",
    manifest: {
      browser_specific_settings: { gecko: { id: extensionId } },
      manifest_version: 3,
      optional_permissions: ["userScripts"],
      host_permissions: ["*://example.com/*"],
    },
    files: {
      "1.file.js": `
        // window.wrappedJSObject will be undefined if we unexpectedly run
        // in the MAIN world instead of the "USER_SCRIPT" world.
        window.wrappedJSObject.resultCollector ??= cloneInto([], window);
        var resultCollector_push = msg => {
          window.wrappedJSObject.resultCollector.push(msg);
        };
        resultCollector_push("1.file");dump("1.file.js ran\\n");
      `,
      "3.file.js": "resultCollector_push('3.file');dump('3.file.js ran\\n');",
    },
    async background() {
      async function register() {
        await browser.userScripts.register([
          {
            id: "userScripts ID (for register and update)",
            matches: ["*://example.com/resultCollector"],
            js: [
              { file: "1.file.js" },
              // 1.file.js defines the "resultCollector_push" function, and that
              // function should be available to the other scripts since they all
              // run in the same USER_SCRIPT world.
              { code: "resultCollector_push('2.code');dump('1.code ran\\n');" },
              { file: "3.file.js" },
              { code: "resultCollector_push('4.code');dump('4.code ran\\n');" },
            ],
            runAt: "document_start",
            world: "USER_SCRIPT",
          },
        ]);
      }
      async function update() {
        await browser.userScripts.update([
          {
            id: "userScripts ID (for register and update)",
            js: [
              { file: "1.file.js" },
              { code: "resultCollector_push('2.updated');" },
            ],
          },
        ]);
      }

      browser.test.onMessage.addListener(async msg => {
        browser.test.assertEq("update_userScripts", msg, "Expected msg");
        await update();
        browser.test.sendMessage("update_userScripts:done");
      });
      await register();
      browser.test.sendMessage("registered");
    },
  });

  let contentPageBeforeExtStarted = await ExtensionTestUtils.loadContentPage(
    "http://example.com/resultCollector"
  );

  await extension.startup();
  await extension.awaitMessage("registered");

  let contentPageAfterRegister = await ExtensionTestUtils.loadContentPage(
    "http://example.com/resultCollector"
  );
  Assert.deepEqual(
    await collectResults(contentPageAfterRegister),
    ["1.file", "2.code", "3.file", "4.code"],
    "All USER_SCRIPT world scripts executed in a new page after registration"
  );
  Assert.deepEqual(
    await collectResults(contentPageBeforeExtStarted),
    "NotRun",
    "User scripts did not execute in content that existed before registration"
  );

  // Now call userScripts.update() and check whether it injects.
  extension.sendMessage("update_userScripts");
  await extension.awaitMessage("update_userScripts:done");

  // Reload - this is a new load after the userScripts.update() call.
  await contentPageAfterRegister.loadURL("http://example.com/resultCollector");
  Assert.deepEqual(
    await collectResults(contentPageAfterRegister),
    ["1.file", "2.updated"],
    "userScripts.update() applies new scripts to new documents"
  );

  Assert.deepEqual(
    await collectResults(contentPageBeforeExtStarted),
    "NotRun",
    "userScripts.update() does not run code in existing documents"
  );

  await contentPageAfterRegister.close();
  await contentPageBeforeExtStarted.close();

  await extension.unload();
});
