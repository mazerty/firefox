/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

/* File in use Zucchini partial MAR file staged patch apply failure test */

async function run_test() {
  if (!setupTestCommon()) {
    return;
  }
  const STATE_AFTER_STAGE = gIsServiceTest ? STATE_APPLIED_SVC : STATE_APPLIED;
  const STATE_AFTER_RUNUPDATE = gIsServiceTest
    ? STATE_PENDING_SVC
    : STATE_PENDING;
  gTestFiles = gTestFilesPartialSuccess;
  gTestDirs = gTestDirsPartialSuccess;
  await setupUpdaterTest(FILE_PARTIAL_ZUCCHINI_MAR, false);
  const testFile = getTestFileByName("0exe0.exe");
  await runHelperFileInUse(testFile.relPathDir + testFile.fileName, false);
  await stageUpdate(STATE_AFTER_STAGE, true);
  checkPostUpdateRunningFile(false);
  checkFilesAfterUpdateSuccess(getStageDirFile, true);
  checkUpdateLogContents(LOG_PARTIAL_SUCCESS, true);
  // Switch the application to the staged application that was updated.
  runUpdate(STATE_AFTER_RUNUPDATE, true, 1, true);
  await waitForHelperExit();
  await testPostUpdateProcessing();
  checkPostUpdateRunningFile(false);
  setTestFilesAndDirsForFailure();
  checkFilesAfterUpdateFailure(getApplyDirFile);
  checkUpdateLogContains(ERR_RENAME_FILE);
  checkUpdateLogContains(
    ERR_MOVE_DESTDIR_7 + "\n" + STATE_FAILED_WRITE_ERROR + "\n" + CALL_QUIT
  );
  await waitForUpdateXMLFiles(true, false);
  await checkUpdateManager(
    STATE_AFTER_RUNUPDATE,
    true,
    STATE_AFTER_RUNUPDATE,
    0,
    0
  );
  checkCallbackLog();
}
