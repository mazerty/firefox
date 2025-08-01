/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

@file:Suppress("DEPRECATION")

package org.mozilla.fenix.ui.robots

import android.util.Log
import android.widget.RelativeLayout
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotDisplayed
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.ComposeTestRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.NoMatchingViewException
import androidx.test.espresso.action.ViewActions.click
import androidx.test.espresso.assertion.ViewAssertions.matches
import androidx.test.espresso.matcher.ViewMatchers.Visibility
import androidx.test.espresso.matcher.ViewMatchers.hasDescendant
import androidx.test.espresso.matcher.ViewMatchers.hasSibling
import androidx.test.espresso.matcher.ViewMatchers.isAssignableFrom
import androidx.test.espresso.matcher.ViewMatchers.isCompletelyDisplayed
import androidx.test.espresso.matcher.ViewMatchers.isDescendantOfA
import androidx.test.espresso.matcher.ViewMatchers.isDisplayed
import androidx.test.espresso.matcher.ViewMatchers.withContentDescription
import androidx.test.espresso.matcher.ViewMatchers.withEffectiveVisibility
import androidx.test.espresso.matcher.ViewMatchers.withId
import androidx.test.espresso.matcher.ViewMatchers.withText
import androidx.test.uiautomator.By
import androidx.test.uiautomator.UiScrollable
import androidx.test.uiautomator.UiSelector
import androidx.test.uiautomator.Until
import org.hamcrest.CoreMatchers.allOf
import org.mozilla.fenix.R
import org.mozilla.fenix.components.menu.MenuDialogTestTag.WEB_EXTENSION_ITEM
import org.mozilla.fenix.helpers.Constants.RETRY_COUNT
import org.mozilla.fenix.helpers.Constants.TAG
import org.mozilla.fenix.helpers.Constants.recommendedAddons
import org.mozilla.fenix.helpers.DataGenerationHelper.getStringResource
import org.mozilla.fenix.helpers.HomeActivityIntentTestRule
import org.mozilla.fenix.helpers.MatcherHelper.assertUIObjectExists
import org.mozilla.fenix.helpers.MatcherHelper.assertUIObjectIsGone
import org.mozilla.fenix.helpers.MatcherHelper.itemContainingText
import org.mozilla.fenix.helpers.MatcherHelper.itemWithResId
import org.mozilla.fenix.helpers.MatcherHelper.itemWithResIdContainingText
import org.mozilla.fenix.helpers.MatcherHelper.itemWithText
import org.mozilla.fenix.helpers.TestAssetHelper.waitingTime
import org.mozilla.fenix.helpers.TestAssetHelper.waitingTimeLong
import org.mozilla.fenix.helpers.TestHelper.appName
import org.mozilla.fenix.helpers.TestHelper.mDevice
import org.mozilla.fenix.helpers.TestHelper.packageName
import org.mozilla.fenix.helpers.TestHelper.restartApp
import org.mozilla.fenix.helpers.TestHelper.waitForAppWindowToBeUpdated
import org.mozilla.fenix.helpers.click
import org.mozilla.fenix.helpers.ext.waitNotNull

/**
 * Implementation of Robot Pattern for the Addons Management Settings.
 */

class SettingsSubMenuAddonsManagerRobot {
    fun verifyAddonsListIsDisplayed(shouldBeDisplayed: Boolean) =
        assertUIObjectExists(addonsList(), exists = shouldBeDisplayed)

    fun waitForAddonsListProgressBarToBeGone() = assertUIObjectIsGone(itemWithResId("$packageName:id/add_ons_progress_bar"), waitingTime = waitingTimeLong)

    fun waitForAddonsDownloadOverlayToBeGone() = assertUIObjectIsGone(itemWithResId("$packageName:id/addonProgressOverlay"), waitingTime = waitingTimeLong)

    fun verifyAddonDownloadOverlay() {
        Log.i(TAG, "verifyAddonDownloadOverlay: Trying to verify that the \"Downloading and verifying extension\" prompt is displayed")
        onView(withText(R.string.mozac_extension_install_progress_caption)).check(matches(isDisplayed()))
        Log.i(TAG, "verifyAddonDownloadOverlay: Verified that the \"Downloading and verifying extension\" prompt is displayed")
    }

    fun verifyAddonPermissionPrompt(addonName: String) {
        waitForAddonsDownloadOverlayToBeGone()
        assertUIObjectExists(
            itemWithResIdContainingText(
                "$packageName:id/title",
                "Add $addonName",
            ),
            itemWithResIdContainingText(
                "$packageName:id/optional_or_required_text",
                getStringResource(R.string.addons_permissions_heading_required_permissions),
            ),
            itemWithResIdContainingText(
                "$packageName:id/optional_settings_title",
                getStringResource(R.string.mozac_feature_addons_permissions_dialog_heading_optional_settings),
            ),
            itemWithResIdContainingText(
                "$packageName:id/allow_in_private_browsing",
                getStringResource(R.string.mozac_feature_addons_settings_allow_in_private_browsing_2),
            ),
            itemWithResIdContainingText(
                "$packageName:id/learn_more_link",
                getStringResource(R.string.mozac_feature_addons_permissions_dialog_learn_more),
            ),
            itemWithResIdContainingText(
                "$packageName:id/deny_button",
                getStringResource(R.string.mozac_feature_addons_permissions_dialog_cancel),
            ),
            itemWithResIdContainingText(
                "$packageName:id/allow_button",
                getStringResource(R.string.mozac_feature_addons_permissions_dialog_add),
            ),
        )
    }

    fun clickInstallAddon(addonName: String) {
        for (i in 1..RETRY_COUNT) {
            Log.i(TAG, "verifyAddonAvailableInMainMenu: Started try #$i")
            try {
                Log.i(TAG, "clickInstallAddon: Waiting for $waitingTime ms for add-ons list to exist")
                addonsList().waitForExists(waitingTime)
                Log.i(TAG, "clickInstallAddon: Waited for $waitingTime ms for add-ons list to exist")
                Log.i(TAG, "clickInstallAddon: Trying to scroll into view the install $addonName button")
                addonsList().scrollIntoView(
                    mDevice.findObject(
                        UiSelector()
                            .resourceId("$packageName:id/details_container")
                            .childSelector(UiSelector().text(addonName)),
                    ),
                )
                Log.i(TAG, "clickInstallAddon: Scrolled into view the install $addonName button")
                Log.i(TAG, "clickInstallAddon: Trying to click the install $addonName button")
                installButtonForAddon(addonName).click()
                Log.i(TAG, "clickInstallAddon: Clicked the install $addonName button")

                break
            } catch (e: NoMatchingViewException) {
                Log.i(TAG, "clickInstallAddon: NoMatchingViewException caught, executing fallback methods")
                addonsMenu {
                }.goBackToHomeScreen {
                }.openThreeDotMenu {
                }.openAddonsManagerMenu {
                }
            }
        }
    }

    fun verifyAddonInstallCompletedPrompt(addonName: String, activityTestRule: HomeActivityIntentTestRule) {
        // Assigns a more descriptive name to the addon if it is "Bitwarden", otherwise keeps the original name
        // The name of this extenssion is being displayed differently across the app
        var addonName = if (addonName == "Bitwarden") "Bitwarden Password Manager" else addonName

        for (i in 1..RETRY_COUNT) {
            Log.i(TAG, "verifyAddonInstallCompletedPrompt: Started try #$i")
            try {
                assertUIObjectExists(
                    itemContainingText("$addonName was added"),
                    itemContainingText("Update permissions and data preferences any time in the extension settings."),
                    itemContainingText("OK"),
                    waitingTime = waitingTimeLong,
                )

                break
            } catch (e: AssertionError) {
                Log.i(TAG, "verifyAddonInstallCompletedPrompt: AssertionError caught, executing fallback methods")
                if (i == RETRY_COUNT) {
                    throw e
                } else {
                    restartApp(activityTestRule)
                    homeScreen {
                    }.openThreeDotMenu {
                    }.openAddonsManagerMenu {
                        waitForAddonsListProgressBarToBeGone()
                        scrollToAddon(addonName)
                        clickInstallAddon(addonName)
                        verifyAddonPermissionPrompt(addonName)
                        acceptPermissionToInstallAddon()
                    }
                }
            }
        }
    }

    fun closeAddonInstallCompletePrompt() {
        Log.i(TAG, "closeAddonInstallCompletePrompt: Trying to click the \"OK\" button from the completed add-on install prompt")
        itemWithResIdContainingText(
            "$packageName:id/confirm_button",
            "OK",
        ).click()
        Log.i(TAG, "closeAddonInstallCompletePrompt: Clicked the \"OK\" button from the completed add-on install prompt")
    }

    fun verifyAddonIsInstalled(addonName: String) {
        // Assigns a more descriptive name to the addon if it is "Bitwarden", otherwise keeps the original name
        // The name of this extenssion is being displayed differently across the app
        var addonName = if (addonName == "Bitwarden") "Bitwarden Password Manager" else addonName

        scrollToAddon(addonName)
        Log.i(TAG, "verifyAddonIsInstalled: Trying to verify that the $addonName add-on was installed")
        onView(
            allOf(
                withId(R.id.add_button),
                isDescendantOfA(withId(R.id.add_on_item)),
                hasSibling(hasDescendant(withText(addonName))),
            ),
        ).check(matches(withEffectiveVisibility(Visibility.INVISIBLE)))
        Log.i(TAG, "verifyAddonIsInstalled: Verified that the $addonName add-on was installed")
    }

    fun verifyEnabledTitleDisplayed() {
        Log.i(TAG, "verifyEnabledTitleDisplayed: Trying to verify that the \"Enabled\" heading is displayed")
        onView(withText("Enabled"))
            .check(matches(isCompletelyDisplayed()))
        Log.i(TAG, "verifyEnabledTitleDisplayed: Verified that the \"Enabled\" heading is displayed")
    }

    fun cancelInstallAddon() = cancelInstall()
    fun acceptPermissionToInstallAddon() = allowPermissionToInstall()
    fun verifyAddonsItems() {
        Log.i(TAG, "verifyAddonsItems: Trying to verify that the \"Recommended\" heading is visible")
        onView(allOf(withId(R.id.title), withText("Recommended")))
            .check(matches(withEffectiveVisibility(Visibility.VISIBLE)))
        Log.i(TAG, "verifyAddonsItems: Verified that the \"Recommended\" heading is visible")
        Log.i(TAG, "verifyAddonsItems: Trying to verify that all uBlock Origin items are completely displayed")
        onView(
            allOf(
                isAssignableFrom(RelativeLayout::class.java),
                withId(R.id.add_on_item),
                hasDescendant(allOf(withId(R.id.add_on_icon), isCompletelyDisplayed())),
                hasDescendant(
                    allOf(
                        withId(R.id.details_container),
                        hasDescendant(withText("uBlock Origin")),
                        hasDescendant(withText("Finally, an efficient wide-spectrum content blocker. Easy on CPU and memory.")),
                        hasDescendant(withId(R.id.rating)),
                        hasDescendant(withId(R.id.review_count)),
                    ),
                ),
                hasDescendant(withId(R.id.add_button)),
            ),
        ).check(matches(isCompletelyDisplayed()))
        Log.i(TAG, "verifyAddonsItems: Verified that all uBlock Origin items are completely displayed")
    }
    fun verifyAddonCanBeInstalled(addonName: String) {
        scrollToAddon(addonName)
        mDevice.waitNotNull(Until.findObject(By.text(addonName)), waitingTime)
        Log.i(TAG, "verifyAddonCanBeInstalled: Trying to verify that the install $addonName button is visible")
        onView(
            allOf(
                withId(R.id.add_button),
                hasSibling(
                    hasDescendant(
                        allOf(
                            withId(R.id.add_on_name),
                            withText(addonName),
                        ),
                    ),
                ),
            ),
        ).check(matches(withEffectiveVisibility(Visibility.VISIBLE)))
        Log.i(TAG, "verifyAddonCanBeInstalled: Verified that the install $addonName button is visible")
    }

    fun selectAllowInPrivateBrowsing() {
        assertUIObjectExists(itemWithText("Allow extension to run in private browsing"), waitingTime = waitingTimeLong)
        Log.i(TAG, "selectAllowInPrivateBrowsing: Trying to click the \"Allow in private browsing\" check box")
        onView(withId(R.id.allow_in_private_browsing)).click()
        Log.i(TAG, "selectAllowInPrivateBrowsing: Clicked the \"Allow in private browsing\" check box")
    }

    fun installAddon(addonName: String, activityTestRule: HomeActivityIntentTestRule) {
        homeScreen {
        }.openThreeDotMenu {
        }.openAddonsManagerMenu {
            waitForAddonsListProgressBarToBeGone()
            clickInstallAddon(addonName)
            verifyAddonPermissionPrompt(addonName)
            acceptPermissionToInstallAddon()
            verifyAddonInstallCompletedPrompt(addonName, activityTestRule)
        }
    }

    fun installAddonInPrivateMode(addonName: String, activityTestRule: HomeActivityIntentTestRule) {
        homeScreen {
        }.openThreeDotMenu {
        }.openAddonsManagerMenu {
            waitForAddonsListProgressBarToBeGone()
            clickInstallAddon(addonName)
            verifyAddonPermissionPrompt(addonName)
            selectAllowInPrivateBrowsing()
            acceptPermissionToInstallAddon()
            verifyAddonInstallCompletedPrompt(addonName, activityTestRule)
        }
    }

    fun verifyRecommendedAddonsViewFromRedesignedMainMenu(composeTestRule: ComposeTestRule) {
        verifyTheRecommendedAddons(composeTestRule)
        Log.i(TAG, "verifyRecommendedAddonsViewFromRedesignedMainMenu: Trying to verify that that the \"Discover more extensions\" button is displayed")
        composeTestRule.onNode(
            hasText(getStringResource(R.string.browser_menu_discover_more_extensions)), useUnmergedTree = true,
        ).assertIsDisplayed()
        Log.i(TAG, "verifyRecommendedAddonsViewFromRedesignedMainMenu: Verified that that the \"Discover more extensions\" button is displayed")
    }

    fun verifyNoInstalledExtensionsPromotionBanner(composeTestRule: ComposeTestRule) {
        Log.i(TAG, "verifyNoInstalledExtensionsPromotionBanner: Trying to verify that the \"Make $appName your own\" heading is displayed")
        composeTestRule.onNode(
            hasText("Make $appName your own"),
        ).assertIsDisplayed()
        Log.i(TAG, "verifyNoInstalledExtensionsPromotionBanner: Verified that the \"Make $appName your own\" heading is displayed")
        Log.i(TAG, "verifyNoInstalledExtensionsPromotionBanner: Trying to verify that that the \"Extensions level up your browsing, from changing how $appName looks and performs to boosting privacy and safety.\" message is displayed")
        composeTestRule.onNode(
            hasText("Extensions level up your browsing, from changing how $appName looks and performs to boosting privacy and safety."),
        ).assertIsDisplayed()
        Log.i(TAG, "verifyNoInstalledExtensionsPromotionBanner: Verified that that the \"Extensions level up your browsing, from changing how $appName looks and performs to boosting privacy and safety.\" message is displayed")
        Log.i(TAG, "verifyNoInstalledExtensionsPromotionBanner: Trying to verify that that the \"Learn more\" link is displayed")
        composeTestRule.onNode(
            hasContentDescription("Learn more Links available"),
        ).assertIsDisplayed()
        Log.i(TAG, "verifyNoInstalledExtensionsPromotionBanner: Verified that that the \"Learn more\" link is displayed")
    }

    fun verifyExtensionsEnabledButton(composeTestRule: ComposeTestRule) {
        Log.i(TAG, "verifyExtensionsEnabledButton: Trying to verify that the \"You have extensions installed, but not enabled\" heading is displayed")
        composeTestRule.onNode(
            hasText(getStringResource(R.string.browser_menu_disabled_extensions_banner_onboarding_header)),
        ).assertIsDisplayed()
        Log.i(TAG, "verifyExtensionsEnabledButton: Verified that the \"You have extensions installed, but not enabled\" heading is displayed")
        Log.i(TAG, "verifyDisabledExtensionsPromotionBanner: Trying to verify that that the \"To use extensions, enable them in settings or by selecting “Manage extensions” below.\" message is displayed")
        composeTestRule.onNode(
            hasText("To use extensions, enable them in settings or by selecting “Manage extensions” below."),
        ).assertIsDisplayed()
        Log.i(TAG, "verifyDisabledExtensionsPromotionBanner: Verified that that the \"To use extensions, enable them in settings or by selecting “Manage extensions” below.\" message is displayed")
        Log.i(TAG, "verifyDisabledExtensionsPromotionBanner: Trying to verify that that the \"Learn more\" link is displayed")
        composeTestRule.onNode(
            hasContentDescription("Learn more Links available"),
        ).assertIsDisplayed()
        Log.i(TAG, "verifyDisabledExtensionsPromotionBanner: Verified that that the \"Learn more\" link is displayed")
    }

    fun verifyTheRecommendedAddons(composeTestRule: ComposeTestRule) {
        var verifiedCount = 0

        for (i in 1..RETRY_COUNT) {
            Log.i(TAG, "verifyTheRecommendedAddons: Started try #$i")
            try {
                recommendedAddons.forEach { addon ->
                    if (verifiedCount == 3) return
                    try {
                        waitForAppWindowToBeUpdated()
                        Log.i(TAG, "verifyTheRecommendedAddons: Trying to verify that addon: $addon is recommended and displayed")
                        composeTestRule.onNode(hasText(addon, substring = true))
                            .assertIsDisplayed()
                        Log.i(TAG, "verifyTheRecommendedAddons: Verified that addon: $addon is recommended and displayed")

                        Log.i(TAG, "verifyTheRecommendedAddons: Trying to verify that addon: $addon install button is displayed")
                        composeTestRule.onNode(hasContentDescription("Add $addon", substring = true))
                            .assertIsDisplayed()
                        Log.i(TAG, "verifyTheRecommendedAddons: Verify that addon: $addon install button is displayed")

                        verifiedCount++
                    } catch (e: AssertionError) {
                        Log.i(TAG, "verifyTheRecommendedAddons: Addon: $addon is not displayed, moving to the next one")
                    }
                }
                if (verifiedCount < 3) {
                    throw AssertionError("$TAG, verifyTheRecommendedAddons: Less than 3 addons were verified. Only $verifiedCount addons were verified.")
                }

                break
            } catch (e: AssertionError) {
                Log.i(TAG, "verifyTheRecommendedAddons: AssertionError caught, executing fallback methods")
                if (i == RETRY_COUNT) {
                    throw e
                }
            }
        }
    }

    fun installRecommendedAddon(recommendedExtensionTitle: String, composeTestRule: ComposeTestRule) {
        waitForAppWindowToBeUpdated()
        Log.i(TAG, "installARecommendedAddons: Trying to click addon: $recommendedExtensionTitle install button")
        composeTestRule.onNodeWithContentDescription("Add $recommendedExtensionTitle", substring = true).performClick()
        Log.i(TAG, "installARecommendedAddons: Clicked addon: $recommendedExtensionTitle install button")
    }

    fun verifyManageExtensionsButtonFromRedesignedMainMenu(composeTestRule: ComposeTestRule, isDisplayed: Boolean) {
        if (isDisplayed) {
            Log.i(TAG, "verifyManageExtensionsButtonFromRedesignedMainMenu: Trying to verify that the \"Manage extensions\" button is displayed")
            composeTestRule.onNodeWithText(getStringResource(R.string.browser_menu_manage_extensions), useUnmergedTree = true).assertIsDisplayed()
            Log.i(TAG, "verifyManageExtensionsButtonFromRedesignedMainMenu: Verified that the \"Manage extensions\" button is displayed")
        } else {
            Log.i(TAG, "verifyManageExtensionsButtonFromRedesignedMainMenu: Trying to verify that the \"Manage extensions\" button is not displayed")
            composeTestRule.onNodeWithText(getStringResource(R.string.browser_menu_manage_extensions), useUnmergedTree = true).assertIsNotDisplayed()
            Log.i(TAG, "verifyManageExtensionsButtonFromRedesignedMainMenu: Verified that the \"Manage extensions\" button is not displayed")
        }
    }

    fun clickManageExtensionsButtonFromRedesignedMainMenu(composeTestRule: ComposeTestRule) {
        Log.i(TAG, "clickManageExtensionsButtonFromRedesignedMainMenu: Trying to click the manage extensions button")
        composeTestRule.onNodeWithText(getStringResource(R.string.browser_menu_manage_extensions), useUnmergedTree = true).performClick()
        Log.i(TAG, "clickManageExtensionsButtonFromRedesignedMainMenu: Clicked the manage extensions button")
    }

    fun verifyDiscoverMoreExtensionsButton(composeTestRule: ComposeTestRule, isDisplayed: Boolean) {
        if (isDisplayed) {
            Log.i(TAG, "verifyDiscoverMoreExtensionsButton: Trying to verify that the \"Discover more\" button is displayed")
            composeTestRule.onNode(hasText(getStringResource(R.string.browser_menu_discover_more_extensions)), useUnmergedTree = true).assertIsDisplayed()
            Log.i(TAG, "verifyDiscoverMoreExtensionsButton: Verified that the \"Discover more\" button is displayed")
        } else {
            Log.i(TAG, "verifyDiscoverMoreExtensionsButton: Trying to verify that the \"Discover more\" button is not displayed")
            composeTestRule.onNode(hasText(getStringResource(R.string.browser_menu_discover_more_extensions)), useUnmergedTree = true).assertIsNotDisplayed()
            Log.i(TAG, "verifyDiscoverMoreExtensionsButton: Verified that the \"Discover more\" button is not displayed")
        }
    }

    fun verifyInstalledExtension(composeTestRule: ComposeTestRule, extensionTitle: String) {
        Log.i(TAG, "verifyInstalledExtension: Trying to verify that extension: $extensionTitle is displayed")
        composeTestRule.onNode(
            hasTestTag(WEB_EXTENSION_ITEM),
        ).assert(
            hasContentDescription(extensionTitle, substring = true),
        ).assertIsDisplayed()
        Log.i(TAG, "verifyInstalledExtension: Verified that extension: $extensionTitle is displayed")
    }
    class Transition {
        fun goBackToHomeScreen(interact: HomeScreenRobot.() -> Unit): HomeScreenRobot.Transition {
            Log.i(TAG, "goBackToHomeScreen: Trying to click navigate up toolbar button")
            onView(allOf(withContentDescription("Navigate up"))).click()
            Log.i(TAG, "goBackToHomeScreen: Clicked the navigate up toolbar button")

            HomeScreenRobot().interact()
            return HomeScreenRobot.Transition()
        }

        fun goBackToBrowser(interact: BrowserRobot.() -> Unit): BrowserRobot.Transition {
            Log.i(TAG, "goBackToBrowser: Trying to click navigate up toolbar button")
            onView(allOf(withContentDescription("Navigate up"))).click()
            Log.i(TAG, "goBackToBrowser: Clicked the navigate up toolbar button")

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }

        fun openDetailedMenuForAddon(
            addonName: String,
            interact: SettingsSubMenuAddonsManagerAddonDetailedMenuRobot.() -> Unit,
        ): SettingsSubMenuAddonsManagerAddonDetailedMenuRobot.Transition {
            // Assigns a more descriptive name to the addon if it is "Bitwarden", otherwise keeps the original name
            // The name of this extenssion is being displayed differently across the app
            var addonName = if (addonName == "Bitwarden") "Bitwarden Password Manager" else addonName

            scrollToAddon(addonName)
            Log.i(TAG, "openDetailedMenuForAddon: Trying to verify that the $addonName add-on is visible")
            addonItem(addonName).check(matches(withEffectiveVisibility(Visibility.VISIBLE)))
            Log.i(TAG, "openDetailedMenuForAddon: Verified that the $addonName add-on is visible")
            Log.i(TAG, "openDetailedMenuForAddon: Trying to click the $addonName add-on")
            addonItem(addonName).perform(click())
            Log.i(TAG, "openDetailedMenuForAddon: Clicked the $addonName add-on")

            SettingsSubMenuAddonsManagerAddonDetailedMenuRobot().interact()
            return SettingsSubMenuAddonsManagerAddonDetailedMenuRobot.Transition()
        }

        fun clickExtensionsPromotionBannerLearnMoreLink(composeTestRule: ComposeTestRule, interact: BrowserRobot.() -> Unit): BrowserRobot.Transition {
            Log.i(TAG, "clickExtensionsPromotionBannerLearnMoreLink: Trying to click the \"Learn more\" link")
            composeTestRule.onNode(
                hasContentDescription("Learn more Links available"),
            ).performClick()
            Log.i(TAG, "clickExtensionsPromotionBannerLearnMoreLink: Clicked the \"Learn more\" link")

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }

        fun clickDiscoverMoreExtensionsButton(composeTestRule: ComposeTestRule, interact: BrowserRobot.() -> Unit): BrowserRobot.Transition {
            Log.i(TAG, "clickDiscoverMoreExtensionsButton: Trying to click the \"Discover more extensions\" link")
            composeTestRule.onNode(hasText(getStringResource(R.string.browser_menu_discover_more_extensions)), useUnmergedTree = true).performClick()
            Log.i(TAG, "clickDiscoverMoreExtensionsButton: Clicked the \"Discover more extensions\" link")

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }
    }

    private fun installButtonForAddon(addonName: String) =
        onView(
            allOf(
                withContentDescription("Install $addonName"),
                isDescendantOfA(withId(R.id.add_on_item)),
                hasSibling(hasDescendant(withText(addonName))),
            ),
        )

    private fun cancelInstall() {
        Log.i(TAG, "cancelInstall: Trying to verify that the \"Cancel\" button is completely displayed")
        onView(allOf(withId(R.id.deny_button), withText("Cancel"))).check(matches(isCompletelyDisplayed()))
        Log.i(TAG, "cancelInstall: Verified that the \"Cancel\" button is completely displayed")
        Log.i(TAG, "cancelInstall: Trying to click the \"Cancel\" button")
        onView(allOf(withId(R.id.deny_button), withText("Cancel"))).perform(click())
        Log.i(TAG, "cancelInstall: Clicked the \"Cancel\" button")
    }

    private fun allowPermissionToInstall() {
        Log.i(TAG, "allowPermissionToInstall: Trying to click the \"Add\" button")
        itemWithResIdContainingText(
            "$packageName:id/allow_button",
            getStringResource(R.string.mozac_feature_addons_permissions_dialog_add),
        ).click()
        Log.i(TAG, "allowPermissionToInstall: Clicked the \"Add\" button")
    }
}

fun addonsMenu(interact: SettingsSubMenuAddonsManagerRobot.() -> Unit): SettingsSubMenuAddonsManagerRobot.Transition {
    SettingsSubMenuAddonsManagerRobot().interact()
    return SettingsSubMenuAddonsManagerRobot.Transition()
}

private fun scrollToAddon(addonName: String) {
    Log.i(TAG, "scrollToAddon: Trying to scroll into view add-on: $addonName")
    addonsList().scrollIntoView(
        itemWithResIdContainingText(
            resourceId = "$packageName:id/add_on_name",
            text = addonName,
        ),
    )
    Log.i(TAG, "scrollToAddon: Scrolled into view add-on: $addonName")
}

private fun addonItem(addonName: String) =
    onView(
        allOf(
            withId(R.id.add_on_item),
            hasDescendant(
                allOf(
                    withId(R.id.add_on_name),
                    withText(addonName),
                ),
            ),
        ),
    )

private fun addonsList() =
    UiScrollable(UiSelector().resourceId("$packageName:id/add_ons_list")).setAsVerticalList()
