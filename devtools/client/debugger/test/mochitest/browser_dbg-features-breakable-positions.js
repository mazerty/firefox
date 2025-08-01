/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

"use strict";

const testServer = createVersionizedHttpTestServer(
  "examples/sourcemaps-reload-uncompressed"
);
const TEST_URL = testServer.urlFor("index.html");

// getTokenFromPosition pauses 0.5s for each line,
// so this test is quite slow to complete
requestLongerTimeout(10);

/**
 * Cover the breakpoints positions/columns:
 * - assert that the UI displayed markers in CodeMirror next to each breakable columns,
 * - assert the data in the reducers about the breakable columns.
 *
 * Note that it doesn't assert that the breakpoint can be hit.
 * It only verify data integrity and the UI.
 */
add_task(async function testBreakableLinesOverReloads() {
  const dbg = await initDebuggerWithAbsoluteURL(
    TEST_URL,
    "index.html",
    "script.js",
    "original.js"
  );

  info("Assert breakable lines of the first html page load");
  await assertBreakablePositions(dbg, "index.html", 85, [
    { line: 16, columns: [6, 14] },
    { line: 17, columns: [] },
    { line: 21, columns: [12, 20, 48] },
    { line: 24, columns: [12, 20] },
    { line: 25, columns: [] },
    { line: 30, columns: [] },
    { line: 36, columns: [] },
    { line: 39, columns: [] },
    { line: 41, columns: [8, 18] },
    { line: 42, columns: [] },
    { line: 43, columns: [] },
  ]);

  info("Pretty print first html page load and assert breakable lines");
  await togglePrettyPrint(dbg);
  await assertBreakablePositions(dbg, "index.html:formatted", 96, [
    { line: 16, columns: [0, 8] },
    { line: 22, columns: [0, 8, 35] },
    { line: 27, columns: [0, 8] },
    { line: 28, columns: [] },
    { line: 36, columns: [] },
    { line: 48, columns: [] },
    { line: 50, columns: [2, 12] },
    { line: 53, columns: [] },
  ]);
  await closeTab(dbg, "index.html:formatted");

  info("Assert breakable lines of the first original source file, original.js");
  // The length of original.js is longer than the test file
  // because the sourcemap replaces the content of the original file
  // and appends a few lines with a "WEBPACK FOOTER" comment
  // All the appended lines are empty lines or comments, so none of them are breakable.
  await assertBreakablePositions(dbg, "original.js", 15, [
    { line: 1, columns: [] },
    { line: 2, columns: [2, 9, 32] },
    { line: 3, columns: [] },
    { line: 5, columns: [] },
    { line: 8, columns: [2, 8] },
    { line: 9, columns: [2, 10] },
    { line: 10, columns: [] },
  ]);

  info("Assert breakable lines of the simple first load of script.js");
  await assertBreakablePositions(dbg, "script.js", 9, [
    { line: 1, columns: [0, 8] },
    { line: 5, columns: [2, 10] },
    { line: 7, columns: [2, 9] },
    { line: 8, columns: [] },
    { line: 9, columns: [] },
  ]);

  info("Pretty print first load of script.js and assert breakable lines");
  await togglePrettyPrint(dbg);
  await assertBreakablePositions(dbg, "script.js:formatted", 8, [
    { line: 1, columns: [0, 8] },
    { line: 4, columns: [2, 10] },
    { line: 6, columns: [2, 9] },
    { line: 7, columns: [] },
  ]);
  await closeTab(dbg, "script.js:formatted");

  info(
    "Reload the page, wait for sources and assert that breakable lines get updated"
  );
  testServer.switchToNextVersion();
  await reload(dbg, "index.html", "script.js", "original.js");

  info("Assert breakable lines of the more complex second load of script.js");
  await assertBreakablePositions(dbg, "script.js", 23, [
    { line: 2, columns: [0, 8] },
    { line: 13, columns: [4, 12] },
    { line: 14, columns: [] },
    { line: 15, columns: [] },
    { line: 16, columns: [] },
    { line: 17, columns: [] },
    { line: 18, columns: [2, 10] },
    { line: 19, columns: [] },
    { line: 20, columns: [] },
    { line: 21, columns: [] },
    { line: 22, columns: [] },
    { line: 23, columns: [] },
  ]);

  info("Pretty print first load of script.js and assert breakable lines");
  await togglePrettyPrint(dbg);
  await assertBreakablePositions(dbg, "script.js:formatted", 23, [
    { line: 2, columns: [0, 8] },
    { line: 13, columns: [4, 12] },
    { line: 14, columns: [] },
    { line: 15, columns: [] },
    { line: 16, columns: [] },
    { line: 17, columns: [] },
    { line: 18, columns: [2, 10] },
    { line: 19, columns: [] },
    { line: 20, columns: [] },
    { line: 21, columns: [] },
    { line: 22, columns: [] },
  ]);
  await closeTab(dbg, "script.js:formatted");

  info("Assert breakable lines of the second html page load");
  await assertBreakablePositions(dbg, "index.html", 33, [
    { line: 25, columns: [6, 14] },
    { line: 27, columns: [] },
  ]);

  info("Pretty print second html page load and assert breakable lines");
  await togglePrettyPrint(dbg);
  await assertBreakablePositions(dbg, "index.html:formatted", 33, [
    { line: 25, columns: [0, 8] },
  ]);
  await closeTab(dbg, "index.html:formatted");

  info("Assert breakable lines of the second orignal file");
  // See first assertion about original.js,
  // the size of original.js doesn't match the size of the test file
  await assertBreakablePositions(dbg, "original.js", 18, [
    { line: 1, columns: [] },
    { line: 2, columns: [2, 9, 32] },
    { line: 3, columns: [] },
    { line: 8, columns: [] },
    { line: 9, columns: [2, 8] },
    { line: 10, columns: [2, 10] },
    { line: 11, columns: [] },
    { line: 13, columns: [0, 8] },
  ]);
});

async function assertBreakablePositions(
  dbg,
  file,
  numberOfLines,
  breakablePositions
) {
  await selectSource(dbg, file);
  is(
    getLineCount(dbg),
    numberOfLines,
    `We show the expected number of lines in CodeMirror for ${file}`
  );
  for (let line = 1; line <= numberOfLines; line++) {
    info(`Asserting line #${line}`);
    const positions = breakablePositions.find(
      position => position.line == line
    );
    // If we don't have any position, only assert that the line isn't breakable
    if (!positions) {
      await assertLineIsBreakable(dbg, file, line, false);
      continue;
    }
    const { columns } = positions;
    // Otherwise, set a breakpoint on the line to ensure we force fetching the breakable columns per line
    // (this is only fetch on-demand)
    await addBreakpointViaGutter(dbg, line);
    await assertBreakpoint(dbg, line);
    const source = findSource(dbg, file);

    // If there is no column breakpoint, skip all further assertions
    // Last lines of inline script are reported as breakable lines and selectors reports
    // one breakable column, but, we don't report any available column breakpoint for them.
    if (!columns.length) {
      // So, only ensure that there really is no marker on this line
      const lineElement = await getTokenFromPosition(dbg, { line });
      const columnMarkers = lineElement.querySelectorAll(".column-breakpoint");
      is(
        columnMarkers.length,
        0,
        `There is no breakable columns on line ${line}`
      );
      await removeBreakpoint(dbg, source.id, line);
      continue;
    }

    const selectorPositions = dbg.selectors.getBreakpointPositionsForSource(
      source.id
    );
    ok(selectorPositions, "Selector returned positions");
    const selectorPositionsForLine = selectorPositions[line];
    ok(selectorPositionsForLine, "Selector returned positions for the line");
    is(
      selectorPositionsForLine.length,
      columns.length,
      "Selector has the expected number of breakable columns"
    );
    for (const selPos of selectorPositionsForLine) {
      is(
        selPos.location.line,
        line,
        "Selector breakable column has the right line"
      );
      ok(
        columns.includes(selPos.location.column),
        `Selector breakable column has an expected column (${
          selPos.location.column
        } in ${JSON.stringify(columns)}) for line ${line}`
      );
      is(
        selPos.location.source.id,
        source.id,
        "Selector breakable column has the right source id"
      );
      is(
        selPos.location.source.url,
        source.url,
        "Selector breakable column has the right source url"
      );
    }

    const tokenElement = await getTokenFromPosition(dbg, { line });
    const lineElement = tokenElement.closest(".cm-line");
    // Those are the breakpoint chevron we click on to set a breakpoint on a given column
    const columnMarkers = [
      ...lineElement.querySelectorAll(".column-breakpoint"),
    ];
    is(
      columnMarkers.length,
      columns.length,
      "Got the expeced number of column markers"
    );

    // The first breakable column received the line breakpoint when calling addBreakpoint()
    const firstColumn = columns[0];
    ok(
      findColumnBreakpoint(dbg, file, line, firstColumn),
      `The first column ${firstColumn} has a breakpoint automatically`
    );

    for (const [index, column] of Object.entries(columns)) {
      const columnMarkerIndex = Number(index);
      // The first column breakpoint is shifted
      if (columnMarkerIndex == 0) {
        continue;
      }
      ok(
        !findColumnBreakpoint(dbg, file, line, column),
        `Before clicking on the marker, the column ${column} was not having a breakpoint`
      );

      const onSetBreakpoint = waitForDispatch(dbg.store, "SET_BREAKPOINT");
      let marker = getColumnMarker(lineElement, columnMarkerIndex);
      marker.click();
      await onSetBreakpoint;
      ok(
        findColumnBreakpoint(dbg, file, line, column),
        `Was able to set column breakpoint for ${file} @ ${line}:${column}`
      );

      const onRemoveBreakpoint = waitForDispatch(
        dbg.store,
        "REMOVE_BREAKPOINT"
      );
      marker = getColumnMarker(lineElement, columnMarkerIndex);
      marker.click();
      await onRemoveBreakpoint;

      ok(
        !findColumnBreakpoint(dbg, file, line, column),
        `Removed the just-added column breakpoint`
      );
    }

    await removeBreakpoint(dbg, source.id, line);
  }
}

function getColumnMarker(lineElement, index) {
  return lineElement.querySelectorAll(".column-breakpoint")[index];
}
