/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

"use strict";

const TEST_URL = `${URL_ROOT_COM_SSL}touch_iframe_parent.html`;
const TEST_URL2 = `${URL_ROOT_COM_SSL}touch_iframe_parent_desktop.html`;
const REMOTE_IFRAME_URL = `${URL_ROOT_ORG_SSL}touch_iframe_child.html`;

const EXPECTED_TAP_EVENTS =
  "pointerdown touchstart pointerup touchend mousemove mousedown mouseup click";
const EXPECTED_DRAG_EVENTS =
  "pointerdown touchstart pointermove touchmove pointerup touchend";
const EXPECTED_DOUBLE_TAP_EVENTS = `${EXPECTED_TAP_EVENTS} ${EXPECTED_TAP_EVENTS} dblclick`;
const EXPECTED_DOUBLE_TAP_ZOOM_EVENTS = `pointerdown touchstart pointerup touchend pointerdown touchstart pointerup touchend`;
const EXPECTED_LONG_TAP_EVENTS = `pointerdown touchstart mousemove contextmenu pointerup touchend`;

addRDMTask(TEST_URL, async function ({ ui }) {
  reloadOnTouchChange(true);
  await toggleTouchSimulation(ui);
  await runTests(
    ui,
    ["topFrame", "localIFrame", "remoteIFrame"],
    ["tap", "drag", "double_tap", "long_tap"]
  );
  await toggleTouchSimulation(ui);
});

// The following tests change the page's zoom state, so we run each of them
// separately to ensure they don't interfere with each other.
for (const frameName of ["topFrame", "localIFrame", "remoteIFrame"]) {
  addRDMTask(TEST_URL2, async function ({ ui }) {
    reloadOnTouchChange(true);
    await toggleTouchSimulation(ui);
    await runTests(ui, [frameName], ["double_tap_zoom"]);
    await toggleTouchSimulation(ui);
  });
}

function runTests(ui, frameNames, tests) {
  return spawnViewportTask(
    ui,
    {
      tests,
      frameNames,
      REMOTE_IFRAME_URL,
      EXPECTED_TAP_EVENTS,
      EXPECTED_DOUBLE_TAP_EVENTS,
      EXPECTED_DOUBLE_TAP_ZOOM_EVENTS,
      EXPECTED_LONG_TAP_EVENTS,
      EXPECTED_DRAG_EVENTS,
    },
    async args => {
      const frames = {
        topFrame: content.document.body,
        localIFrame: content.document.getElementById("local-iframe"),
        remoteIFrame: content.document.getElementById("remote-iframe"),
      };

      // Load the remote iframe and wait for it to be loaded.
      frames.remoteIFrame.src = args.REMOTE_IFRAME_URL;
      await ContentTaskUtils.waitForEvent(frames.remoteIFrame, "load");
      // Wait for the remote iframe to be ready to receive events.
      await SpecialPowers.spawn(frames.remoteIFrame, [], async () => {
        await SpecialPowers.contentTransformsReceived(content);
      });

      const body = content.document.body;

      async function checkEventLog(expectedFrame, expectedEvents, test) {
        const lastExpectedEvent = expectedEvents.substring(
          expectedEvents.lastIndexOf(" ") + 1
        );
        await waitForEventFromFrame(expectedFrame, lastExpectedEvent, 2000);
        // wait some more to ensure there are no unexpected delayed events
        await waitForTime(500);

        for (const frameName of ["topFrame", "localIFrame", "remoteIFrame"]) {
          const expected =
            frameName === expectedFrame ? expectedEvents : undefined;
          is(
            body.dataset[frameName],
            expected,
            `${frameName} received the expected events in the ${test} test`
          );
        }
      }

      function clearEventLog() {
        for (const key of Object.keys(body.dataset)) {
          delete body.dataset[key];
        }
      }

      function waitForTime(ms) {
        return new Promise(resolve => {
          content.setTimeout(resolve, ms);
        });
      }

      function synthesizeMouseEvent(target, type, offsetX, offsetY) {
        return new Promise(resolve => {
          EventUtils.synthesizeNativeMouseEvent(
            {
              target,
              type,
              offsetX,
              offsetY,
              win: content,
            },
            resolve
          );
        });
      }

      async function synthesizeClick(target, waitFor) {
        await synthesizeMouseEvent(target, "mousedown", 50, 50);
        if (waitFor) {
          await waitFor();
        }
        await synthesizeMouseEvent(target, "mouseup", 50, 50);
      }

      async function waitForEventFromFrame(frameName, type, timeout = 100) {
        await ContentTaskUtils.waitForCondition(
          () => body.dataset[frameName]?.split(" ").includes(type),
          `Waiting for ${type} event from ${frameName}`,
          10,
          timeout / 10
        );
      }

      for (const frameName of args.frameNames) {
        const frame = frames[frameName];
        for (const test of args.tests) {
          clearEventLog();
          switch (test) {
            case "tap": {
              await synthesizeClick(frame);
              await checkEventLog(frameName, args.EXPECTED_TAP_EVENTS, test);
              break;
            }

            case "drag": {
              await synthesizeMouseEvent(frame, "mousedown", 50, 50);
              await synthesizeMouseEvent(frame, "mousemove", 60, 50);
              // wait for the touchmove event to be received before synthesizing the next one
              // to ensure the received events are in the expected order
              await waitForEventFromFrame(frameName, "touchmove");
              await synthesizeMouseEvent(frame, "mouseup", 60, 50);
              await checkEventLog(frameName, args.EXPECTED_DRAG_EVENTS, test);
              break;
            }

            case "long_tap": {
              await synthesizeClick(frame, () =>
                waitForEventFromFrame(frameName, "contextmenu", 1000)
              );
              await checkEventLog(
                frameName,
                args.EXPECTED_LONG_TAP_EVENTS,
                test
              );
              break;
            }

            case "double_tap": {
              await synthesizeClick(frame);
              // wait for the click event to be received before synthesizing the next one
              // to ensure the received events are in the expected order
              await waitForEventFromFrame(frameName, "click");
              await synthesizeClick(frame);
              await checkEventLog(
                frameName,
                args.EXPECTED_DOUBLE_TAP_EVENTS,
                test
              );
              break;
            }

            case "double_tap_zoom": {
              await synthesizeClick(frame);
              await synthesizeClick(frame);
              await checkEventLog(
                frameName,
                args.EXPECTED_DOUBLE_TAP_ZOOM_EVENTS,
                test
              );
              break;
            }
          }
        }
      }
    }
  );
}
