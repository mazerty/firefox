/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/debugger/test/mochitest/shared-head.js",
  this
);

const SCRIPT_FILE = "script_aboutdebugging_devtoolstoolbox_breakpoint.js";
const TAB_URL =
  "https://example.com/browser/devtools/client/aboutdebugging/" +
  "test/browser/resources/doc_aboutdebugging_devtoolstoolbox_breakpoint.html";

/* import-globals-from helper-collapsibilities.js */
Services.scriptloader.loadSubScript(
  CHROME_URL_ROOT + "helper-collapsibilities.js",
  this
);

/**
 * Test breakpoints in about:devtools-toolbox tabs (ie non localTab tab target).
 */
add_task(async function () {
  const testTab = await addTab(TAB_URL);

  info("Force all debug target panes to be expanded");
  prepareCollapsibilitiesTest();

  const { document, tab, window } = await openAboutDebugging();
  await selectThisFirefoxPage(document, window.AboutDebugging.store);
  const { devtoolsTab, devtoolsWindow } = await openAboutDevtoolsToolbox(
    document,
    tab,
    window,
    TAB_URL
  );
  info("Select performance panel");
  const toolbox = getToolbox(devtoolsWindow);
  await toolbox.selectTool("jsdebugger");

  info("Add a breakpoint at line 10 in the test script");
  const debuggerContext = createDebuggerContext(toolbox);
  await selectSource(debuggerContext, SCRIPT_FILE);
  await addBreakpoint(debuggerContext, SCRIPT_FILE, 10);

  is(
    gBrowser.selectedTab,
    devtoolsTab,
    "Selected tab should already be the devtools tab"
  );

  info("Invoke testMethod, expect the script to pause on line 10");
  let onContentTaskDone = invokeTestMethod(testTab);

  info("Wait for the debugger to pause");
  await waitForPaused(debuggerContext);
  const script = findSource(debuggerContext, SCRIPT_FILE);
  await assertPausedAtSourceAndLine(debuggerContext, script.id, 10);
  is(
    gBrowser.selectedTab,
    devtoolsTab,
    "Selected tab should still be the devtools tab"
  );

  info("Resume");
  await resume(debuggerContext);

  info("Wait for the paused content task to also resolve");
  await onContentTaskDone;

  info("Select another tab, and check that breaking focuses the toolbox tab");
  gBrowser.selectedTab = testTab;
  onContentTaskDone = invokeTestMethod(testTab);

  info("Wait for the debugger to pause, and check the selected tab");
  await waitForPaused(debuggerContext);
  is(
    gBrowser.selectedTab,
    devtoolsTab,
    "Selected tab should be back to the devtools tab"
  );

  info("Resume");
  await resume(debuggerContext);

  info("Wait for the paused content task to also resolve");
  await onContentTaskDone;

  info("Remove breakpoint");
  await removeBreakpoint(debuggerContext, script.id, 10);

  await closeAboutDevtoolsToolbox(document, devtoolsTab, window);
  await removeTab(testTab);
  await removeTab(tab);
});

function invokeTestMethod(tab) {
  return ContentTask.spawn(tab.linkedBrowser, {}, function () {
    content.wrappedJSObject.testMethod();
  });
}
