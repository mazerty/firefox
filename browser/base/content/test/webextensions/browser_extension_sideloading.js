/* eslint-disable mozilla/no-arbitrary-setTimeout */
const { AddonManagerPrivate } = ChromeUtils.importESModule(
  "resource://gre/modules/AddonManager.sys.mjs"
);

const { AddonTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/AddonTestUtils.sys.mjs"
);

AddonTestUtils.initMochitest(this);

AddonTestUtils.hookAMTelemetryEvents();

const kSideloaded = true;

async function createWebExtension(details) {
  let options = {
    manifest: {
      manifest_version: details.manifest_version ?? 2,

      browser_specific_settings: { gecko: { id: details.id } },

      name: details.name,

      permissions: details.permissions,
      host_permissions: details.host_permissions,
      incognito: details.incognito,
    },
  };

  if (details.iconURL) {
    options.manifest.icons = { 64: details.iconURL };
  }

  let xpi = AddonTestUtils.createTempWebExtensionFile(options);

  await AddonTestUtils.manuallyInstall(xpi);
}

function promiseEvent(eventEmitter, event) {
  return new Promise(resolve => {
    eventEmitter.once(event, resolve);
  });
}

function getAddonElement(managerWindow, addonId) {
  return TestUtils.waitForCondition(
    () =>
      managerWindow.document.querySelector(`addon-card[addon-id="${addonId}"]`),
    `Found entry for sideload extension addon "${addonId}" in HTML about:addons`
  );
}

function assertSideloadedAddonElementState(addonElement, pressed) {
  const enableBtn = addonElement.querySelector('[action="toggle-disabled"]');
  is(
    enableBtn.pressed,
    pressed,
    `The enable button is ${!pressed ? " not " : ""} pressed`
  );
  is(enableBtn.localName, "moz-toggle", "The enable button is a toggle");
}

function clickEnableExtension(addonElement) {
  addonElement.querySelector('[action="toggle-disabled"]').click();
}

add_task(async function test_sideloading() {
  const DEFAULT_ICON_URL =
    "chrome://mozapps/skin/extensions/extensionGeneric.svg";

  await SpecialPowers.pushPrefEnv({
    set: [
      ["xpinstall.signatures.required", false],
      ["extensions.autoDisableScopes", 15],
      ["extensions.ui.disableUnsignedWarnings", true],
    ],
  });

  Services.fog.testResetFOG();

  const ID1 = "addon1@tests.mozilla.org";
  await createWebExtension({
    id: ID1,
    name: "Test 1",
    userDisabled: true,
    permissions: ["history", "https://*/*"],
    iconURL: "foo-icon.png",
  });

  const ID2 = "addon2@tests.mozilla.org";
  await createWebExtension({
    manifest_version: 3,
    id: ID2,
    name: "Test 2",
    host_permissions: ["<all_urls>"],
  });

  const ID3 = "addon3@tests.mozilla.org";
  await createWebExtension({
    id: ID3,
    name: "Test 3",
    permissions: ["<all_urls>"],
  });

  const ID4 = "addon4@tests.mozilla.org";
  await createWebExtension({
    id: ID4,
    name: "Test 4",
    incognito: "not_allowed",
    permissions: [],
  });

  const ID5 = "addon5@tests.mozilla.org";
  await createWebExtension({
    id: ID5,
    name: "Test 5",
    incognito: "not_allowed",
    permissions: ["<all_urls>"],
  });

  const ID6 = "addon6@tests.mozilla.org";
  await createWebExtension({
    id: ID6,
    name: "Test 6",
    incognito: "not_allowed",
    permissions: ["history", "https://*/*"],
  });

  testCleanup = async function () {
    // clear out ExtensionsUI state about sideloaded extensions so
    // subsequent tests don't get confused.
    ExtensionsUI.sideloaded.clear();
    ExtensionsUI.emit("change");
  };

  // Navigate away from the starting page to force about:addons to load
  // in a new tab during the tests below.
  BrowserTestUtils.startLoadingURIString(
    gBrowser.selectedBrowser,
    "about:robots"
  );
  await BrowserTestUtils.browserLoaded(gBrowser.selectedBrowser);

  registerCleanupFunction(async function () {
    // Return to about:blank when we're done
    BrowserTestUtils.startLoadingURIString(
      gBrowser.selectedBrowser,
      "about:blank"
    );
    await BrowserTestUtils.browserLoaded(gBrowser.selectedBrowser);
  });

  let changePromise = new Promise(resolve => {
    ExtensionsUI.on("change", function listener() {
      ExtensionsUI.off("change", listener);
      resolve();
    });
  });
  ExtensionsUI._checkForSideloaded();
  await changePromise;

  // Check for the addons badge on the hamburger menu
  let menuButton = document.getElementById("PanelUI-menu-button");
  is(
    menuButton.getAttribute("badge-status"),
    "addon-alert",
    "Should have addon alert badge"
  );

  // Find the menu entries for sideloaded extensions
  await gCUITestUtils.openMainMenu();

  let addons = PanelUI.addonNotificationContainer;
  is(
    addons.children.length,
    4,
    "Have 4 menu entries for sideloaded extensions"
  );

  info(
    "Test disabling sideloaded addon 1 using the permission prompt secondary button"
  );

  // Click the first sideloaded extension
  let popupPromise = promisePopupNotificationShown("addon-webext-permissions");
  addons.children[0].click();

  // The click should hide the main menu. This is currently synchronous.
  Assert.notEqual(
    PanelUI.panel.state,
    "open",
    "Main menu is closed or closing."
  );

  // When we get the permissions prompt, we should be at the extensions
  // list in about:addons
  let panel = await popupPromise;
  is(
    gBrowser.currentURI.spec,
    "about:addons",
    "Foreground tab is at about:addons"
  );

  const VIEW = "addons://list/extension";
  let win = gBrowser.selectedBrowser.contentWindow;

  await TestUtils.waitForCondition(
    () => !win.gViewController.isLoading,
    "about:addons view is fully loaded"
  );
  is(
    win.gViewController.currentViewId,
    VIEW,
    "about:addons is at extensions list"
  );

  // Check the contents of the notification, then choose "Cancel"
  checkNotification(
    panel,
    /\/foo-icon\.png$/,
    [
      ["webext-perms-host-description-all-urls"],
      ["webext-perms-description-history"],
    ],
    kSideloaded
  );

  panel.secondaryButton.click();

  let [addon1, addon2, addon3, addon4, addon5, addon6] =
    await AddonManager.getAddonsByIDs([ID1, ID2, ID3, ID4, ID5, ID6]);
  ok(addon1.seen, "Addon should be marked as seen");
  is(addon1.userDisabled, true, "Addon 1 should still be disabled");
  is(addon2.userDisabled, true, "Addon 2 should still be disabled");
  is(addon3.userDisabled, true, "Addon 3 should still be disabled");
  is(addon4.userDisabled, true, "Addon 4 should still be disabled");
  is(addon5.userDisabled, true, "Addon 5 should still be disabled");
  is(addon6.userDisabled, true, "Addon 6 should still be disabled");

  BrowserTestUtils.removeTab(gBrowser.selectedTab);

  // Should still have 2 entries in the hamburger menu
  await gCUITestUtils.openMainMenu();

  addons = PanelUI.addonNotificationContainer;
  is(
    addons.children.length,
    4,
    "Have 4 menu entries for sideloaded extensions"
  );

  // Close the hamburger menu and go directly to the addons manager
  await gCUITestUtils.hideMainMenu();

  win = await BrowserAddonUI.openAddonsMgr(VIEW);
  await waitAboutAddonsViewLoaded(win.document);

  // about:addons addon entry element.
  const addonElement = await getAddonElement(win, ID2);

  assertSideloadedAddonElementState(addonElement, false);

  info("Test enabling sideloaded addon 2 from about:addons enable button");

  // When clicking enable we should see the permissions notification
  popupPromise = promisePopupNotificationShown("addon-webext-permissions");
  clickEnableExtension(addonElement);
  panel = await popupPromise;
  checkNotification(
    panel,
    DEFAULT_ICON_URL,
    [["webext-perms-host-description-all-urls"]],
    kSideloaded
  );
  ok(
    panel.querySelector(".webext-perm-privatebrowsing moz-checkbox"),
    "Expect incognito checkbox in sideload prompt"
  );

  // Accept the permissions
  panel.button.click();
  await promiseEvent(ExtensionsUI, "change");

  addon2 = await AddonManager.getAddonByID(ID2);
  is(addon2.userDisabled, false, "Addon 2 should be enabled");
  assertSideloadedAddonElementState(addonElement, true);

  // Should still have 1 entry in the hamburger menu
  await gCUITestUtils.openMainMenu();

  addons = PanelUI.addonNotificationContainer;
  is(addons.children.length, 4, "Have 4 menu entry for sideloaded extensions");

  // Close the hamburger menu and go to the detail page for this addon
  await gCUITestUtils.hideMainMenu();

  win = await BrowserAddonUI.openAddonsMgr(
    `addons://detail/${encodeURIComponent(ID3)}`
  );

  // Trigger all remaining addon install from the app menu, to be able to cover the
  // post install notification that should be triggered when the permission
  // dialog is accepted from that flow.
  const enableSideloadedFromAppMenu = async (addon, testPanelCb) => {
    popupPromise = promisePopupNotificationShown("addon-webext-permissions");
    ExtensionsUI.showSideloaded(gBrowser, addon);
    panel = await popupPromise;
    await testPanelCb();
    // Accept the permissions
    panel.button.click();
    await promiseEvent(ExtensionsUI, "change");
  };

  info("Test enabling sideloaded addon 3 from app menu");
  await enableSideloadedFromAppMenu(addon3, () => {
    checkNotification(
      panel,
      DEFAULT_ICON_URL,
      [["webext-perms-host-description-all-urls"]],
      kSideloaded
    );
  });

  addon3 = await AddonManager.getAddonByID(ID3);
  is(addon3.userDisabled, false, "Addon 3 should be enabled");

  addons = PanelUI.addonNotificationContainer;
  is(addons.children.length, 3, "Have 3 menu entry for sideloaded extensions");

  info("Test enabling sideloaded addon 4 from app menu");
  await enableSideloadedFromAppMenu(addon4, () => {
    checkNotification(panel, DEFAULT_ICON_URL, [], kSideloaded);
  });
  addon4 = await AddonManager.getAddonByID(ID4);
  is(addon4.userDisabled, false, "Addon 4 should be enabled");
  addons = PanelUI.addonNotificationContainer;
  is(addons.children.length, 2, "Have 2 menu entry for sideloaded extensions");

  info("Test enabling sideloaded addon 5 from app menu");
  await enableSideloadedFromAppMenu(addon5, () => {
    checkNotification(
      panel,
      DEFAULT_ICON_URL,
      [["webext-perms-host-description-all-urls"]],
      kSideloaded
    );
  });
  addon5 = await AddonManager.getAddonByID(ID5);
  is(addon5.userDisabled, false, "Addon 5 should be enabled");
  addons = PanelUI.addonNotificationContainer;
  is(addons.children.length, 1, "Have 1 menu entry for sideloaded extensions");

  info("Test enabling sideloaded addon 6 from app menu");
  await enableSideloadedFromAppMenu(addon6, () => {
    checkNotification(
      panel,
      DEFAULT_ICON_URL,
      [
        ["webext-perms-host-description-all-urls"],
        ["webext-perms-description-history"],
      ],
      kSideloaded
    );
  });
  addon6 = await AddonManager.getAddonByID(ID6);
  is(addon6.userDisabled, false, "Addon 6 should be enabled");

  isnot(
    menuButton.getAttribute("badge-status"),
    "addon-alert",
    "Should no longer have addon alert badge"
  );

  await new Promise(resolve => setTimeout(resolve, 100));

  for (let addon of [addon1, addon2, addon3, addon4, addon5, addon6]) {
    await addon.uninstall();
  }

  isnot(
    menuButton.getAttribute("badge-status"),
    "addon-alert",
    "Should no longer have addon alert badge"
  );

  BrowserTestUtils.removeTab(gBrowser.selectedTab);

  // Assert that the expected AddonManager telemetry are being recorded.
  const expectedExtra = { source: "app-profile", method: "sideload" };

  const baseEvent = { object: "extension", extra: expectedExtra };
  const createBaseEventAddon = n => ({
    ...baseEvent,
    value: `addon${n}@tests.mozilla.org`,
  });
  const getEventsForAddonId = (events, addonId) =>
    events.filter(ev => ev.value === addonId);

  const amEvents = AddonTestUtils.getAMTelemetryEvents();

  // Test telemetry events for addon1 (1 permission and 1 origin).
  info("Test telemetry events collected for addon1");

  const baseEventAddon1 = createBaseEventAddon(1);

  const blocklist_state = `${Ci.nsIBlocklistService.STATE_NOT_BLOCKED}`;

  Assert.deepEqual(
    AddonTestUtils.getAMGleanEvents("manage", { addon_id: ID1 }),
    [
      {
        addon_id: ID1,
        method: "sideload_prompt",
        addon_type: "extension",
        source: "app-profile",
        source_method: "sideload",
        num_strings: "2",
        blocklist_state,
      },
      {
        addon_id: ID1,
        method: "uninstall",
        addon_type: "extension",
        source: "app-profile",
        source_method: "sideload",
        blocklist_state,
      },
    ],
    "Got the expected Glean events for addon1."
  );

  const collectedEventsAddon1 = getEventsForAddonId(
    amEvents,
    baseEventAddon1.value
  );
  const expectedEventsAddon1 = [
    {
      ...baseEventAddon1,
      method: "sideload_prompt",
      extra: { ...expectedExtra, num_strings: "2", blocklist_state },
    },
    {
      ...baseEventAddon1,
      method: "uninstall",
      extra: { ...expectedExtra, blocklist_state },
    },
  ];

  let i = 0;
  for (let event of collectedEventsAddon1) {
    Assert.deepEqual(
      event,
      expectedEventsAddon1[i++],
      "Got the expected telemetry event"
    );
  }

  is(
    collectedEventsAddon1.length,
    expectedEventsAddon1.length,
    "Got the expected number of telemetry events for addon1"
  );

  const baseEventAddon2 = createBaseEventAddon(2);
  const collectedEventsAddon2 = getEventsForAddonId(
    amEvents,
    baseEventAddon2.value
  );
  const expectedEventsAddon2 = [
    {
      ...baseEventAddon2,
      method: "sideload_prompt",
      extra: { ...expectedExtra, num_strings: "1", blocklist_state },
    },
    {
      ...baseEventAddon2,
      method: "enable",
      extra: { ...expectedExtra, blocklist_state },
    },
    {
      ...baseEventAddon2,
      method: "uninstall",
      extra: { ...expectedExtra, blocklist_state },
    },
  ];

  i = 0;
  for (let event of collectedEventsAddon2) {
    Assert.deepEqual(
      event,
      expectedEventsAddon2[i++],
      "Got the expected telemetry event"
    );
  }

  is(
    collectedEventsAddon2.length,
    expectedEventsAddon2.length,
    "Got the expected number of telemetry events for addon2"
  );

  Assert.deepEqual(
    AddonTestUtils.getAMGleanEvents("manage", { addon_id: ID2 }),
    [
      {
        addon_id: ID2,
        method: "sideload_prompt",
        addon_type: "extension",
        source: "app-profile",
        source_method: "sideload",
        num_strings: "1",
        blocklist_state,
      },
      {
        addon_id: ID2,
        method: "enable",
        addon_type: "extension",
        source: "app-profile",
        source_method: "sideload",
        blocklist_state,
      },
      {
        addon_id: ID2,
        method: "uninstall",
        addon_type: "extension",
        source: "app-profile",
        source_method: "sideload",
        blocklist_state,
      },
    ],
    "Got the expected Glean events for addon2."
  );
});
