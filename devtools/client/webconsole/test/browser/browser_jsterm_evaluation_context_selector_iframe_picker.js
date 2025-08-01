/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// Test that the evaluation context selector reacts as expected when using the Toolbox
// iframe picker.

const TEST_URI = `https://example.com/document-builder.sjs?html=${encodeURIComponent(`
  <html>
    <h1>example.com</h1>
    <iframe src="https://example.org/document-builder.sjs?html=example.org"></iframe>
    <iframe src="https://example.net/document-builder.sjs?html=example.net"></iframe>
  </html>
`)}`;

add_task(async function () {
  // Enable the context selector and the frames button.
  await pushPref("devtools.command-button-frames.enabled", true);

  const hud = await openNewTabAndConsole(TEST_URI);

  info("Wait until the iframe picker button is displayed");
  await waitFor(() => getFramesButton(hud.toolbox));

  const evaluationContextSelectorButton = hud.ui.outputNode.querySelector(
    ".webconsole-evaluation-selector-button"
  );
  await executeAndWaitForResultMessage(
    hud,
    "document.location.host",
    `"example.com"`
  );
  ok(true, "The expression was evaluated in the example.com document.");

  info("Select the example.org iframe");
  selectFrameInIframePicker(hud.toolbox, "https://example.org");
  await waitFor(() =>
    evaluationContextSelectorButton.innerText.includes("example.org")
  );
  ok(true, "The context was set to the example.org document");

  await executeAndWaitForResultMessage(
    hud,
    "document.location.host",
    `"example.org"`
  );
  ok(true, "The expression was evaluated in the example.org document.");

  info("Select the example.net iframe");
  selectFrameInIframePicker(hud.toolbox, "https://example.net");
  await waitFor(() =>
    evaluationContextSelectorButton.innerText.includes("example.net")
  );
  ok(true, "The context was set to the example.net document");

  await executeAndWaitForResultMessage(
    hud,
    "document.location.host",
    `"example.net"`
  );
  ok(true, "The expression was evaluated in the example.net document.");

  info("Select the Top frame");
  selectFrameInIframePicker(hud.toolbox, "https://example.com");
  await waitFor(() =>
    evaluationContextSelectorButton.innerText.includes("Top")
  );
  ok(true, "The context was set to the example.com document");

  await executeAndWaitForResultMessage(
    hud,
    "document.location.host",
    `"example.com"`
  );
  ok(true, "The expression was evaluated in the example.com document.");
});

function getFramesButton(toolbox) {
  return toolbox.doc.getElementById("command-button-frames");
}

function selectFrameInIframePicker(toolbox, host) {
  const commandItem = Array.from(
    toolbox.doc.querySelectorAll("#toolbox-frame-menu .command .label")
  ).find(label => label.textContent.startsWith(host));
  if (!commandItem) {
    throw new Error(`Couldn't find any frame starting with "${host}"`);
  }

  commandItem.click();
}
