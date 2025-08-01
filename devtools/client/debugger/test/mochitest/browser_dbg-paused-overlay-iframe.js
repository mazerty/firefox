/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

// Tests the paused overlay in a remote frame

"use strict";

const TEST_COM_URI = `${URL_ROOT_COM_SSL}examples/doc_dbg-fission-frame-sources.html`;

add_task(async function () {
  info("Load a test page with a remote frame");
  // simple1.js is imported by the main page. simple2.js comes from the remote frame.
  const dbg = await initDebuggerWithAbsoluteURL(
    TEST_COM_URI,
    "simple1.js",
    "simple2.js"
  );
  const {
    selectors: { getSelectedSource },
    commands,
  } = dbg;

  info("Add breakpoint within the iframe, in `foo` function");
  await selectSource(dbg, "simple2.js");
  await addBreakpoint(dbg, "simple2.js", 5);

  info("Call `foo` in the iframe");
  SpecialPowers.spawn(gBrowser.selectedBrowser, [], () => {
    const iframe = content.document.querySelector("iframe");
    SpecialPowers.spawn(iframe, [], () => {
      return content.wrappedJSObject.foo();
    });
  });
  await waitForPaused(dbg);
  ok(true, "debugger is paused");

  // We need to retrieve the highlighterTestFront for the frame target.
  const iframeTarget = commands.targetCommand
    .getAllTargets([commands.targetCommand.TYPES.FRAME])
    .find(target => target.url.includes("example.org"));
  const highlighterTestFront = await iframeTarget.getFront("highlighterTest");

  info("Check that the paused overlay is displayed");
  await waitFor(() => highlighterTestFront.isPausedDebuggerOverlayVisible());
  ok(true, "Paused debugger overlay is visible");

  ok(
    getSelectedSource().url.includes("simple2.js"),
    "Selected source is simple2.js"
  );
  await assertPausedAtSourceAndLine(dbg, findSource(dbg, "simple2.js").id, 5);

  info("Test clicking the resume button");
  await highlighterTestFront.clickPausedDebuggerOverlayButton(
    "paused-dbg-resume-button"
  );

  await waitForResumed(dbg);
  ok("The debugger isn't paused after clicking on the resume button");

  await waitFor(async () => {
    const visible = await highlighterTestFront.isPausedDebuggerOverlayVisible();
    return !visible;
  });

  ok(true, "The overlay is now hidden");
});
