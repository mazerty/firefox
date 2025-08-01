/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.ui.robots

import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.assertion.ViewAssertions.matches
import androidx.test.espresso.intent.Intents
import androidx.test.espresso.intent.matcher.BundleMatchers
import androidx.test.espresso.intent.matcher.IntentMatchers
import androidx.test.espresso.matcher.ViewMatchers.hasSibling
import androidx.test.espresso.matcher.ViewMatchers.isDisplayed
import androidx.test.espresso.matcher.ViewMatchers.withId
import androidx.test.espresso.matcher.ViewMatchers.withText
import androidx.test.uiautomator.By
import androidx.test.uiautomator.UiSelector
import androidx.test.uiautomator.Until
import org.hamcrest.Matchers.allOf
import org.mozilla.fenix.R
import org.mozilla.fenix.helpers.AppAndSystemHelper.forceCloseApp
import org.mozilla.fenix.helpers.Constants.TAG
import org.mozilla.fenix.helpers.DataGenerationHelper.getStringResource
import org.mozilla.fenix.helpers.MatcherHelper.assertUIObjectExists
import org.mozilla.fenix.helpers.MatcherHelper.itemContainingText
import org.mozilla.fenix.helpers.MatcherHelper.itemWithResId
import org.mozilla.fenix.helpers.MatcherHelper.itemWithResIdContainingText
import org.mozilla.fenix.helpers.MatcherHelper.itemWithText
import org.mozilla.fenix.helpers.TestAssetHelper.waitingTime
import org.mozilla.fenix.helpers.TestHelper.mDevice
import org.mozilla.fenix.helpers.TestHelper.packageName
import org.mozilla.fenix.helpers.ext.waitNotNull

class ShareOverlayRobot {

    // This function verifies the share layout when more than one tab is shared - a list of tabs is shown
    fun verifyShareTabsOverlay(vararg tabsTitles: String) {
        Log.i(TAG, "verifyShareTabsOverlay: Trying to verify that the share overlay site list is displayed")
        onView(withId(R.id.shared_site_list))
            .check(matches(isDisplayed()))
        Log.i(TAG, "verifyShareTabsOverlay: Verified that the share overlay site list is displayed")
        for (tabs in tabsTitles) {
            Log.i(TAG, "verifyShareTabsOverlay: Trying to verify the shared tab: $tabs favicon and url")
            onView(withText(tabs))
                .check(
                    matches(
                        allOf(
                            hasSibling(withId(R.id.share_tab_favicon)),
                            hasSibling(withId(R.id.share_tab_url)),
                        ),
                    ),
                )
            Log.i(TAG, "verifyShareTabsOverlay: Verified the shared tab: $tabs favicon and url")
        }
    }

    // This function verifies the share layout when a single tab is shared - no tab info shown
    fun verifyShareTabLayout() {
        assertUIObjectExists(
            // Share layout
            itemWithResId("$packageName:id/sharingLayout"),
            // Send to device section
            itemWithResId("$packageName:id/devicesList"),
            // Recently used section
            itemWithResId("$packageName:id/recentAppsContainer"),
            // All actions sections
            itemWithResId("$packageName:id/appsList"),
            // Send to device header
            itemWithResIdContainingText(
                "$packageName:id/accountHeaderText",
                getStringResource(R.string.share_device_subheader),
            ),
            // Recently used header
            itemWithResIdContainingText(
                "$packageName:id/recent_apps_link_header",
                getStringResource(R.string.share_link_recent_apps_subheader),
            ),
            // All actions header
            itemWithResIdContainingText(
                "$packageName:id/apps_link_header",
                getStringResource(R.string.share_link_all_apps_subheader),
            ),
            // Save as PDF button
            itemContainingText(getStringResource(R.string.share_save_to_pdf)),
        )
    }

    // this verifies the Android sharing layout - not customized for sharing tabs
    fun verifyAndroidShareLayout() {
        mDevice.waitNotNull(Until.findObject(By.res("android:id/resolver_list")))
    }

    fun verifySharingWithSelectedApp(appName: String, content: String, subject: String) {
        val sharingApp = mDevice.findObject(UiSelector().text(appName))
        Log.i(TAG, "verifySharingWithSelectedApp: Trying to verify that sharing app: $appName exists")
        if (sharingApp.exists()) {
            Log.i(TAG, "verifySharingWithSelectedApp: Sharing app: $appName exists")
            Log.i(TAG, "verifySharingWithSelectedApp: Trying to click sharing app: $appName and wait for a new window")
            sharingApp.clickAndWaitForNewWindow()
            Log.i(TAG, "verifySharingWithSelectedApp: Clicked sharing app: $appName and waited for a new window")
            verifySharedTabsIntent(content, subject)
            // Close the app after successful verification
            forceCloseApp(appName)
        } else {
            Log.i(TAG, "verifySharingWithSelectedApp: Sharing app: $appName not found.")
        }
    }

    private fun verifySharedTabsIntent(text: String, subject: String) {
        Log.i(TAG, "verifySharedTabsIntent: Trying to verify the intent of the shared tab with text: $text, and subject: $subject")
        Intents.intended(
            allOf(
                IntentMatchers.hasExtra(Intent.EXTRA_TEXT, text),
                IntentMatchers.hasExtra(Intent.EXTRA_SUBJECT, subject),
            ),
        )
        Log.i(TAG, "verifySharedTabsIntent: Verified the intent of the shared tab with text: $text, and subject: $subject")
    }

    fun verifyShareLinkIntent(url: Uri) {
        Log.i(TAG, "verifyShareLinkIntent: Trying to verify that the share intent for link: $url is launched")
        // verify share intent is launched and matched with associated passed in URL
        Intents.intended(
            allOf(
                IntentMatchers.hasAction(Intent.ACTION_CHOOSER),
                IntentMatchers.hasExtras(
                    allOf(
                        BundleMatchers.hasEntry(
                            Intent.EXTRA_INTENT,
                            allOf(
                                IntentMatchers.hasAction(Intent.ACTION_SEND),
                                IntentMatchers.hasType("text/plain"),
                                IntentMatchers.hasExtra(
                                    Intent.EXTRA_TEXT,
                                    url.toString(),
                                ),
                            ),
                        ),
                    ),
                ),
            ),
        )
        Log.i(TAG, "verifyShareLinkIntent: Verified that the share intent for link: $url was launched")
    }

    fun clickSharingApp(appName: String) {
        val sharingApp = itemWithText(appName)
        Log.i(TAG, "clickSharingApp: Trying to verify that sharing app: $appName exists")
        if (sharingApp.exists()) {
            Log.i(TAG, "clickSharingApp: Sharing app: $appName exists")
            Log.i(TAG, "clickSharingApp: Trying to click sharing app: $appName and wait for a new window")
            sharingApp.clickAndWaitForNewWindow()
            Log.i(TAG, "clickSharingApp: Clicked sharing app: $appName and waited for a new window")
        } else {
            Log.i(TAG, "clickSharingApp: Sharing app: $appName not found.")
        }
    }

    class Transition {
        fun clickSaveAsPDF(interact: DownloadRobot.() -> Unit): DownloadRobot.Transition {
            Log.i(TAG, "clickSaveAsPDF: Trying to click the \"SAVE AS PDF\" share overlay button")
            itemContainingText("Save as PDF").click()
            Log.i(TAG, "clickSaveAsPDF: Clicked the \"SAVE AS PDF\" share overlay button")

            DownloadRobot().interact()
            return DownloadRobot.Transition()
        }

        fun clickPrintButton(interact: BrowserRobot.() -> Unit): BrowserRobot.Transition {
            itemWithText("Print").waitForExists(waitingTime)
            Log.i(TAG, "clickPrintButton: Trying to click the \"Print\" share overlay button")
            itemWithText("Print").click()
            Log.i(TAG, "clickPrintButton: Clicked the \"Print\" share overlay button")

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }
    }
}

fun shareOverlay(interact: ShareOverlayRobot.() -> Unit): ShareOverlayRobot.Transition {
    ShareOverlayRobot().interact()
    return ShareOverlayRobot.Transition()
}
