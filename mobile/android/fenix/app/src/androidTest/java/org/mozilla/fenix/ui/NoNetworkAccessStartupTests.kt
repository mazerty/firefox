/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.ui

import androidx.compose.ui.test.junit4.AndroidComposeTestRule
import androidx.core.net.toUri
import androidx.test.filters.SdkSuppress
import org.junit.Ignore
import org.junit.Rule
import org.junit.Test
import org.mozilla.fenix.customannotations.SmokeTest
import org.mozilla.fenix.helpers.AppAndSystemHelper.setNetworkEnabled
import org.mozilla.fenix.helpers.HomeActivityTestRule
import org.mozilla.fenix.helpers.TestSetup
import org.mozilla.fenix.ui.robots.browserScreen
import org.mozilla.fenix.ui.robots.homeScreen
import org.mozilla.fenix.ui.robots.navigationToolbar

/**
 * Tests to verify some main UI flows with Network connection off
 *
 */

class NoNetworkAccessStartupTests : TestSetup() {

    @get:Rule
    val composeTestRule =
        AndroidComposeTestRule(
            HomeActivityTestRule.withDefaultSettingsOverrides(launchActivity = false),
        ) { it.activity }

    // Test running on beta/release builds in CI:
    // caution when making changes to it, so they don't block the builds
    // Based on STR from https://github.com/mozilla-mobile/fenix/issues/16886
    // TestRail link: https://mozilla.testrail.io/index.php?/cases/view/2240542
    @Test
    fun noNetworkConnectionStartupTest() {
        setNetworkEnabled(false)

        composeTestRule.activityRule.launchActivity(null)

        homeScreen {
            verifyHomeScreen()
        }
    }

    // Based on STR from https://github.com/mozilla-mobile/fenix/issues/16886
    // TestRail link: https://mozilla.testrail.io/index.php?/cases/view/2240722
    @Test
    fun networkInterruptedFromBrowserToHomeTest() {
        val url = "example.com"

        composeTestRule.activityRule.launchActivity(null)

        navigationToolbar {
        }.enterURLAndEnterToBrowser(url.toUri()) {}

        setNetworkEnabled(false)

        browserScreen {
        }.goToHomescreen(composeTestRule) {
            verifyHomeScreen()
        }
    }

    // TestRail link: https://mozilla.testrail.io/index.php?/cases/view/2240723
    @Test
    fun testPageReloadAfterNetworkInterrupted() {
        val url = "example.com"

        composeTestRule.activityRule.launchActivity(null)

        navigationToolbar {
        }.enterURLAndEnterToBrowser(url.toUri()) {}

        setNetworkEnabled(false)

        browserScreen {
        }.openThreeDotMenu {
        }.refreshPage { }
    }

    // TestRail link: https://mozilla.testrail.io/index.php?/cases/view/2240721
    @SdkSuppress(minSdkVersion = 34)
    @SmokeTest
    @Test
    fun testSignInPageWithNoNetworkConnection() {
        setNetworkEnabled(false)

        composeTestRule.activityRule.launchActivity(null)

        homeScreen {
        }.openThreeDotMenu {
        }.openSettings {
        }.openTurnOnSyncMenu {
            tapOnUseEmailToSignIn()
            browserScreen {
                verifyUrl("firefox.com")
            }
        }
    }
}
