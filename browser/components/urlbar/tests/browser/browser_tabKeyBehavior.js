/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

// This test makes sure that the tab key properly adjusts the selection or moves
// through toolbar items, depending on the urlbar state.
// When the view is open, tab should go through results if the urlbar was
// focused with the mouse, or has a typed string.

"use strict";

add_setup(async function () {
  let sidebarRevampEnabled = Services.prefs.getBoolPref(
    "sidebar.revamp",
    false
  );
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.urlbar.suggest.quickactions", false],
      ["browser.urlbar.scotchBonnet.enableOverride", false],
      ["browser.urlbar.tabToSearch.onboard.interactionsLeft", 0],
    ],
  });

  for (let i = 0; i < UrlbarPrefs.get("maxRichResults"); i++) {
    await PlacesTestUtils.addVisits("http://example.com/" + i);
  }

  registerCleanupFunction(PlacesUtils.history.clear);
  // Make sure there's a non-disabled, non-hidden button before the address
  // input field:
  if (!sidebarRevampEnabled) {
    CustomizableUI.addWidgetToArea("sidebar-button", "nav-bar", 0);
  }
  registerCleanupFunction(() => {
    CustomizableUI.reset();
  });
});

add_task(async function tabWithSearchString() {
  info("Tab with a search string");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "exam",
  });
  await expectTabThroughResults();
  info("Reverse Tab with a search string");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "exam",
  });
  await expectTabThroughResults({ reverse: true });
});

add_task(async function tabNoSearchString() {
  info("Tab without a search string");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "",
  });
  await expectTabThroughToolbar();
  info("Reverse Tab without a search string");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "",
  });
  await expectTabThroughToolbar({ reverse: true });
});

add_task(async function tabAfterBlur() {
  info("Tab after closing the view");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "exam",
  });
  await UrlbarTestUtils.promisePopupClose(window);
  await expectTabThroughToolbar();
});

add_task(async function tabNoSearchStringMouseFocus() {
  info("Tab in a new blank tab after mouse focus");
  await BrowserTestUtils.withNewTab("about:blank", async () => {
    await UrlbarTestUtils.promisePopupOpen(window, () => {
      EventUtils.synthesizeMouseAtCenter(gURLBar.inputField, {});
    });
    await UrlbarTestUtils.promiseSearchComplete(window);
    await expectTabThroughResults();
  });
  info("Tab in a loaded tab after mouse focus");
  await BrowserTestUtils.withNewTab("example.com", async () => {
    await UrlbarTestUtils.promisePopupOpen(window, () => {
      EventUtils.synthesizeMouseAtCenter(gURLBar.inputField, {});
    });
    await UrlbarTestUtils.promiseSearchComplete(window);
    await expectTabThroughResults();
  });
});

add_task(async function tabNoSearchStringKeyboardFocus() {
  info("Tab in a new blank tab after keyboard focus");
  await BrowserTestUtils.withNewTab("about:blank", async () => {
    await UrlbarTestUtils.promisePopupOpen(window, () => {
      EventUtils.synthesizeKey("l", { accelKey: true });
    });
    await UrlbarTestUtils.promiseSearchComplete(window);
    await expectTabThroughToolbar();
  });
  info("Tab in a loaded tab after keyboard focus");
  await BrowserTestUtils.withNewTab("example.com", async () => {
    await UrlbarTestUtils.promisePopupOpen(window, () => {
      EventUtils.synthesizeKey("l", { accelKey: true });
    });
    await UrlbarTestUtils.promiseSearchComplete(window);
    await expectTabThroughToolbar();
  });
});

add_task(async function tabRetainedResultMouseFocus() {
  info("Tab after retained results with mouse focus");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "exam",
  });
  await UrlbarTestUtils.promisePopupClose(window);
  await UrlbarTestUtils.promisePopupOpen(window, () => {
    EventUtils.synthesizeMouseAtCenter(gURLBar.inputField, {});
  });
  await UrlbarTestUtils.promiseSearchComplete(window);
  await expectTabThroughResults();
});

add_task(async function tabRetainedResultsKeyboardFocus() {
  info("Tab after retained results with keyboard focus");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "exam",
  });
  await UrlbarTestUtils.promisePopupClose(window);
  await UrlbarTestUtils.promisePopupOpen(window, () => {
    EventUtils.synthesizeKey("l", { accelKey: true });
  });
  await UrlbarTestUtils.promiseSearchComplete(window);
  await expectTabThroughResults();
});

add_task(async function tabRetainedResults() {
  info("Tab with a search string after mouse focus.");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "exam",
  });
  await UrlbarTestUtils.promisePopupClose(window);
  EventUtils.synthesizeMouseAtCenter(gURLBar.inputField, {});
  await expectTabThroughResults();
});

add_task(async function tabSearchModePreview() {
  info(
    "Tab past a search mode preview keywordoffer after focusing with the keyboard."
  );
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "@",
  });
  await UrlbarTestUtils.promisePopupClose(window);
  await UrlbarTestUtils.promisePopupOpen(window, () => {
    EventUtils.synthesizeKey("l", { accelKey: true });
  });
  await UrlbarTestUtils.promiseSearchComplete(window);
  let result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.ok(
    result.searchParams.keyword,
    "The first result is a keyword offer."
  );

  // Sanity check: the Urlbar value is cleared when keywordoffer results are
  // selected.
  EventUtils.synthesizeKey("KEY_ArrowDown");
  Assert.ok(!gURLBar.value, "The Urlbar should have no value.");
  EventUtils.synthesizeKey("KEY_ArrowUp");

  await expectTabThroughResults();

  await UrlbarTestUtils.promisePopupClose(window, async () => {
    gURLBar.blur();
    // Verify that blur closes search mode preview.
    await UrlbarTestUtils.assertSearchMode(window, null);
  });
});

add_task(async function tabTabToSearch() {
  info("Tab past a tab-to-search result after focusing with the keyboard.");
  await SearchTestUtils.installSearchExtension();

  for (let i = 0; i < 3; i++) {
    await PlacesTestUtils.addVisits(["https://example.com/"]);
  }

  // Search for a tab-to-search result.
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "exam",
  });
  await UrlbarTestUtils.promisePopupClose(window);
  await UrlbarTestUtils.promisePopupOpen(window, () => {
    EventUtils.synthesizeKey("l", { accelKey: true });
  });
  await UrlbarTestUtils.promiseSearchComplete(window);
  let tabToSearchResult = (
    await UrlbarTestUtils.waitForAutocompleteResultAt(window, 1)
  ).result;
  Assert.equal(
    tabToSearchResult.providerName,
    "TabToSearch",
    "The second result is a tab-to-search result."
  );

  await expectTabThroughResults();

  await UrlbarTestUtils.promisePopupClose(window, async () => {
    gURLBar.blur();
    await UrlbarTestUtils.assertSearchMode(window, null);
  });
  await PlacesUtils.history.clear();
});

add_task(async function tabNoSearchStringSearchMode() {
  info(
    "Tab through the toolbar when refocusing a Urlbar in search mode with the keyboard."
  );
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "",
  });
  // Enter history search mode to avoid hitting the network.
  await UrlbarTestUtils.enterSearchMode(window, {
    source: UrlbarUtils.RESULT_SOURCE.HISTORY,
  });
  await UrlbarTestUtils.promisePopupClose(window);
  await UrlbarTestUtils.promisePopupOpen(window, () => {
    EventUtils.synthesizeKey("l", { accelKey: true });
  });
  await UrlbarTestUtils.promiseSearchComplete(window);

  await expectTabThroughToolbar();

  // We have to reopen the view to exit search mode.
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "",
  });
  await UrlbarTestUtils.exitSearchMode(window);
  await UrlbarTestUtils.promisePopupClose(window);
});

add_task(async function tabOnTopSites() {
  info("Tab through the toolbar when focusing the Address Bar on top sites.");
  for (let val of [true, false]) {
    info(`Test with keyboard_navigation set to "${val}"`);
    await SpecialPowers.pushPrefEnv({
      set: [["browser.toolbars.keyboard_navigation", val]],
    });
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: "",
    });
    Assert.greater(
      UrlbarTestUtils.getResultCount(window),
      0,
      "There should be some results"
    );
    Assert.deepEqual(
      UrlbarTestUtils.getSelectedElement(window),
      null,
      "There should be no selection"
    );

    await expectTabThroughToolbar();
    await UrlbarTestUtils.promisePopupClose(window);
    await SpecialPowers.popPrefEnv();
  }
});

add_task(async function tabActionsSearchMode() {
  await SpecialPowers.pushPrefEnv({
    set: [["browser.urlbar.searchRestrictKeywords.featureGate", true]],
  });

  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "@actions",
  });
  EventUtils.synthesizeKey("KEY_Enter");

  await UrlbarTestUtils.assertSearchMode(window, {
    source: UrlbarUtils.RESULT_SOURCE.ACTIONS,
    isPreview: false,
    entry: "keywordoffer",
    restrictType: "keyword",
  });
  await expectTabThroughResults();

  // We have to reopen the view to exit search mode.
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "",
  });
  await UrlbarTestUtils.exitSearchMode(window);

  await UrlbarTestUtils.promisePopupClose(window);
  await SpecialPowers.popPrefEnv();
});

async function expectTabThroughResults(options = { reverse: false }) {
  let resultCount = UrlbarTestUtils.getResultCount(window);
  Assert.greater(resultCount, 0, "There should be results");

  let result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);

  let initiallySelectedIndex = result.heuristic ? 0 : -1;
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    initiallySelectedIndex,
    "Check the initial selection."
  );

  for (let i = initiallySelectedIndex + 1; i < resultCount; i++) {
    EventUtils.synthesizeKey("KEY_Tab", { shiftKey: options.reverse });
    if (
      UrlbarTestUtils.getButtonForResultIndex(
        window,
        "result-menu",
        UrlbarTestUtils.getSelectedRowIndex(window)
      )
    ) {
      EventUtils.synthesizeKey("KEY_Tab", { shiftKey: options.reverse });
    }
    Assert.equal(
      UrlbarTestUtils.getSelectedRowIndex(window),
      options.reverse ? resultCount - i : i
    );
  }

  EventUtils.synthesizeKey("KEY_Tab");

  if (!options.reverse) {
    Assert.equal(
      UrlbarTestUtils.getSelectedRowIndex(window),
      initiallySelectedIndex,
      "Should be back at the initial selection."
    );
  }

  await UrlbarTestUtils.promisePopupClose(window, () => {
    gURLBar.blur();
  });
}

async function expectTabThroughToolbar(options = { reverse: false }) {
  if (gURLBar.getAttribute("pageproxystate") == "valid") {
    Assert.equal(document.activeElement, gURLBar.inputField);
    EventUtils.synthesizeKey("KEY_Tab");
    Assert.notEqual(document.activeElement, gURLBar.inputField);
  } else {
    let focusPromise = waitForFocusOnNextFocusableElement(options.reverse);
    EventUtils.synthesizeKey("KEY_Tab", { shiftKey: options.reverse });
    await focusPromise;
  }
  Assert.ok(!gURLBar.view.isOpen, "The urlbar view should be closed.");
}

async function waitForFocusOnNextFocusableElement(reverse = false) {
  let sidebarRevampEnabled = Services.prefs.getBoolPref(
    "sidebar.revamp",
    false
  );
  if (
    !Services.prefs.getBoolPref("browser.toolbars.keyboard_navigation", true)
  ) {
    let sidebar = document.querySelector("sidebar-main");
    return BrowserTestUtils.waitForCondition(
      () =>
        document.activeElement ==
        (!sidebarRevampEnabled ? gBrowser.selectedBrowser : sidebar)
    );
  }
  let urlbar = document.getElementById("urlbar-container");
  let nextFocusableElement = reverse
    ? urlbar.previousElementSibling
    : urlbar.nextElementSibling;
  while (
    nextFocusableElement &&
    (!nextFocusableElement.classList.contains("toolbarbutton-1") ||
      nextFocusableElement.hasAttribute("hidden") ||
      nextFocusableElement.hasAttribute("disabled") ||
      BrowserTestUtils.isHidden(nextFocusableElement))
  ) {
    nextFocusableElement = reverse
      ? nextFocusableElement.previousElementSibling
      : nextFocusableElement.nextElementSibling;
  }
  info(
    `Next focusable element: ${nextFocusableElement.localName}.#${nextFocusableElement.id}`
  );

  Assert.ok(
    nextFocusableElement.classList.contains("toolbarbutton-1"),
    "We should have a reference to the next focusable element after the Urlbar."
  );

  return BrowserTestUtils.waitForCondition(
    () => nextFocusableElement.tabIndex == -1
  );
}
