/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

@file:Suppress("TooManyFunctions")

package org.mozilla.fenix.ui.robots

import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.compose.ui.test.ExperimentalTestApi
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.junit4.ComposeTestRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.recyclerview.widget.RecyclerView
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.action.ViewActions
import androidx.test.espresso.action.ViewActions.longClick
import androidx.test.espresso.assertion.PositionAssertions.isCompletelyAbove
import androidx.test.espresso.assertion.PositionAssertions.isPartiallyBelow
import androidx.test.espresso.assertion.ViewAssertions.matches
import androidx.test.espresso.contrib.RecyclerViewActions
import androidx.test.espresso.matcher.ViewMatchers.Visibility
import androidx.test.espresso.matcher.ViewMatchers.hasDescendant
import androidx.test.espresso.matcher.ViewMatchers.isCompletelyDisplayed
import androidx.test.espresso.matcher.ViewMatchers.withEffectiveVisibility
import androidx.test.espresso.matcher.ViewMatchers.withId
import androidx.test.espresso.matcher.ViewMatchers.withText
import androidx.test.uiautomator.By
import androidx.test.uiautomator.By.textContains
import androidx.test.uiautomator.UiSelector
import androidx.test.uiautomator.Until
import org.hamcrest.CoreMatchers.allOf
import org.junit.Assert.assertTrue
import org.mozilla.fenix.R
import org.mozilla.fenix.helpers.AppAndSystemHelper.registerAndCleanupIdlingResources
import org.mozilla.fenix.helpers.Constants
import org.mozilla.fenix.helpers.Constants.LONG_CLICK_DURATION
import org.mozilla.fenix.helpers.Constants.RETRY_COUNT
import org.mozilla.fenix.helpers.Constants.TAG
import org.mozilla.fenix.helpers.DataGenerationHelper.getStringResource
import org.mozilla.fenix.helpers.HomeActivityComposeTestRule
import org.mozilla.fenix.helpers.MatcherHelper.assertItemTextEquals
import org.mozilla.fenix.helpers.MatcherHelper.assertUIObjectExists
import org.mozilla.fenix.helpers.MatcherHelper.assertUIObjectIsGone
import org.mozilla.fenix.helpers.MatcherHelper.itemWithDescription
import org.mozilla.fenix.helpers.MatcherHelper.itemWithResId
import org.mozilla.fenix.helpers.MatcherHelper.itemWithResIdAndText
import org.mozilla.fenix.helpers.MatcherHelper.itemWithResIdContainingText
import org.mozilla.fenix.helpers.SessionLoadedIdlingResource
import org.mozilla.fenix.helpers.TestAssetHelper.waitingTime
import org.mozilla.fenix.helpers.TestAssetHelper.waitingTimeShort
import org.mozilla.fenix.helpers.TestHelper.mDevice
import org.mozilla.fenix.helpers.TestHelper.packageName
import org.mozilla.fenix.helpers.TestHelper.waitForObjects
import org.mozilla.fenix.helpers.click
import org.mozilla.fenix.helpers.ext.waitNotNull
import org.mozilla.fenix.helpers.matchers.hasItemsCount
import org.mozilla.fenix.tabstray.TabsTrayTestTag

/**
 * Implementation of Robot Pattern for the URL toolbar.
 */
class NavigationToolbarRobot {
    fun verifyUrl(url: String) {
        Log.i(TAG, "verifyUrl: Trying to verify toolbar text matches $url")
        onView(withId(R.id.mozac_browser_toolbar_url_view)).check(matches(withText(url)))
        Log.i(TAG, "verifyUrl: Verified toolbar text matches $url")
    }

    fun verifyTabButtonShortcutMenuItems() {
        Log.i(TAG, "verifyTabButtonShortcutMenuItems: Trying to verify tab counter shortcut options")
        onView(withId(R.id.mozac_browser_menu_recyclerView))
            .check(matches(hasDescendant(withText("Close tab"))))
            .check(matches(hasDescendant(withText("New private tab"))))
            .check(matches(hasDescendant(withText("New tab"))))
        Log.i(TAG, "verifyTabButtonShortcutMenuItems: Verified tab counter shortcut options")
    }

    fun verifyTabButtonShortcutMenuItemsForNormalHomescreen() {
        Log.i(TAG, "verifyTabButtonShortcutMenuItemsForNormalHomescreen: Trying to verify tab counter shortcut options")
        onView(withId(R.id.mozac_browser_menu_recyclerView))
            .check(matches(hasItemsCount(2)))
            .check(matches(hasDescendant(withText("New tab"))))
            .check(matches(hasDescendant(withText("New private tab"))))
        Log.i(TAG, "verifyTabButtonShortcutMenuItemsForNormalHomescreen: Verified tab counter shortcut options")
    }

    fun verifyTabButtonShortcutMenuItemsForPrivateHomescreen() {
        Log.i(TAG, "verifyTabButtonShortcutMenuItemsForPrivateHomescreen: Trying to verify tab counter shortcut options")
        onView(withId(R.id.mozac_browser_menu_recyclerView))
            .check(matches(hasItemsCount(2)))
            .check(matches(hasDescendant(withText("New tab"))))
            .check(matches(hasDescendant(withText("New private tab"))))
        Log.i(TAG, "verifyTabButtonShortcutMenuItemsForPrivateHomescreen: Verified tab counter shortcut options")
    }

    fun verifyReaderViewDetected(exists: Boolean = false) {
        assertUIObjectExists(readerViewToggle(), exists = exists)
    }

    fun toggleReaderView() {
        Log.i(TAG, "toggleReaderView: Trying to click the reader view button")
        readerViewToggle().click()
        Log.i(TAG, "toggleReaderView: Clicked the reader view button")
    }

    fun verifyClipboardSuggestionsAreDisplayed(link: String = "", shouldBeDisplayed: Boolean) {
        assertUIObjectExists(
            itemWithResId("$packageName:id/fill_link_from_clipboard"),
            exists = shouldBeDisplayed,
        )
        // On Android 12 or above we don't SHOW the URL unless the user requests to do so.
        // See for more information https://github.com/mozilla-mobile/fenix/issues/22271
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            assertUIObjectExists(
                itemWithResIdAndText(
                    "$packageName:id/clipboard_url",
                    link,
                ),
                exists = shouldBeDisplayed,
            )
        }
    }

    fun longClickEditModeToolbar() {
        Log.i(TAG, "longClickEditModeToolbar: Trying to long click the edit mode toolbar")
        mDevice.findObject(By.res("$packageName:id/mozac_browser_toolbar_edit_url_view"))
            .click(LONG_CLICK_DURATION)
        Log.i(TAG, "longClickEditModeToolbar: Long clicked the edit mode toolbar")
    }

    fun clickContextMenuItem(item: String) {
        mDevice.waitNotNull(
            Until.findObject(By.text(item)),
            waitingTime,
        )
        Log.i(TAG, "clickContextMenuItem: Trying click context menu item: $item")
        mDevice.findObject(By.text(item)).click()
        Log.i(TAG, "clickContextMenuItem: Clicked context menu item: $item")
    }

    fun clickClearToolbarButton() {
        Log.i(TAG, "clickClearToolbarButton: Trying click the clear address button")
        clearAddressBarButton().click()
        Log.i(TAG, "clickClearToolbarButton: Clicked the clear address button")
    }

    fun verifyToolbarIsEmpty() =
        assertUIObjectExists(
            itemWithResIdContainingText(
                "$packageName:id/mozac_browser_toolbar_edit_url_view",
                getStringResource(R.string.search_hint),
            ),
        )

    // New unified search UI selector
    fun verifySearchBarPlaceholder(text: String) {
        Log.i(TAG, "verifySearchBarPlaceholder: Waiting for $waitingTime ms for the toolbar to exist")
        homeUrlBar().waitForExists(waitingTime)
        Log.i(TAG, "verifySearchBarPlaceholder: Waited for $waitingTime ms for the toolbar to exist")
        assertItemTextEquals(homeUrlBar(), expectedText = text)
    }

    // New unified search UI selector
    fun verifyDefaultSearchEngine(engineName: String) =
        assertUIObjectExists(
            searchSelectorButton().getChild(UiSelector().descriptionStartsWith(engineName)),
        )

    fun verifyTextSelectionOptions(vararg textSelectionOptions: String) {
        for (textSelectionOption in textSelectionOptions) {
            mDevice.waitNotNull(Until.findObject(textContains(textSelectionOption)), waitingTime)
        }
    }

    fun verifyRedesignedNavigationToolbarItems() {
        assertUIObjectExists(
            itemWithDescription(getStringResource(R.string.browser_menu_back)),
            itemWithDescription(getStringResource(R.string.browser_menu_forward)),
            itemWithDescription(getStringResource(R.string.search_hint)),
            itemWithDescription("More options"),
            itemWithResId("$packageName:id/counter_box"),
        )
    }

    fun verifyTranslationButton(isPageTranslated: Boolean, originalLanguage: String = "", translatedLanguage: String = "") {
        if (isPageTranslated) {
            for (i in 1..RETRY_COUNT) {
                Log.i(TAG, "verifyTranslationButton: Started try #$i")
                try {
                    assertUIObjectExists(itemWithDescription("Page translated from $originalLanguage to $translatedLanguage."))

                    break
                } catch (e: AssertionError) {
                    Log.i(TAG, "verifyTranslationButton: AssertionError caught, executing fallback methods")
                    navigationToolbar {
                    }.openThreeDotMenu {
                    }.refreshPage { }
                }
            }
        } else {
            assertUIObjectExists(itemWithDescription(getStringResource(R.string.browser_toolbar_translate)))
        }
    }

    fun verifyReaderViewNavigationToolbarButton(isReaderViewEnabled: Boolean) {
        if (isReaderViewEnabled) {
            assertUIObjectExists(itemWithDescription(getStringResource(R.string.browser_menu_read_close)))
        } else {
            assertUIObjectExists(itemWithDescription(getStringResource(R.string.browser_menu_read)))
        }
    }

    fun longTapNavButton(buttonDescription: String) {
        Log.i(TAG, "longTapNavButton: Waiting to find the nav bar $buttonDescription button.")
        mDevice.findObject(UiSelector().description("Back")).waitForExists(waitingTime)
        Log.i(TAG, "longTapNavButton: Trying to long click the nav bar $buttonDescription button.")
        mDevice.findObject(
            By.desc(buttonDescription)
                .enabled(true)
                .hasAncestor(By.res("$packageName:id/navigation_bar")),
        )
            .click(LONG_CLICK_DURATION)
        Log.i(TAG, "longTapNavButton: Long clicked the nav bar $buttonDescription button.")
    }

    fun verifyTabHistorySheetIsDisplayed(isDisplayed: Boolean) {
        assertUIObjectExists(
            itemWithResId("$packageName:id/tabHistoryRecyclerView"),
            exists = isDisplayed,
        )
    }

    fun verifyTabHistoryContainsWebsite(websiteUrl: String, isDisplayed: Boolean) {
        assertUIObjectExists(
            itemWithResIdAndText("$packageName:id/site_list_item", websiteUrl),
            exists = isDisplayed,
        )
    }

    // Verifies that the address bar is displayed separately, or merged with the navbar in landscape mode.
    fun verifyAddressBarIsDisplayedSeparately(isSeparate: Boolean, isAtTop: Boolean) {
        val addressBar = "$packageName:id/toolbar"
        val navBar = "$packageName:id/navigation_bar"

        if (isSeparate) {
            assertUIObjectExists(itemWithResId(addressBar), itemWithResId(navBar))
        } else {
            assertUIObjectIsGone(itemWithResId(if (isAtTop) navBar else addressBar))
            assertUIObjectExists(itemWithResId(if (isAtTop) addressBar else navBar))
        }
    }

    fun verifyAddressBarPosition(isAtTop: Boolean) {
        Log.i(TAG, "verifyAddressBarPosition: Trying to verify the toolbar address bar position is at the top: $isAtTop.")
        onView(withId(R.id.toolbar)).check(
            if (isAtTop) {
                isCompletelyAbove(withId(R.id.engineView))
            } else {
                isPartiallyBelow(withId(R.id.engineView))
            },
        )
        Log.i(TAG, "verifyAddressBarPosition: Verified the toolbar address bar position is at the top: $isAtTop.")
    }

    fun verifyNavBarBarPosition(isAtBottom: Boolean) {
        Log.i(TAG, "verifyNavBarBarPosition: Trying to verify the toolbar navbar position is at the bottom: $isAtBottom.")
        onView(allOf(withId(R.id.navigation_bar), isCompletelyDisplayed())).check(
            if (isAtBottom) {
                isPartiallyBelow(withId(R.id.engineView))
            } else {
                isCompletelyAbove(withId(R.id.engineView))
            },
        )
        Log.i(TAG, "verifyNavBarBarPosition: Verified the toolbar navbar position is at the bottom: $isAtBottom.")
    }

    class Transition {
        private lateinit var sessionLoadedIdlingResource: SessionLoadedIdlingResource

        fun enterURLAndEnterToBrowser(
            url: Uri,
            interact: BrowserRobot.() -> Unit,
        ): BrowserRobot.Transition {
            sessionLoadedIdlingResource = SessionLoadedIdlingResource()

            openEditURLView()
            Log.i(TAG, "enterURLAndEnterToBrowser: Trying to set toolbar text to: $url")
            awesomeBar().setText(url.toString())
            Log.i(TAG, "enterURLAndEnterToBrowser: Toolbar text was set to: $url")
            Log.i(TAG, "enterURLAndEnterToBrowser: Trying to press device enter button")
            mDevice.pressEnter()
            Log.i(TAG, "enterURLAndEnterToBrowser: Pressed device enter button")

            registerAndCleanupIdlingResources(sessionLoadedIdlingResource) {
                Log.i(TAG, "enterURLAndEnterToBrowser: Trying to assert that home screen layout or download button or the total cookie protection contextual hint exist")
                assertTrue(
                    itemWithResId("$packageName:id/browserLayout").waitForExists(waitingTime) ||
                        itemWithResId("$packageName:id/download_button").waitForExists(waitingTime) ||
                        itemWithResId("cfr.dismiss").waitForExists(waitingTime),
                )
                Log.i(TAG, "enterURLAndEnterToBrowser: Asserted that home screen layout or download button or the total cookie protection contextual hint exist")
            }

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }

        fun enterURL(
            url: Uri,
            interact: BrowserRobot.() -> Unit,
        ): BrowserRobot.Transition {
            sessionLoadedIdlingResource = SessionLoadedIdlingResource()

            openEditURLView()
            Log.i(TAG, "enterURLAndEnterToBrowser: Trying to set toolbar text to: $url")
            awesomeBar().setText(url.toString())
            Log.i(TAG, "enterURLAndEnterToBrowser: Toolbar text was set to: $url")
            Log.i(TAG, "enterURLAndEnterToBrowser: Trying to press device enter button")
            mDevice.pressEnter()
            Log.i(TAG, "enterURLAndEnterToBrowser: Pressed device enter button")

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }

        fun openTabCrashReporter(interact: BrowserRobot.() -> Unit): BrowserRobot.Transition {
            val crashUrl = "about:crashcontent"

            sessionLoadedIdlingResource = SessionLoadedIdlingResource()

            openEditURLView()
            Log.i(TAG, "openTabCrashReporter: Trying to set toolbar text to: $crashUrl")
            awesomeBar().setText(crashUrl)
            Log.i(TAG, "openTabCrashReporter: Toolbar text was set to: $crashUrl")
            Log.i(TAG, "openTabCrashReporter: Trying to press device enter button")
            mDevice.pressEnter()
            Log.i(TAG, "openTabCrashReporter: Pressed device enter button")

            registerAndCleanupIdlingResources(sessionLoadedIdlingResource) {
                Log.i(TAG, "openTabCrashReporter: Trying to find the tab crasher image")
                mDevice.findObject(UiSelector().resourceId("$packageName:id/crash_tab_image"))
                Log.i(TAG, "openTabCrashReporter: Found the tab crasher image")
            }

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }

        fun openThreeDotMenu(interact: ThreeDotMenuMainRobot.() -> Unit): ThreeDotMenuMainRobot.Transition {
            mDevice.waitNotNull(Until.findObject(By.res("$packageName:id/mozac_browser_toolbar_menu")), waitingTime)
            Log.i(TAG, "openThreeDotMenu: Trying to click the main menu button")
            threeDotButton().click()
            Log.i(TAG, "openThreeDotMenu: Clicked the main menu button")

            ThreeDotMenuMainRobot().interact()
            return ThreeDotMenuMainRobot.Transition()
        }

        fun openTabDrawer(composeTestRule: HomeActivityComposeTestRule, interact: TabDrawerRobot.() -> Unit): TabDrawerRobot.Transition {
            for (i in 1..Constants.RETRY_COUNT) {
                try {
                    Log.i(TAG, "openTabDrawer: Started try #$i")
                    mDevice.waitForObjects(
                        mDevice.findObject(
                            UiSelector()
                                .resourceId("$packageName:id/mozac_browser_toolbar_browser_actions"),
                        ),
                        waitingTime,
                    )
                    Log.i(TAG, "openTabDrawer: Trying to click the tabs tray button")
                    tabTrayButton().click()
                    Log.i(TAG, "openTabDrawer: Clicked the tabs tray button")
                    Log.i(TAG, "openTabDrawer: Trying to verify that the tabs tray exists")
                    composeTestRule.onNodeWithTag(TabsTrayTestTag.TABS_TRAY).assertExists()
                    Log.i(TAG, "openTabDrawer: Verified that the tabs tray exists")

                    break
                } catch (e: AssertionError) {
                    Log.i(TAG, "openTabDrawer: AssertionError caught, executing fallback methods")
                    if (i == Constants.RETRY_COUNT) {
                        throw e
                    } else {
                        Log.i(TAG, "openTabDrawer: Waiting for device to be idle")
                        mDevice.waitForIdle()
                        Log.i(TAG, "openTabDrawer: Waited for device to be idle")
                    }
                }
            }
            Log.i(TAG, "openTabDrawer: Trying to verify the tabs tray new tab FAB button exists")
            composeTestRule.onNodeWithTag(TabsTrayTestTag.FAB).assertExists()
            Log.i(TAG, "openTabDrawer: Verified the tabs tray new tab FAB button exists")

            TabDrawerRobot(composeTestRule).interact()
            return TabDrawerRobot.Transition(composeTestRule)
        }

        fun visitLinkFromClipboard(interact: BrowserRobot.() -> Unit): BrowserRobot.Transition {
            Log.i(TAG, "visitLinkFromClipboard: Waiting for $waitingTimeShort ms for clear address button to exist")
            if (clearAddressBarButton().waitForExists(waitingTimeShort)) {
                Log.i(TAG, "visitLinkFromClipboard: Waited for $waitingTimeShort ms for clear address button to exist")
                Log.i(TAG, "visitLinkFromClipboard: Trying to click the clear address button")
                clearAddressBarButton().click()
                Log.i(TAG, "visitLinkFromClipboard: Clicked the clear address button")
            }

            mDevice.waitNotNull(
                Until.findObject(By.res("$packageName:id/clipboard_title")),
                waitingTime,
            )

            // On Android 12 or above we don't SHOW the URL unless the user requests to do so.
            // See for more information https://github.com/mozilla-mobile/fenix/issues/22271
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                mDevice.waitNotNull(
                    Until.findObject(By.res("$packageName:id/clipboard_url")),
                    waitingTime,
                )
            }
            Log.i(TAG, "visitLinkFromClipboard: Trying to click the fill link from clipboard button")
            fillLinkButton().click()
            Log.i(TAG, "visitLinkFromClipboard: Clicked the fill link from clipboard button")

            Log.i(TAG, "visitLinkFromClipboard: Trying to press device enter button")
            mDevice.pressEnter()
            Log.i(TAG, "visitLinkFromClipboard: Pressed device enter button")

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }

        fun goBackToHomeScreen(interact: HomeScreenRobot.() -> Unit): HomeScreenRobot.Transition {
            Log.i(TAG, "goBackToHomeScreen: Trying to click the device back button")
            mDevice.pressBack()
            Log.i(TAG, "goBackToHomeScreen: Clicked the device back button")
            Log.i(TAG, "goBackToHomeScreen: Waiting for $waitingTimeShort ms for $packageName window to be updated")
            mDevice.waitForWindowUpdate(packageName, waitingTimeShort)
            Log.i(TAG, "goBackToHomeScreen: Waited for $waitingTimeShort ms for $packageName window to be updated")

            HomeScreenRobot().interact()
            return HomeScreenRobot.Transition()
        }

        fun goBackToBrowserScreen(interact: BrowserRobot.() -> Unit): BrowserRobot.Transition {
            Log.i(TAG, "goBackToBrowserScreen: Trying to click the device back button")
            mDevice.pressBack()
            Log.i(TAG, "goBackToBrowserScreen: Clicked the device back button")
            Log.i(TAG, "goBackToBrowserScreen: Waiting for $waitingTimeShort ms for $packageName window to be updated")
            mDevice.waitForWindowUpdate(packageName, waitingTimeShort)
            Log.i(TAG, "goBackToBrowserScreen: Waited for $waitingTimeShort ms for $packageName window to be updated")

            BrowserRobot().interact()
            return BrowserRobot.Transition()
        }

        fun openTabButtonShortcutsMenu(interact: NavigationToolbarRobot.() -> Unit): Transition {
            mDevice.waitNotNull(Until.findObject(By.res("$packageName:id/counter_root")))
            Log.i(TAG, "openTabButtonShortcutsMenu: Trying to long click the tab counter button")
            tabsCounter().perform(longClick())
            Log.i(TAG, "openTabButtonShortcutsMenu: Long clicked the tab counter button")

            NavigationToolbarRobot().interact()
            return Transition()
        }

        fun closeTabFromShortcutsMenu(interact: NavigationToolbarRobot.() -> Unit): Transition {
            Log.i(TAG, "closeTabFromShortcutsMenu: Waiting for device to be idle for $waitingTime ms")
            mDevice.waitForIdle(waitingTime)
            Log.i(TAG, "closeTabFromShortcutsMenu: Waited for device to be idle for $waitingTime ms")
            Log.i(TAG, "closeTabFromShortcutsMenu: Trying to click the \"Close tab\" button")
            onView(withId(R.id.mozac_browser_menu_recyclerView))
                .perform(
                    RecyclerViewActions.actionOnItem<RecyclerView.ViewHolder>(
                        hasDescendant(
                            withText("Close tab"),
                        ),
                        ViewActions.click(),
                    ),
                )
            Log.i(TAG, "closeTabFromShortcutsMenu: Clicked the \"Close tab\" button")

            NavigationToolbarRobot().interact()
            return Transition()
        }

        fun openNewTabFromShortcutsMenu(interact: SearchRobot.() -> Unit): SearchRobot.Transition {
            Log.i(TAG, "openNewTabFromShortcutsMenu: Waiting for device to be idle for $waitingTime ms")
            mDevice.waitForIdle(waitingTime)
            Log.i(TAG, "openNewTabFromShortcutsMenu: Waited for device to be idle for $waitingTime ms")
            Log.i(TAG, "openNewTabFromShortcutsMenu: Trying to click the \"New tab\" button")
            onView(withId(R.id.mozac_browser_menu_recyclerView))
                .perform(
                    RecyclerViewActions.actionOnItem<RecyclerView.ViewHolder>(
                        hasDescendant(
                            withText("New tab"),
                        ),
                        ViewActions.click(),
                    ),
                )
            Log.i(TAG, "openNewTabFromShortcutsMenu: Clicked the \"New tab\" button")

            SearchRobot().interact()
            return SearchRobot.Transition()
        }

        fun openNewPrivateTabFromShortcutsMenu(interact: SearchRobot.() -> Unit): SearchRobot.Transition {
            Log.i(TAG, "openNewPrivateTabFromShortcutsMenu: Waiting for device to be idle for $waitingTime ms")
            mDevice.waitForIdle(waitingTime)
            Log.i(TAG, "openNewPrivateTabFromShortcutsMenu: Waited for device to be idle for $waitingTime ms")
            Log.i(TAG, "openNewPrivateTabFromShortcutsMenu: Trying to click the \"New private tab\" button")
            onView(withId(R.id.mozac_browser_menu_recyclerView))
                .perform(
                    RecyclerViewActions.actionOnItem<RecyclerView.ViewHolder>(
                        hasDescendant(
                            withText("New private tab"),
                        ),
                        ViewActions.click(),
                    ),
                )
            Log.i(TAG, "openNewPrivateTabFromShortcutsMenu: Clicked the \"New private tab\" button")

            SearchRobot().interact()
            return SearchRobot.Transition()
        }

        fun clickUrlbar(interact: SearchRobot.() -> Unit): SearchRobot.Transition {
            Log.i(TAG, "clickUrlbar: Trying to click the toolbar")
            urlBar().click()
            Log.i(TAG, "clickUrlbar: Clicked the toolbar")
            Log.i(TAG, "clickUrlbar: Waiting for $waitingTime ms for the edit mode toolbar to exist")
            mDevice.findObject(
                UiSelector().resourceId("$packageName:id/mozac_browser_toolbar_edit_url_view"),
            ).waitForExists(waitingTime)
            Log.i(TAG, "clickUrlbar: Waited for $waitingTime ms for the edit mode toolbar to exist")

            SearchRobot().interact()
            return SearchRobot.Transition()
        }

        fun clickSearchSelectorButton(interact: SearchRobot.() -> Unit): SearchRobot.Transition {
            Log.i(TAG, "clickSearchSelectorButton: Waiting for $waitingTime ms for the search selector button to exist")
            searchSelectorButton().waitForExists(waitingTime)
            Log.i(TAG, "clickSearchSelectorButton: Waited for $waitingTime ms for the search selector button to exist")
            Log.i(TAG, "clickSearchSelectorButton: Trying to click the search selector button")
            searchSelectorButton().click()
            Log.i(TAG, "clickSearchSelectorButton: Clicked the search selector button")

            SearchRobot().interact()
            return SearchRobot.Transition()
        }

        fun clickTranslateButton(
            composeTestRule: ComposeTestRule,
            isPageTranslated: Boolean = false,
            originalLanguage: String = "",
            translatedLanguage: String = "",
            interact: TranslationsRobot.() -> Unit,
        ): TranslationsRobot.Transition {
            if (isPageTranslated) {
                Log.i(TAG, "clickTranslateButton: Trying to click the translate button")
                itemWithDescription("Page translated from $originalLanguage to $translatedLanguage.").click()
                Log.i(TAG, "clickTranslateButton: Clicked the translate button")
            } else {
                Log.i(TAG, "clickTranslateButton: Trying to click the translate button")
                itemWithDescription(getStringResource(R.string.browser_toolbar_translate)).click()
                Log.i(TAG, "clickTranslateButton: Clicked the translate button")
            }

            TranslationsRobot(composeTestRule).interact()
            return TranslationsRobot.Transition(composeTestRule)
        }

        // New navbar design home screen search button
        @OptIn(ExperimentalTestApi::class)
        fun clickHomeScreenSearchButton(composeTestRule: ComposeTestRule, interact: SearchRobot.() -> Unit): SearchRobot.Transition {
            Log.i(TAG, "clickHomeScreenSearchButton: Waiting for $waitingTime ms for the search button to exist.")
            composeTestRule.waitUntilAtLeastOneExists(hasContentDescription("Search or enter address"))
            Log.i(TAG, "clickHomeScreenSearchButton: Waited for $waitingTime ms for the search button to exist.")
            Log.i(TAG, "clickHomeScreenSearchButton: Trying to click the nav bar search button.")
            composeTestRule.onNodeWithContentDescription("Search or enter address").performClick()
            Log.i(TAG, "clickHomeScreenSearchButton: Clicked the nav bar search button.")

            SearchRobot().interact()
            return SearchRobot.Transition()
        }
    }
}

fun navigationToolbar(interact: NavigationToolbarRobot.() -> Unit): NavigationToolbarRobot.Transition {
    NavigationToolbarRobot().interact()
    return NavigationToolbarRobot.Transition()
}

fun openEditURLView() {
    Log.i(TAG, "openEditURLView: Waiting for $waitingTime ms for the toolbar to exist")
    urlBar().waitForExists(waitingTime)
    Log.i(TAG, "openEditURLView: Waited for $waitingTime ms for the toolbar to exist")
    Log.i(TAG, "openEditURLView: Trying to click the toolbar")
    urlBar().click()
    Log.i(TAG, "openEditURLView: Clicked the toolbar")
    Log.i(TAG, "openEditURLView: Waiting for $waitingTime ms for the edit mode toolbar to exist")
    itemWithResId("$packageName:id/mozac_browser_toolbar_edit_url_view").waitForExists(waitingTime)
    Log.i(TAG, "openEditURLView: Waited for $waitingTime ms for the edit mode toolbar to exist")
}

private fun urlBar() = mDevice.findObject(UiSelector().resourceId("$packageName:id/toolbar"))
private fun homeUrlBar() = mDevice.findObject(UiSelector().resourceId("$packageName:id/toolbar_text"))
private fun awesomeBar() =
    mDevice.findObject(UiSelector().resourceId("$packageName:id/mozac_browser_toolbar_edit_url_view"))
private fun threeDotButton() = onView(withId(R.id.mozac_browser_toolbar_menu))
private fun tabTrayButton() = onView(withId(R.id.tab_button))
private fun tabsCounter() = onView(
    allOf(
        withId(R.id.counter_root),
        withEffectiveVisibility(Visibility.VISIBLE),
    ),
)
private fun fillLinkButton() = onView(withId(R.id.fill_link_from_clipboard))
private fun clearAddressBarButton() = itemWithResId("$packageName:id/mozac_browser_toolbar_clear_view")
private fun readerViewToggle() =
    itemWithDescription(getStringResource(R.string.browser_menu_read))

private fun searchSelectorButton() =
    mDevice.findObject(UiSelector().resourceId("$packageName:id/search_selector"))
