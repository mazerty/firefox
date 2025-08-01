/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

/* File locked Zucchini partial MAR file staged patch apply failure test */

async function run_test() {
  if (!setupTestCommon()) {
    return;
  }
  const STATE_AFTER_STAGE = gIsServiceTest ? STATE_PENDING_SVC : STATE_PENDING;
  gTestFiles = gTestFilesPartialSuccess;
  gTestDirs = gTestDirsPartialSuccess;
  setTestFilesAndDirsForFailure();
  await setupUpdaterTest(FILE_PARTIAL_ZUCCHINI_MAR, false);
  await runHelperLockFile(getTestFileByName("searchpluginspng1.png"));
  await stageUpdate(STATE_AFTER_STAGE, true);
  checkPostUpdateRunningFile(false);
  // Files aren't checked after staging since this test locks a file which
  // prevents reading the file.
  checkUpdateLogContains(ERR_ENSURE_COPY);
  // Switch the application to the staged application that was updated.
  runUpdate(STATE_FAILED_READ_ERROR, false, 1, false);
  await waitForHelperExit();
  await testPostUpdateProcessing();
  checkPostUpdateRunningFile(false);
  checkFilesAfterUpdateFailure(getApplyDirFile);
  checkUpdateLogContains(ERR_UNABLE_OPEN_DEST);
  checkUpdateLogContains(STATE_FAILED_READ_ERROR + "\n" + CALL_QUIT);
  await waitForUpdateXMLFiles();
  await checkUpdateManager(STATE_NONE, false, STATE_FAILED, READ_ERROR, 1);
  checkCallbackLog();
}
