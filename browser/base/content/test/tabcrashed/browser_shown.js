"use strict";

const PAGE =
  "data:text/html,<html><body>A%20regular,%20everyday,%20normal%20page.";
const COMMENTS = "Here's my test comment!";

// Avoid timeouts, as in bug 1325530
requestLongerTimeout(2);

add_setup(async function () {
  await setupLocalCrashReportServer();
});

/**
 * This function returns a Promise that resolves once the following
 * actions have taken place:
 *
 * 1) A new tab is opened up at PAGE
 * 2) The tab is crashed
 * 3) The about:tabcrashed page's fields are set in accordance with
 *    fieldValues
 * 4) The tab is restored
 * 5) A crash report is received from the testing server
 * 6) Any tab crash prefs that were overwritten are reset
 *
 * @param fieldValues
 *        An Object describing how to set the about:tabcrashed
 *        fields. The following properties are accepted:
 *
 *        comments (String)
 *          The comments to put in the comment textarea
 *        includeURL (bool)
 *          The checked value of the "Include URL" checkbox
 *
 *        If any of these fields are missing, the defaults from
 *        the user preferences are used.
 * @param expectedExtra
 *        An Object describing the expected values that the submitted
 *        crash report's extra data should contain.
 * @returns Promise
 */
function crashTabTestHelper(fieldValues, expectedExtra, shouldFail = false) {
  return BrowserTestUtils.withNewTab(
    {
      gBrowser,
      url: PAGE,
    },
    async function (browser) {
      let prefs = TabCrashHandler.prefs;
      let originalSendReport = prefs.getBoolPref("sendReport");
      let originalIncludeURL = prefs.getBoolPref("includeURL");

      let tab = gBrowser.getTabForBrowser(browser);
      await BrowserTestUtils.crashFrame(browser);
      let doc = browser.contentDocument;

      // Since about:tabcrashed will run in the parent process, we can safely
      // manipulate its DOM nodes directly
      let comments = doc.getElementById("comments");
      let includeURL = doc.getElementById("includeURL");

      if (fieldValues.hasOwnProperty("comments")) {
        comments.value = fieldValues.comments;
      }

      if (fieldValues.hasOwnProperty("includeURL")) {
        includeURL.checked = fieldValues.includeURL;
      }

      let crashReport = shouldFail
        ? promiseCrashReportFail()
        : promiseCrashReport(expectedExtra);
      let restoreTab = browser.contentDocument.getElementById("restoreTab");
      restoreTab.click();
      await BrowserTestUtils.waitForEvent(tab, "SSTabRestored");
      await crashReport;

      // Submitting the crash report may have set some prefs regarding how to
      // send tab crash reports. Let's reset them for the next test.
      prefs.setBoolPref("sendReport", originalSendReport);
      prefs.setBoolPref("includeURL", originalIncludeURL);
    }
  );
}

/**
 * Tests what we send with the crash report by default. By default, we do not
 * send any comments or the URL of the crashing page.
 */
add_task(async function test_default() {
  let submissionBefore = Glean.crashSubmission.success.testGetValue();
  await crashTabTestHelper(
    {},
    {
      SubmittedFrom: "CrashedTab",
      Throttleable: "1",
      Comments: null,
      URL: "",
    }
  );
  Assert.equal(
    submissionBefore + 1,
    Glean.crashSubmission.success.testGetValue()
  );
});

/**
 * Test just sending a comment.
 */
add_task(async function test_just_a_comment() {
  let submissionBefore = Glean.crashSubmission.success.testGetValue();
  await crashTabTestHelper(
    {
      SubmittedFrom: "CrashedTab",
      Throttleable: "1",
      comments: COMMENTS,
    },
    {
      Comments: COMMENTS,
      URL: "",
    }
  );
  Assert.equal(
    submissionBefore + 1,
    Glean.crashSubmission.success.testGetValue()
  );
});

/**
 * Test that we will send the URL of the page if includeURL is checked.
 */
add_task(async function test_send_URL() {
  let submissionBefore = Glean.crashSubmission.success.testGetValue();
  await crashTabTestHelper(
    {
      SubmittedFrom: "CrashedTab",
      Throttleable: "1",
      includeURL: true,
    },
    {
      Comments: null,
      URL: PAGE,
    }
  );
  Assert.equal(
    submissionBefore + 1,
    Glean.crashSubmission.success.testGetValue()
  );
});

/**
 * Test that we can send comments and the URL
 */
add_task(async function test_send_all() {
  let successBefore = Glean.crashSubmission.success.testGetValue();
  let failureBefore = Glean.crashSubmission.failure.testGetValue();
  await crashTabTestHelper(
    {
      SubmittedFrom: "CrashedTab",
      Throttleable: "1",
      includeURL: true,
      comments: COMMENTS,
    },
    {
      Comments: COMMENTS,
      URL: PAGE,
    }
  );
  Assert.equal(successBefore + 1, Glean.crashSubmission.success.testGetValue());
  Assert.equal(failureBefore, Glean.crashSubmission.failure.testGetValue());
});

add_task(async function test_send_error() {
  let successBefore = Glean.crashSubmission.success.testGetValue();
  let failureBefore = Glean.crashSubmission.failure.testGetValue();
  const eventsBefore = Glean.crashSubmission.failureEvent.testGetValue() ?? [];

  Assert.equal(
    null,
    Glean.crashSubmission.collectorErrors.unknown_error.testGetValue()
  );
  let invalidAnnotation =
    Glean.crashSubmission.collectorErrors.malformed_invalid_annotation_value_400.testGetValue();

  await crashTabTestHelper(
    {
      SubmittedFrom: "CrashedTab",
      Throttleable: "0",
      comments: "fail-me://400:malformed_invalid_annotation_value",
    },
    {
      Comments: "fail-me://400:malformed_invalid_annotation_value",
    },
    /* shouldFail */ true
  );
  Assert.equal(successBefore, Glean.crashSubmission.success.testGetValue());
  Assert.equal(failureBefore + 1, Glean.crashSubmission.failure.testGetValue());

  Assert.equal(
    null,
    Glean.crashSubmission.collectorErrors.unknown_error.testGetValue()
  );
  Assert.equal(
    invalidAnnotation + 1,
    Glean.crashSubmission.collectorErrors.malformed_invalid_annotation_value_400.testGetValue()
  );

  const events = Glean.crashSubmission.failureEvent.testGetValue();
  Assert.equal(eventsBefore.length + 1, events.length);
  Assert.equal(
    "received bad response: 400 Discarded=malformed_invalid_annotation_value",
    events[events.length - 1].extra.reason
  );
  Assert.greater(events[events.length - 1].extra.id.length, 0, "id not set");
});

add_task(async function test_send_error_server() {
  let successBefore = Glean.crashSubmission.success.testGetValue();
  let failureBefore = Glean.crashSubmission.failure.testGetValue();
  let unknownError503 =
    Glean.crashSubmission.collectorErrors.unknown_error_503.testGetValue();
  const eventsBefore = Glean.crashSubmission.failureEvent.testGetValue() ?? [];

  await crashTabTestHelper(
    {
      SubmittedFrom: "CrashedTab",
      Throttleable: "0",
      comments: "fail-me://503:should_get_unkown_error",
    },
    {
      Comments: "fail-me://503:should_get_unkown_error",
    },
    /* shouldFail */ true
  );
  Assert.equal(successBefore, Glean.crashSubmission.success.testGetValue());
  Assert.equal(failureBefore + 1, Glean.crashSubmission.failure.testGetValue());

  Assert.equal(
    unknownError503 + 1,
    Glean.crashSubmission.collectorErrors.unknown_error_503.testGetValue()
  );

  const events = Glean.crashSubmission.failureEvent.testGetValue();
  Assert.equal(eventsBefore.length + 1, events.length);
  Assert.equal(
    "received bad response: 503 Discarded=unknown_error",
    events[events.length - 1].extra.reason
  );
});
