/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.helpers

import android.content.Context
import android.net.Uri
import android.util.Log
import android.view.View
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.ComposeTestRule
import androidx.compose.ui.test.performClick
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.action.ViewActions.longClick
import androidx.test.espresso.intent.Intents
import androidx.test.espresso.matcher.ViewMatchers.hasSibling
import androidx.test.espresso.matcher.ViewMatchers.withChild
import androidx.test.espresso.matcher.ViewMatchers.withId
import androidx.test.espresso.matcher.ViewMatchers.withParent
import androidx.test.espresso.matcher.ViewMatchers.withText
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.By
import androidx.test.uiautomator.By.res
import androidx.test.uiautomator.UiDevice
import androidx.test.uiautomator.UiObject
import androidx.test.uiautomator.UiObject2
import androidx.test.uiautomator.UiObjectNotFoundException
import androidx.test.uiautomator.UiScrollable
import androidx.test.uiautomator.UiSelector
import androidx.test.uiautomator.Until
import mozilla.components.support.ktx.android.content.appName
import org.hamcrest.CoreMatchers.allOf
import org.hamcrest.Matcher
import org.junit.Assert
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.mozilla.fenix.R
import org.mozilla.fenix.compose.snackbar.SNACKBAR_BUTTON_TEST_TAG
import org.mozilla.fenix.compose.snackbar.SNACKBAR_TEST_TAG
import org.mozilla.fenix.helpers.Constants.TAG
import org.mozilla.fenix.helpers.MatcherHelper.assertUIObjectExists
import org.mozilla.fenix.helpers.MatcherHelper.itemContainingText
import org.mozilla.fenix.helpers.TestAssetHelper.waitingTime
import org.mozilla.fenix.helpers.TestAssetHelper.waitingTimeShort
import org.mozilla.fenix.helpers.TestAssetHelper.waitingTimeVeryShort
import org.mozilla.fenix.helpers.ext.waitNotNull

object TestHelper {

    val appContext: Context = InstrumentationRegistry.getInstrumentation().targetContext
    val appName = appContext.appName
    var mDevice: UiDevice = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation())
    val packageName: String = appContext.packageName

    fun scrollToElementByText(text: String): UiScrollable? {
        val appView = UiScrollable(UiSelector().scrollable(true))
        Log.i(TAG, "scrollToElementByText: Waiting for $waitingTimeShort ms for scrollable app view to exist")
        appView.waitForExists(waitingTimeShort)
        Log.i(TAG, "scrollToElementByText: Waited for $waitingTimeShort ms")

        if (appView.exists()) {
            Log.i(TAG, "scrollToElementByText: Scrollable view found, attempting to scroll '$text' into view")
            try {
                appView.scrollTextIntoView(text)
                Log.i(TAG, "scrollToElementByText: Successfully scrolled '$text' into view")
            } catch (e: UiObjectNotFoundException) {
                Log.w(TAG, "scrollToElementByText: Failed to scroll to '$text': ${e.message}")
            }
            return appView
        } else {
            Log.i(TAG, "scrollToElementByText: No scrollable view found — likely all content is visible")
            val target = mDevice.findObject(UiSelector().textContains(text))
            if (!target.exists()) {
                Log.w(TAG, "scrollToElementByText: '$text' not found on screen and scrolling is not possible")
            } else {
                Log.i(TAG, "scrollToElementByText: '$text' is already visible without scrolling")
            }
            return null
        }
    }

    fun longTapSelectItem(url: Uri) {
        mDevice.waitNotNull(
            Until.findObject(By.text(url.toString())),
            waitingTime,
        )
        Log.i(TAG, "longTapSelectItem: Trying to long click item with $url")
        onView(
            allOf(
                withId(R.id.url),
                withText(url.toString()),
            ),
        ).perform(longClick())
        Log.i(TAG, "longTapSelectItem: Long clicked item with $url")
    }

    fun restartApp(activity: HomeActivityIntentTestRule) {
        with(activity) {
            updateCachedSettings()
            Log.i(TAG, "restartApp: Trying to finish the current activity")
            finishActivity()
            Log.i(TAG, "restartApp: Finished the current activity")
            Log.i(TAG, "restartApp: Waiting for device to be idle")
            mDevice.waitForIdle()
            Log.i(TAG, "restartApp: Waited for device to be idle")
            Log.i(TAG, "restartApp: Trying to launch the activity")
            launchActivity(null)
            Log.i(TAG, "restartApp: Launched the activity")
        }
    }

    fun closeApp(activity: HomeActivityIntentTestRule) {
        Log.i(TAG, "closeApp: Trying to finish and remove the task of the current activity")
        activity.activity.finishAndRemoveTask()
        Log.i(TAG, "closeApp: Finished and removed the task of the current activity")
    }

    fun relaunchCleanApp(activity: HomeActivityIntentTestRule) {
        closeApp(activity)
        Log.i(TAG, "relaunchCleanApp: Trying to clear intents state")
        Intents.release()
        Log.i(TAG, "relaunchCleanApp: Cleared intents state")
        Log.i(TAG, "relaunchCleanApp: Trying to launch the activity")
        activity.launchActivity(null)
        Log.i(TAG, "relaunchCleanApp: Launched the activity")
    }

    fun waitUntilObjectIsFound(resourceName: String) {
        mDevice.waitNotNull(
            Until.findObjects(By.res(resourceName)),
            waitingTime,
        )
    }

    fun clickSnackbarButton(composeTestRule: ComposeTestRule, expectedText: String) {
        Log.i(TAG, "clickSnackbarButton: Waiting for compose test rule to be idle")
        composeTestRule.waitForIdle()
        Log.i(TAG, "clickSnackbarButton: Waited for compose test rule to be idle")
        Log.i(TAG, "clickSnackbarButton: Trying to click $expectedText snackbar button")
        composeTestRule.onNode(hasTestTag(SNACKBAR_BUTTON_TEST_TAG) or hasText(expectedText)).performClick()
        Log.i(TAG, "clickSnackbarButton: Clicked $expectedText snackbar button")
    }

    fun waitUntilSnackbarGone() {
        Log.i(TAG, "waitUntilSnackbarGone: Waiting for $waitingTime ms until the snckabar is gone")
        mDevice.findObject(
            UiSelector().resourceId(SNACKBAR_TEST_TAG),
        ).waitUntilGone(waitingTime)
        Log.i(TAG, "waitUntilSnackbarGone: Waited for $waitingTime ms until the snckabar was gone")
    }

    fun verifySnackBarText(expectedText: String) = assertUIObjectExists(itemContainingText(expectedText))

    // exit from Menus to home screen or browser
    fun exitMenu() {
        val menuToolbar =
            mDevice.findObject(UiSelector().resourceId("$packageName:id/navigationToolbar"))
        while (menuToolbar.waitForExists(waitingTimeShort)) {
            Log.i(TAG, "exitMenu: Trying to press the device back button to return to the app home/browser view")
            mDevice.pressBack()
            Log.i(TAG, "exitMenu: Pressed the device back button to return to the app home/browser view")
        }
    }

    fun UiDevice.waitForObjects(obj: UiObject, waitingTime: Long = TestAssetHelper.waitingTime) {
        Log.i(TAG, "waitForObjects: Waiting for device to be idle")
        this.waitForIdle()
        Log.i(TAG, "waitForObjects: Waited for device to be idle")
        Log.i(TAG, "waitForObjects: Waiting for $waitingTime ms to assert that ${obj.selector} is not null")
        Assert.assertNotNull(obj.waitForExists(waitingTime))
        Log.i(TAG, "waitForObjects: Waited for $waitingTime ms and asserted that ${obj.selector} is not null")
    }

    fun hasCousin(matcher: Matcher<View>): Matcher<View> {
        return withParent(
            hasSibling(
                withChild(
                    matcher,
                ),
            ),
        )
    }

    fun verifyLightThemeApplied(expected: Boolean) {
        Log.i(TAG, "verifyLightThemeApplied: Trying to verify that that the \"Light\" theme was applied")
        assertFalse("$TAG: Light theme not selected", expected)
        Log.i(TAG, "verifyLightThemeApplied: Verified that that the \"Light\" theme was applied")
    }

    fun verifyDarkThemeApplied(expected: Boolean) {
        Log.i(TAG, "verifyDarkThemeApplied: Trying to verify that that the \"Dark\" theme was applied")
        assertTrue("$TAG: Dark theme not selected", expected)
        Log.i(TAG, "verifyDarkThemeApplied: Verified that that the \"Dark\" theme was applied")
    }

    fun waitForAppWindowToBeUpdated() {
        Log.i(TAG, "waitForAppWindowToBeUpdated: Waiting for $waitingTimeVeryShort ms for $packageName window to be updated")
        mDevice.waitForWindowUpdate(packageName, waitingTimeVeryShort)
        Log.i(TAG, "waitForAppWindowToBeUpdated: Waited for $waitingTimeVeryShort ms for $packageName window to be updated")
    }

    fun setPortraitDisplayOrientation() {
        Log.i(TAG, "setPortraitDisplayOrientation: Trying to set device display orientation to portrait")
        mDevice.setOrientationPortrait()
        Log.i(TAG, "setPortraitDisplayOrientation: Display orientation was set to portrait")
        Log.i(TAG, "setPortraitDisplayOrientation: Waiting for device to be idle")
        mDevice.waitForIdle()
        Log.i(TAG, "setPortraitDisplayOrientation: Waited for device to be idle")
    }

    fun setLandscapeDisplayOrientation() {
        Log.i(TAG, "setLandscapeDisplayOrientation: Trying to set device display orientation to landscape")
        mDevice.setOrientationLandscape()
        Log.i(TAG, "setLandscapeDisplayOrientation: Display orientation was set to landscape")
        Log.i(TAG, "setLandscapeDisplayOrientation: Waiting for device to be idle")
        mDevice.waitForIdle()
        Log.i(TAG, "setLandscapeDisplayOrientation: Waited for device to be idle")
    }

    val snackbar: UiObject2?
        get() = mDevice.findObject(res(SNACKBAR_TEST_TAG))

    val snackbarButton: UiObject2?
        get() = mDevice.findObject(res(SNACKBAR_BUTTON_TEST_TAG))
}
