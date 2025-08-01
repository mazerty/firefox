/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

"use strict";

const { AddonTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/AddonTestUtils.sys.mjs"
);

const { EnterprisePolicyTesting } = ChromeUtils.importESModule(
  "resource://testing-common/EnterprisePolicyTesting.sys.mjs"
);

const TESTPATH = "webapi_checkavailable.html";
const TESTPAGE = `${SECURE_TESTROOT}${TESTPATH}`;
const XPI_URL = `${SECURE_TESTROOT}../xpinstall/amosigned.xpi`;
const XPI_ADDON_ID = "amosigned-xpi@tests.mozilla.org";

const ID = "amosigned-xpi@tests.mozilla.org";
// Actual XPI file size and hash are computed in the add_setup callback.
let XPI_LEN = -1;
let XPI_SHA = null;
let XPI_INVALID_SHA =
  "sha256:0000000000000000000000000000000000000000000000000000000000000000";

AddonTestUtils.initMochitest(this);

function waitForClear() {
  const MSG = "WebAPICleanup";
  return new Promise(resolve => {
    let listener = {
      receiveMessage(msg) {
        if (msg.name == MSG) {
          Services.mm.removeMessageListener(MSG, listener);
          resolve();
        }
      },
    };

    Services.mm.addMessageListener(MSG, listener, true);
  });
}

add_setup(async function () {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["extensions.webapi.testing", true],
      ["extensions.install.requireBuiltInCerts", false],
    ],
  });
  info("added preferences");

  // Get the file size (used in this test file to assert the
  // expected maxProgress value set in the addon download
  // dialog).
  const xpiFilePath = getTestFilePath("../xpinstall/amosigned.xpi");
  const xpiStat = await IOUtils.stat(xpiFilePath);
  XPI_LEN = xpiStat.size;

  // Compute the file hash.
  const xpiFileHash = await IOUtils.computeHexDigest(xpiFilePath, "sha256");
  XPI_SHA = `sha256:${xpiFileHash}`;
});

// Wrapper around a common task to run in the content process to test
// the mozAddonManager API.  Takes a URL for the XPI to install and an
// array of steps, each of which can either be an action to take
// (i.e., start or cancel the install) or an install event to wait for.
// Steps that look for a specific event may also include a "props" property
// with properties that the AddonInstall object is expected to have when
// that event is triggered.
async function testInstall(browser, args, steps, description) {
  let success = await SpecialPowers.spawn(
    browser,
    [{ args, steps }],
    async function (opts) {
      let { args, steps } = opts;
      let install = await content.navigator.mozAddonManager.createInstall(args);
      if (!install) {
        await Promise.reject(
          "createInstall() did not return an install object"
        );
      }

      // Check that the initial state of the AddonInstall is sane.
      if (install.state != "STATE_AVAILABLE") {
        await Promise.reject("new install should be in STATE_AVAILABLE");
      }
      if (install.error != null) {
        await Promise.reject("new install should have null error");
      }

      const events = [
        "onDownloadStarted",
        "onDownloadProgress",
        "onDownloadEnded",
        "onDownloadCancelled",
        "onDownloadFailed",
        "onInstallStarted",
        "onInstallEnded",
        "onInstallCancelled",
        "onInstallFailed",
      ];
      let eventWaiter = null;
      let receivedEvents = [];
      let prevEvent = null;
      events.forEach(event => {
        install.addEventListener(event, () => {
          receivedEvents.push({
            event,
            state: install.state,
            error: install.error,
            progress: install.progress,
            maxProgress: install.maxProgress,
          });
          if (eventWaiter) {
            eventWaiter();
          }
        });
      });

      // Returns a promise that is resolved when the given event occurs
      // or rejects if a different event comes first or if props is supplied
      // and properties on the AddonInstall don't match those in props.
      function expectEvent(event, props) {
        return new Promise((resolve, reject) => {
          function check() {
            let received = receivedEvents.shift();
            // Skip any repeated onDownloadProgress events.
            while (
              received &&
              received.event == prevEvent &&
              prevEvent == "onDownloadProgress"
            ) {
              received = receivedEvents.shift();
            }
            // Wait for more events if we skipped all there were.
            if (!received) {
              eventWaiter = () => {
                eventWaiter = null;
                check();
              };
              return;
            }
            prevEvent = received.event;
            if (received.event != event) {
              let err = new Error(
                `expected ${event} but got ${received.event}`
              );
              reject(err);
            }
            if (props) {
              for (let key of Object.keys(props)) {
                if (received[key] != props[key]) {
                  throw new Error(
                    `AddonInstall property ${key} was ${received[key]} but expected ${props[key]}`
                  );
                }
              }
            }
            resolve();
          }
          check();
        });
      }

      while (steps.length) {
        let nextStep = steps.shift();
        if (nextStep.action) {
          if (nextStep.action == "install") {
            try {
              await install.install();
              if (nextStep.expectError) {
                throw new Error("Expected install to fail but it did not");
              }
            } catch (err) {
              if (!nextStep.expectError) {
                throw new Error("Install failed unexpectedly: " + err);
              }
            }
          } else if (nextStep.action == "cancel") {
            await install.cancel();
          } else {
            throw new Error(`unknown action ${nextStep.action}`);
          }
        } else {
          await expectEvent(nextStep.event, nextStep.props);
        }
      }

      return true;
    }
  );

  is(success, true, description);
}

function makeInstallTest(task) {
  return async function () {
    // withNewTab() will close the test tab before returning, at which point
    // the cleanup event will come from the content process.  We need to see
    // that event but don't want to race to install a listener for it after
    // the tab is closed.  So set up the listener now but don't yield the
    // listening promise until below.
    let clearPromise = waitForClear();

    await BrowserTestUtils.withNewTab(TESTPAGE, task);

    await clearPromise;
    is(AddonManager.webAPI.installs.size, 0, "AddonInstall was cleaned up");
  };
}

function makeRegularTest(options, what) {
  return makeInstallTest(async function (browser) {
    let steps = [
      { action: "install" },
      {
        event: "onDownloadStarted",
        props: { state: "STATE_DOWNLOADING" },
      },
      {
        event: "onDownloadProgress",
        props: { maxProgress: XPI_LEN },
      },
      {
        event: "onDownloadEnded",
        props: {
          state: "STATE_DOWNLOADED",
          progress: XPI_LEN,
          maxProgress: XPI_LEN,
        },
      },
      {
        event: "onInstallStarted",
        props: { state: "STATE_INSTALLING" },
      },
      {
        event: "onInstallEnded",
        props: { state: "STATE_INSTALLED" },
      },
    ];

    let installPromptPromise = promisePopupNotificationShown(
      "addon-webext-permissions"
    ).then(panel => {
      panel.button.click();
    });

    let promptPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      options.addonId
    );

    await testInstall(browser, options, steps, what);

    await installPromptPromise;

    await promptPromise;

    // Sanity check to ensure that the test in makeInstallTest() that
    // installs.size == 0 means we actually did clean up.
    Assert.greater(
      AddonManager.webAPI.installs.size,
      0,
      "webAPI is tracking the AddonInstall"
    );

    let addon = await promiseAddonByID(ID);
    isnot(addon, null, "Found the addon");

    // Check that the expected installTelemetryInfo has been stored in the addon details.
    AddonTestUtils.checkInstallInfo(addon, {
      method: "amWebAPI",
      source: "test-host",
      sourceURL: /https:\/\/example.com\/.*\/webapi_checkavailable.html/,
    });

    await addon.uninstall();

    addon = await promiseAddonByID(ID);
    is(addon, null, "Addon was uninstalled");
  });
}

let addonId = XPI_ADDON_ID;
add_task(makeRegularTest({ url: XPI_URL, addonId }, "a basic install works"));
add_task(
  makeRegularTest(
    { url: XPI_URL, addonId, hash: null },
    "install with hash=null works"
  )
);
add_task(
  makeRegularTest(
    { url: XPI_URL, addonId, hash: "" },
    "install with empty string for hash works"
  )
);
add_task(async function test_install_successfully_with_filehash() {
  await makeRegularTest(
    { url: XPI_URL, addonId, hash: XPI_SHA },
    "install with hash works"
  );
});
add_task(async function test_install_successfully_with_filehash_uppercase() {
  await makeRegularTest(
    { url: XPI_URL, addonId, hash: XPI_SHA.toUpperCase() },
    "install with uppercase hash works"
  );
});

add_task(async function test_invalid_hash() {
  let steps = [
    { action: "install", expectError: true },
    {
      event: "onDownloadStarted",
      props: { state: "STATE_DOWNLOADING" },
    },
    { event: "onDownloadProgress" },
    {
      event: "onDownloadFailed",
      props: {
        state: "STATE_DOWNLOAD_FAILED",
        error: "ERROR_INCORRECT_HASH",
      },
    },
  ];

  info("Verify install failure on invalid hash type");
  await makeInstallTest(async function (browser) {
    await testInstall(
      browser,
      { url: XPI_URL, hash: "foo:bar" },
      steps,
      "install with invalid hash fails"
    );

    let addons = await promiseAddonsByIDs([ID]);
    is(addons[0], null, "The addon was not installed");

    Assert.greater(
      AddonManager.webAPI.installs.size,
      0,
      "webAPI is tracking the AddonInstall"
    );
  });

  info("Verify install failure on invalid hash value");
  await makeInstallTest(async function (browser) {
    await testInstall(
      browser,
      { url: XPI_URL, hash: XPI_INVALID_SHA },
      steps,
      "install with invalid hash fails"
    );

    let addons = await promiseAddonsByIDs([ID]);
    is(addons[0], null, "The addon was not installed");

    Assert.greater(
      AddonManager.webAPI.installs.size,
      0,
      "webAPI is tracking the AddonInstall"
    );
  });
});

add_task(
  makeInstallTest(async function (browser) {
    let steps = [
      { action: "cancel" },
      {
        event: "onDownloadCancelled",
        props: {
          state: "STATE_CANCELLED",
          error: null,
        },
      },
    ];

    await testInstall(
      browser,
      { url: XPI_URL },
      steps,
      "canceling an install works"
    );

    let addons = await promiseAddonsByIDs([ID]);
    is(addons[0], null, "The addon was not installed");

    Assert.greater(
      AddonManager.webAPI.installs.size,
      0,
      "webAPI is tracking the AddonInstall"
    );
  })
);

add_task(
  makeInstallTest(async function (browser) {
    let steps = [
      { action: "install", expectError: true },
      {
        event: "onDownloadStarted",
        props: { state: "STATE_DOWNLOADING" },
      },
      { event: "onDownloadProgress" },
      {
        event: "onDownloadFailed",
        props: {
          state: "STATE_DOWNLOAD_FAILED",
          error: "ERROR_NETWORK_FAILURE",
        },
      },
    ];

    await testInstall(
      browser,
      { url: XPI_URL + "bogus" },
      steps,
      "install of a bad url fails"
    );

    let addons = await promiseAddonsByIDs([ID]);
    is(addons[0], null, "The addon was not installed");

    Assert.greater(
      AddonManager.webAPI.installs.size,
      0,
      "webAPI is tracking the AddonInstall"
    );
  })
);

add_task(
  makeInstallTest(async function (browser) {
    let steps = [
      { action: "install", expectError: true },
      {
        event: "onDownloadStarted",
        props: { state: "STATE_DOWNLOADING" },
      },
      { event: "onDownloadProgress" },
      {
        event: "onDownloadFailed",
        props: {
          state: "STATE_DOWNLOAD_FAILED",
          error: "ERROR_INCORRECT_HASH",
        },
      },
    ];

    await testInstall(
      browser,
      { url: XPI_URL, hash: "sha256:bogus" },
      steps,
      "install with bad hash fails"
    );

    let addons = await promiseAddonsByIDs([ID]);
    is(addons[0], null, "The addon was not installed");

    Assert.greater(
      AddonManager.webAPI.installs.size,
      0,
      "webAPI is tracking the AddonInstall"
    );
  })
);

add_task(async function test_permissions_and_policy() {
  async function testBadUrl(url, pattern, successMessage) {
    gBrowser.selectedTab = await BrowserTestUtils.addTab(gBrowser, TESTPAGE);
    let browser = gBrowser.getBrowserForTab(gBrowser.selectedTab);
    await BrowserTestUtils.browserLoaded(browser);
    let result = await SpecialPowers.spawn(
      browser,
      [{ url, pattern }],
      function (opts) {
        return new Promise(resolve => {
          content.navigator.mozAddonManager
            .createInstall({ url: opts.url })
            .then(
              () => {
                resolve({
                  success: false,
                  message: "createInstall should not have succeeded",
                });
              },
              err => {
                if (err.message.match(new RegExp(opts.pattern))) {
                  resolve({ success: true });
                }
                resolve({
                  success: false,
                  message: `Wrong error message: ${err.message}`,
                });
              }
            );
        });
      }
    );
    is(result.success, true, result.message || successMessage);
  }

  await testBadUrl(
    "i am not a url",
    "NS_ERROR_MALFORMED_URI",
    "Installing from an unparseable URL fails"
  );
  gBrowser.removeTab(gBrowser.selectedTab);

  let popupPromise = promisePopupNotificationShown(
    "addon-install-webapi-blocked"
  );
  await Promise.all([
    testBadUrl(
      "https://addons.not-really-mozilla.org/impostor.xpi",
      "not permitted",
      "Installing from non-approved URL fails"
    ),
    popupPromise,
  ]);

  gBrowser.removeTab(gBrowser.selectedTab);

  const blocked_install_message = "Custom Policy Block Message";

  await EnterprisePolicyTesting.setupPolicyEngineWithJson({
    policies: {
      ExtensionSettings: {
        "*": {
          install_sources: [],
          blocked_install_message,
        },
      },
    },
  });

  popupPromise = promisePopupNotificationShown("addon-install-policy-blocked");

  await testBadUrl(
    XPI_URL,
    "not permitted by policy",
    "Installing from policy blocked origin fails"
  );

  const panel = await popupPromise;
  const description = panel.querySelector(
    ".popup-notification-description"
  ).textContent;
  ok(
    description.startsWith("Your organization"),
    "Policy specific error is shown."
  );
  ok(
    description.endsWith(` ${blocked_install_message}`),
    `Found the expected custom blocked message in "${description}"`
  );

  gBrowser.removeTab(gBrowser.selectedTab);

  await EnterprisePolicyTesting.setupPolicyEngineWithJson({
    policies: {
      ExtensionSettings: {
        "*": {
          install_sources: ["<all_urls>"],
        },
      },
    },
  });
});

add_task(
  makeInstallTest(async function (browser) {
    let xpiURL = `${SECURE_TESTROOT}../xpinstall/incompatible.xpi`;
    let id = "incompatible-xpi@tests.mozilla.org";

    let steps = [
      { action: "install", expectError: true },
      {
        event: "onDownloadStarted",
        props: { state: "STATE_DOWNLOADING" },
      },
      { event: "onDownloadProgress" },
      { event: "onDownloadEnded" },
      { event: "onDownloadCancelled", error: "ERROR_INCOMPATIBLE" },
    ];

    await testInstall(
      browser,
      { url: xpiURL },
      steps,
      "install of an incompatible XPI fails"
    );

    let addons = await promiseAddonsByIDs([id]);
    is(addons[0], null, "The addon was not installed");
  })
);

function test_blocklisted_install({ id, stash, expectedError }) {
  return makeInstallTest(async function (browser) {
    await AddonTestUtils.loadBlocklistRawData({
      extensionsMLBF: [{ stash, stash_time: 0 }],
    });
    let needsCleanupBlocklist = true;
    const cleanupBlocklist = async () => {
      if (!needsCleanupBlocklist) {
        return;
      }
      await AddonTestUtils.loadBlocklistRawData({
        extensionsMLBF: [
          {
            stash: { blocked: [], unblocked: [] },
            stash_time: 0,
          },
        ],
      });
      needsCleanupBlocklist = false;
    };
    registerCleanupFunction(cleanupBlocklist);

    let steps = [
      { action: "install", expectError: true },
      { event: "onDownloadStarted" },
      { event: "onDownloadProgress" },
      { event: "onDownloadEnded" },
      {
        event: "onDownloadCancelled",
        props: { state: "STATE_CANCELLED", error: expectedError },
      },
    ];

    await testInstall(
      browser,
      { url: XPI_URL },
      steps,
      "install of a blocked XPI fails"
    );

    let addons = await promiseAddonsByIDs([id]);
    is(addons[0], null, "The addon was not installed");

    await cleanupBlocklist();
  });
}

add_task(function test_webapi_install_hard_blocked() {
  let id = "amosigned-xpi@tests.mozilla.org";
  let version = "2.2";

  return test_blocklisted_install({
    id,
    stash: { blocked: [`${id}:${version}`], unblocked: [] },
    expectedError: "ERROR_BLOCKLISTED",
  })();
});

add_task(async function test_webapi_install_soft_blocked() {
  let id = "amosigned-xpi@tests.mozilla.org";
  let version = "2.2";

  await SpecialPowers.pushPrefEnv({
    set: [["extensions.blocklist.softblock.enabled", true]],
  });

  await test_blocklisted_install({
    id,
    stash: { softblocked: [`${id}:${version}`], blocked: [], unblocked: [] },
    expectedError: "ERROR_SOFT_BLOCKED",
  })();

  await SpecialPowers.popPrefEnv();
});

add_task(
  makeInstallTest(async function (browser) {
    const options = { url: XPI_URL, addonId };
    let steps = [
      { action: "install" },
      {
        event: "onDownloadStarted",
        props: { state: "STATE_DOWNLOADING" },
      },
      {
        event: "onDownloadProgress",
        props: { maxProgress: XPI_LEN },
      },
      {
        event: "onDownloadEnded",
        props: {
          state: "STATE_DOWNLOADED",
          progress: XPI_LEN,
          maxProgress: XPI_LEN,
        },
      },
      {
        event: "onInstallStarted",
        props: { state: "STATE_INSTALLING" },
      },
      {
        event: "onInstallEnded",
        props: { state: "STATE_INSTALLED" },
      },
    ];

    await SpecialPowers.spawn(browser, [TESTPATH], testPath => {
      // `sourceURL` should match the exact location, even after a location
      // update using the history API. In this case, we update the URL with
      // query parameters and expect `sourceURL` to contain those parameters.
      content.history.pushState(
        {}, // state
        "", // title
        `/${testPath}?some=query&par=am`
      );
    });

    let installPromptPromise = promisePopupNotificationShown(
      "addon-webext-permissions"
    ).then(panel => {
      panel.button.click();
    });

    let promptPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      options.addonId
    );

    await Promise.all([
      testInstall(browser, options, steps, "install to check source URL"),
      installPromptPromise,
      promptPromise,
    ]);

    let addon = await promiseAddonByID(ID);

    registerCleanupFunction(async () => {
      await addon.uninstall();
    });

    // Check that the expected installTelemetryInfo has been stored in the
    // addon details.
    AddonTestUtils.checkInstallInfo(addon, {
      method: "amWebAPI",
      source: "test-host",
      sourceURL:
        "https://example.com/webapi_checkavailable.html?some=query&par=am",
    });
  })
);
