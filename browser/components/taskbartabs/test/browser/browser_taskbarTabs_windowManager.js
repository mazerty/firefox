/* Any copyright is dedicated to the Public Domain.
http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

ChromeUtils.defineESModuleGetters(this, {
  sinon: "resource://testing-common/Sinon.sys.mjs",
  TaskbarTabs: "resource:///modules/taskbartabs/TaskbarTabs.sys.mjs",
  TaskbarTabsUtils: "resource:///modules/taskbartabs/TaskbarTabsUtils.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetters(this, {
  WinTaskbar: ["@mozilla.org/windows-taskbar;1", "nsIWinTaskbar"],
});

const registry = new TaskbarTabsRegistry();

const url1 = Services.io.newURI("https://example.com");
const userContextId1 = 0;
const taskbarTab1 = registry.findOrCreateTaskbarTab(url1, userContextId1);
const id1 = taskbarTab1.id;

const url2 = Services.io.newURI("https://subdomain.example.com");
const userContextId2 = 1;
const taskbarTab2 = registry.findOrCreateTaskbarTab(url2, userContextId2);
const id2 = taskbarTab2.id;

add_task(async function test_count_for_id() {
  const wm = new TaskbarTabsWindowManager();
  let testWindowCount = (aCount1, aCount2) => {
    is(
      wm.getCountForId(id1),
      aCount1,
      `${aCount1} Taskbar Tab window(s) should exist for id ${id1}`
    );
    is(
      wm.getCountForId(id2),
      aCount2,
      `${aCount2} Taskbar Tab window(s) should exist for id ${id2}`
    );
  };

  testWindowCount(0, 0);

  let windowPromise = BrowserTestUtils.waitForNewWindow();
  await wm.openWindow(taskbarTab1);
  let win1_to_eject = await windowPromise;

  testWindowCount(1, 0);

  windowPromise = BrowserTestUtils.waitForNewWindow();
  await wm.openWindow(taskbarTab1);
  let win2 = await windowPromise;

  testWindowCount(2, 0);

  windowPromise = BrowserTestUtils.waitForNewWindow();
  await wm.openWindow(taskbarTab2);
  let win3_to_eject = await windowPromise;

  testWindowCount(2, 1);

  let tab1_adopted = await BrowserTestUtils.addTab(window.gBrowser, url1.spec);
  windowPromise = BrowserTestUtils.waitForNewWindow();
  await wm.replaceTabWithWindow(taskbarTab1, tab1_adopted);
  let win4 = await windowPromise;

  testWindowCount(3, 1);

  let tabOpenPromise = BrowserTestUtils.waitForEvent(
    window.gBrowser.tabContainer,
    "TabOpen"
  );
  await wm.ejectWindow(win1_to_eject);
  let tab2 = (await tabOpenPromise).target;

  testWindowCount(2, 1);

  tabOpenPromise = BrowserTestUtils.waitForEvent(
    window.gBrowser.tabContainer,
    "TabOpen"
  );
  await wm.ejectWindow(win3_to_eject);
  let tab3 = (await tabOpenPromise).target;

  testWindowCount(2, 0);

  BrowserTestUtils.removeTab(tab2);
  BrowserTestUtils.removeTab(tab3);
  await Promise.all([
    BrowserTestUtils.closeWindow(win2),
    BrowserTestUtils.closeWindow(win4),
  ]);
});

add_task(async function test_user_context_id() {
  function checkUserContextId(win, taskbarTab) {
    is(
      win.gBrowser.selectedTab.userContextId,
      taskbarTab.userContextId,
      "Tab's userContextId should match that for the Taskbar Tab."
    );
  }

  const wm = new TaskbarTabsWindowManager();

  let testForTaskbarTab = async taskbarTab => {
    let windowPromise = BrowserTestUtils.waitForNewWindow();
    await wm.openWindow(taskbarTab);
    let win = await windowPromise;
    checkUserContextId(win, taskbarTab);

    const tabOpenPromise = BrowserTestUtils.waitForEvent(
      window.gBrowser.tabContainer,
      "TabOpen"
    );
    await wm.ejectWindow(win);
    let tab = (await tabOpenPromise).target;
    win = tab.ownerGlobal;
    checkUserContextId(win, taskbarTab);

    windowPromise = BrowserTestUtils.waitForNewWindow();
    await wm.replaceTabWithWindow(taskbarTab, tab);
    win = await windowPromise;
    checkUserContextId(win, taskbarTab);

    await BrowserTestUtils.closeWindow(win);
  };

  await testForTaskbarTab(taskbarTab1);
  await testForTaskbarTab(taskbarTab2);
});

add_task(async function test_eject_window_selected_tab() {
  const wm = new TaskbarTabsWindowManager();

  let windowPromise = BrowserTestUtils.waitForNewWindow();
  await wm.openWindow(taskbarTab1);
  let win = await windowPromise;

  const tabOpenPromise = BrowserTestUtils.waitForEvent(
    window.gBrowser.tabContainer,
    "TabOpen"
  );
  await wm.ejectWindow(win);
  let tab = (await tabOpenPromise).target;

  is(
    tab,
    window.gBrowser.selectedTab,
    "The ejected Taskbar Tab should be the selected tab."
  );

  await BrowserTestUtils.removeTab(tab);
});

add_task(async function test_window_aumid() {
  const wm = new TaskbarTabsWindowManager();

  let windowPromise = BrowserTestUtils.waitForNewWindow();
  await wm.openWindow(taskbarTab1);
  let winOpen = await windowPromise;

  is(
    TaskbarTabsUtils.getTaskbarTabIdFromWindow(winOpen),
    taskbarTab1.id,
    "The window's `tasbkartab` attribute should match the Taskbar Tab ID when opened."
  );
  is(
    WinTaskbar.getGroupIdForWindow(winOpen),
    taskbarTab1.id,
    "The window AUMID should match the Taskbar Tab ID when opened."
  );

  let tab1_adopted = await BrowserTestUtils.addTab(window.gBrowser, url1.spec);
  windowPromise = BrowserTestUtils.waitForNewWindow();
  await wm.replaceTabWithWindow(taskbarTab1, tab1_adopted);
  let winReplace = await windowPromise;

  is(
    TaskbarTabsUtils.getTaskbarTabIdFromWindow(winReplace),
    taskbarTab1.id,
    "The window's `tasbkartab` attribute should match the Taskbar Tab ID when a tab was replaced with a Tasbkar Tab window."
  );
  is(
    WinTaskbar.getGroupIdForWindow(winReplace),
    taskbarTab1.id,
    "The window AUMID should match the Taskbar Tab ID when a tab was replaced with a Tasbkar Tab window."
  );

  await Promise.all([
    BrowserTestUtils.closeWindow(winOpen),
    BrowserTestUtils.closeWindow(winReplace),
  ]);
});

add_task(async function testTaskbarTabCount() {
  const count = () => TaskbarTabs.getCountForId(taskbarTab1.id);

  is(await count(), 0, "window count starts at 0");

  const window1 = await TaskbarTabs.openWindow(taskbarTab1);
  is(await count(), 1, "window count increases on first open");

  const window2 = await TaskbarTabs.openWindow(taskbarTab1);
  is(await count(), 2, "window count increases on second open");

  await BrowserTestUtils.closeWindow(window1);
  is(await count(), 1, "window count decreases on first close");

  const addedTab = BrowserTestUtils.addTab(gBrowser, url1.spec);
  const window3 = await TaskbarTabs.replaceTabWithWindow(taskbarTab1, addedTab);
  is(await count(), 2, "window count increases on replace");

  await TaskbarTabs.ejectWindow(window2);
  await BrowserTestUtils.windowClosed(window2);
  is(await count(), 1, "window count decreases on ejection");

  await BrowserTestUtils.closeWindow(window3);
  is(await count(), 0, "window count decreases on final close");

  const ejected = gBrowser.tabs[gBrowser.tabs.length - 1];
  const promise = BrowserTestUtils.waitForTabClosing(ejected);
  gBrowser.removeTab(ejected);
  await promise;

  TaskbarTabs.removeTaskbarTab(taskbarTab1.id);
});

add_task(async function testWindowIconSet() {
  const wm = new TaskbarTabsWindowManager();

  let mockWindowsUIUtils = {
    QueryInterface: ChromeUtils.generateQI(["nsIWindowsUIUtils"]),
    setWindowIcon: sinon.spy(),
  };
  wm.testOnlyMockUIUtils(mockWindowsUIUtils);

  let windowPromise = BrowserTestUtils.waitForNewWindow();
  await wm.openWindow(taskbarTab1);
  let win = await windowPromise;

  ok(
    mockWindowsUIUtils.setWindowIcon.calledOnce,
    "The Window icon should have been set."
  );

  // `sinon.spy` will hold a reference to the window by virtue of holding it's
  // arguments, causing tests to fail from a "leaked" window. Release it before
  // closing the window.
  sinon.resetHistory();
  wm.testOnlyMockUIUtils(null);

  await BrowserTestUtils.closeWindow(win);
});
