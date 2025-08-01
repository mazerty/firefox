/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

// Tests loading and pretty printing bundles with sourcemaps disabled

"use strict";

requestLongerTimeout(2);

add_task(async function () {
  await pushPref("devtools.source-map.client-service.enabled", false);
  const dbg = await initDebugger("doc-sourcemaps.html");

  await waitForSources(dbg, "bundle.js");
  const bundleSrc = findSource(dbg, "bundle.js");

  info("Pretty print the bundle");
  await selectSource(dbg, bundleSrc);

  const footerButton = findElement(dbg, "sourceMapFooterButton");
  is(
    footerButton.textContent,
    "Source Maps disabled",
    "The source map button reports the disabling"
  );
  ok(
    footerButton.classList.contains("disabled"),
    "The source map button is disabled"
  );

  await togglePrettyPrint(dbg);
  ok(true, "Pretty printed source shown");

  const toggled = waitForDispatch(dbg.store, "TOGGLE_SOURCE_MAPS_ENABLED");
  await clickOnSourceMapMenuItem(dbg, ".debugger-source-map-enabled");
  await toggled;
  ok(true, "Toggled the Source map setting");

  is(
    footerButton.textContent,
    "original file",
    "The source map button now reports the pretty printed file as original file"
  );
  ok(
    !footerButton.classList.contains("disabled"),
    "The source map button is no longer disabled"
  );
});
