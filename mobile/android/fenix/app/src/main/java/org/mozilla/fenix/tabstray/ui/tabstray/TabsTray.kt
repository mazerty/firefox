/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

@file:OptIn(ExperimentalFoundationApi::class)

package org.mozilla.fenix.tabstray.ui.tabstray

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.tooling.preview.PreviewLightDark
import mozilla.components.browser.state.state.ContentState
import mozilla.components.browser.state.state.TabSessionState
import mozilla.components.browser.state.state.createTab
import mozilla.components.browser.storage.sync.TabEntry
import mozilla.components.lib.state.ext.observeAsState
import org.mozilla.fenix.tabstray.Page
import org.mozilla.fenix.tabstray.TabsTrayAction
import org.mozilla.fenix.tabstray.TabsTrayState
import org.mozilla.fenix.tabstray.TabsTrayStore
import org.mozilla.fenix.tabstray.TabsTrayTestTag
import org.mozilla.fenix.tabstray.ext.isNormalTab
import org.mozilla.fenix.tabstray.syncedtabs.SyncedTabsListItem
import org.mozilla.fenix.tabstray.ui.banner.TabsTrayBanner
import org.mozilla.fenix.tabstray.ui.fab.TabsTrayFab
import org.mozilla.fenix.tabstray.ui.tabpage.NormalTabsPage
import org.mozilla.fenix.tabstray.ui.tabpage.PrivateTabsPage
import org.mozilla.fenix.tabstray.ui.tabpage.SyncedTabsPage
import org.mozilla.fenix.theme.FirefoxTheme
import mozilla.components.browser.storage.sync.Tab as SyncTab
import org.mozilla.fenix.tabstray.ui.syncedtabs.OnTabClick as OnSyncedTabClick
import org.mozilla.fenix.tabstray.ui.syncedtabs.OnTabCloseClick as OnSyncedTabClose

/**
 * Top-level UI for displaying the Tabs Tray feature.
 *
 * @param tabsTrayStore [TabsTrayStore] used to listen for changes to [TabsTrayState].
 * @param displayTabsInGrid Whether the normal and private tabs should be displayed in a grid.
 * @param isInDebugMode True for debug variant or if secret menu is enabled for this session.
 * @param shouldShowTabAutoCloseBanner Whether the tab auto closer banner should be displayed.
 * @param shouldShowLockPbmBanner Whether the lock private browsing banner should be displayed.
 * @param isSignedIn Used to determine whether to show the SYNC FAB when [Page.SyncedTabs] is displayed.
 * @param modifier The [Modifier] used to style the container of the the Tabs Tray UI.
 * @param shouldShowInactiveTabsAutoCloseDialog Whether the inactive tabs auto close dialog should be displayed.
 * @param onTabPageClick Invoked when the user clicks on the Normal, Private, or Synced tabs page button.
 * @param onTabClose Invoked when the user clicks to close a tab.
 * @param onTabClick Invoked when the user clicks on a tab.
 * @param onTabLongClick Invoked when the user long clicks a tab.
 * @param onInactiveTabsHeaderClick Invoked when the user clicks on the inactive tabs section header.
 * @param onDeleteAllInactiveTabsClick Invoked when the user clicks on the delete all inactive tabs button.
 * @param onInactiveTabsAutoCloseDialogShown Invoked when the inactive tabs auto close dialog
 * is presented to the user.
 * @param onInactiveTabAutoCloseDialogCloseButtonClick Invoked when the user clicks on the inactive
 * tab auto close dialog's dismiss button.
 * @param onEnableInactiveTabAutoCloseClick Invoked when the user clicks on the inactive tab auto
 * close dialog's enable button.
 * @param onInactiveTabClick Invoked when the user clicks on an inactive tab.
 * @param onInactiveTabClose Invoked when the user clicks on an inactive tab's close button.
 * @param onSyncedTabClick Invoked when the user clicks on a synced tab.
 * @param onSyncedTabClose Invoked when the user clicks on a synced tab's close button.
 * @param onSaveToCollectionClick Invoked when the user clicks on the save to collection button from
 * the multi select banner.
 * @param onShareSelectedTabsClick Invoked when the user clicks on the share button from the
 * multi select banner.
 * @param onShareAllTabsClick Invoked when the user clicks on the share all tabs banner menu item.
 * @param onTabSettingsClick Invoked when the user clicks on the tab settings banner menu item.
 * @param onRecentlyClosedClick Invoked when the user clicks on the recently closed banner menu item.
 * @param onAccountSettingsClick Invoked when the user clicks on the account settings banner menu item.
 * @param onDeleteAllTabsClick Invoked when the user clicks on the close all tabs banner menu item.
 * @param onBookmarkSelectedTabsClick Invoked when the user clicks on the bookmark banner menu item.
 * @param onDeleteSelectedTabsClick Invoked when the user clicks on the close selected tabs banner menu item.
 * @param onForceSelectedTabsAsInactiveClick Invoked when the user clicks on the make inactive banner menu item.
 * @param onTabAutoCloseBannerViewOptionsClick Invoked when the user clicks to view the auto close options.
 * @param onTabsTrayPbmLockedClick Invoked when the user interacts with the lock private browsing mode banner.
 * @param onTabsTrayPbmLockedDismiss Invoked when the user clicks either button on the
 * lock private browsing mode banner.
 * @param onTabAutoCloseBannerDismiss Invoked when the user clicks to dismiss the auto close banner.
 * @param onTabAutoCloseBannerShown Invoked when the auto close banner has been shown to the user.
 * @param onMove Invoked after the drag and drop gesture completed. Swaps positions of two tabs.
 * @param shouldShowInactiveTabsCFR Returns whether the inactive tabs CFR should be displayed.
 * @param onInactiveTabsCFRShown Invoked when the inactive tabs CFR is displayed.
 * @param onInactiveTabsCFRClick Invoked when the inactive tabs CFR is clicked.
 * @param onInactiveTabsCFRDismiss Invoked when the inactive tabs CFR is dismissed.
 * @param onNormalTabsFabClicked Invoked when the fab is clicked in [Page.NormalTabs].
 * @param onPrivateTabsFabClicked Invoked when the fab is clicked in [Page.PrivateTabs].
 * @param onSyncedTabsFabClicked Invoked when the fab is clicked in [Page.SyncedTabs].
 */
@Suppress("LongMethod", "LongParameterList", "ComplexMethod")
@Composable
fun TabsTray(
    tabsTrayStore: TabsTrayStore,
    displayTabsInGrid: Boolean,
    isInDebugMode: Boolean,
    shouldShowTabAutoCloseBanner: Boolean,
    shouldShowLockPbmBanner: Boolean,
    isSignedIn: Boolean,
    modifier: Modifier = Modifier,
    shouldShowInactiveTabsAutoCloseDialog: (Int) -> Boolean,
    onTabPageClick: (Page) -> Unit,
    onTabClose: (TabSessionState) -> Unit,
    onTabClick: (TabSessionState) -> Unit,
    onTabLongClick: (TabSessionState) -> Unit,
    onInactiveTabsHeaderClick: (Boolean) -> Unit,
    onDeleteAllInactiveTabsClick: () -> Unit,
    onInactiveTabsAutoCloseDialogShown: () -> Unit,
    onInactiveTabAutoCloseDialogCloseButtonClick: () -> Unit,
    onEnableInactiveTabAutoCloseClick: () -> Unit,
    onInactiveTabClick: (TabSessionState) -> Unit,
    onInactiveTabClose: (TabSessionState) -> Unit,
    onSyncedTabClick: OnSyncedTabClick,
    onSyncedTabClose: OnSyncedTabClose,
    onSaveToCollectionClick: () -> Unit,
    onShareSelectedTabsClick: () -> Unit,
    onShareAllTabsClick: () -> Unit,
    onTabSettingsClick: () -> Unit,
    onRecentlyClosedClick: () -> Unit,
    onAccountSettingsClick: () -> Unit,
    onDeleteAllTabsClick: () -> Unit,
    onBookmarkSelectedTabsClick: () -> Unit,
    onDeleteSelectedTabsClick: () -> Unit,
    onForceSelectedTabsAsInactiveClick: () -> Unit,
    onTabAutoCloseBannerViewOptionsClick: () -> Unit,
    onTabsTrayPbmLockedClick: () -> Unit,
    onTabsTrayPbmLockedDismiss: () -> Unit,
    onTabAutoCloseBannerDismiss: () -> Unit,
    onTabAutoCloseBannerShown: () -> Unit,
    onMove: (String, String?, Boolean) -> Unit,
    shouldShowInactiveTabsCFR: () -> Boolean,
    onInactiveTabsCFRShown: () -> Unit,
    onInactiveTabsCFRClick: () -> Unit,
    onInactiveTabsCFRDismiss: () -> Unit,
    onNormalTabsFabClicked: () -> Unit,
    onPrivateTabsFabClicked: () -> Unit,
    onSyncedTabsFabClicked: () -> Unit,
) {
    val tabsTrayState by tabsTrayStore.observeAsState(initialValue = tabsTrayStore.state) { it }
    val pagerState = rememberPagerState(
        initialPage = Page.pageToPosition(tabsTrayState.selectedPage),
        pageCount = { Page.entries.size },
    )
    val syncedTabCount = remember(tabsTrayState.syncedTabs) {
        tabsTrayState.syncedTabs
            .filterIsInstance<SyncedTabsListItem.DeviceSection>()
            .sumOf { deviceSection: SyncedTabsListItem.DeviceSection -> deviceSection.tabs.size }
    }

    LaunchedEffect(tabsTrayState.selectedPage) {
        pagerState.animateScrollToPage(Page.pageToPosition(tabsTrayState.selectedPage))
    }

    Scaffold(
        modifier = modifier,
        topBar = {
            TabsTrayBanner(
                selectedPage = tabsTrayState.selectedPage,
                normalTabCount = tabsTrayState.normalTabs.size + tabsTrayState.inactiveTabs.size,
                privateTabCount = tabsTrayState.privateTabs.size,
                syncedTabCount = syncedTabCount,
                selectionMode = tabsTrayState.mode,
                isInDebugMode = isInDebugMode,
                shouldShowTabAutoCloseBanner = shouldShowTabAutoCloseBanner,
                shouldShowLockPbmBanner = shouldShowLockPbmBanner,
                onTabPageIndicatorClicked = onTabPageClick,
                onSaveToCollectionClick = onSaveToCollectionClick,
                onShareSelectedTabsClick = onShareSelectedTabsClick,
                onShareAllTabsClick = onShareAllTabsClick,
                onTabSettingsClick = onTabSettingsClick,
                onRecentlyClosedClick = onRecentlyClosedClick,
                onAccountSettingsClick = onAccountSettingsClick,
                onDeleteAllTabsClick = onDeleteAllTabsClick,
                onBookmarkSelectedTabsClick = onBookmarkSelectedTabsClick,
                onDeleteSelectedTabsClick = onDeleteSelectedTabsClick,
                onForceSelectedTabsAsInactiveClick = onForceSelectedTabsAsInactiveClick,
                onTabAutoCloseBannerViewOptionsClick = onTabAutoCloseBannerViewOptionsClick,
                onTabsTrayPbmLockedClick = onTabsTrayPbmLockedClick,
                onTabsTrayPbmLockedDismiss = onTabsTrayPbmLockedDismiss,
                onTabAutoCloseBannerDismiss = onTabAutoCloseBannerDismiss,
                onTabAutoCloseBannerShown = onTabAutoCloseBannerShown,
                onEnterMultiselectModeClick = {
                    tabsTrayStore.dispatch(TabsTrayAction.EnterSelectMode)
                },
                onExitSelectModeClick = {
                    tabsTrayStore.dispatch(TabsTrayAction.ExitSelectMode)
                },
            )
        },
        floatingActionButton = {
            TabsTrayFab(
                tabsTrayStore = tabsTrayStore,
                isSignedIn = isSignedIn,
                onNormalTabsFabClicked = onNormalTabsFabClicked,
                onPrivateTabsFabClicked = onPrivateTabsFabClicked,
                onSyncedTabsFabClicked = onSyncedTabsFabClicked,
            )
        },
        containerColor = MaterialTheme.colorScheme.surface,
    ) { paddingValues ->
        HorizontalPager(
            modifier = Modifier
                .padding(paddingValues)
                .fillMaxSize()
                .testTag(TabsTrayTestTag.TABS_TRAY),
            state = pagerState,
            beyondViewportPageCount = 2,
            userScrollEnabled = false,
        ) { position ->
            when (Page.positionToPage(position)) {
                Page.NormalTabs -> {
                    NormalTabsPage(
                        normalTabs = tabsTrayState.normalTabs,
                        inactiveTabs = tabsTrayState.inactiveTabs,
                        selectedTabId = tabsTrayState.selectedTabId,
                        selectionMode = tabsTrayState.mode,
                        inactiveTabsExpanded = tabsTrayState.inactiveTabsExpanded,
                        displayTabsInGrid = displayTabsInGrid,
                        onTabClose = onTabClose,
                        onTabClick = onTabClick,
                        onTabLongClick = onTabLongClick,
                        shouldShowInactiveTabsAutoCloseDialog = shouldShowInactiveTabsAutoCloseDialog,
                        onInactiveTabsHeaderClick = onInactiveTabsHeaderClick,
                        onDeleteAllInactiveTabsClick = onDeleteAllInactiveTabsClick,
                        onInactiveTabsAutoCloseDialogShown = onInactiveTabsAutoCloseDialogShown,
                        onInactiveTabAutoCloseDialogCloseButtonClick = onInactiveTabAutoCloseDialogCloseButtonClick,
                        onEnableInactiveTabAutoCloseClick = onEnableInactiveTabAutoCloseClick,
                        onInactiveTabClick = onInactiveTabClick,
                        onInactiveTabClose = onInactiveTabClose,
                        onMove = onMove,
                        shouldShowInactiveTabsCFR = shouldShowInactiveTabsCFR,
                        onInactiveTabsCFRShown = onInactiveTabsCFRShown,
                        onInactiveTabsCFRClick = onInactiveTabsCFRClick,
                        onInactiveTabsCFRDismiss = onInactiveTabsCFRDismiss,
                        onTabDragStart = {
                            tabsTrayStore.dispatch(TabsTrayAction.ExitSelectMode)
                        },
                    )
                }

                Page.PrivateTabs -> {
                    PrivateTabsPage(
                        privateTabs = tabsTrayState.privateTabs,
                        selectedTabId = tabsTrayState.selectedTabId,
                        selectionMode = tabsTrayState.mode,
                        displayTabsInGrid = displayTabsInGrid,
                        onTabClose = onTabClose,
                        onTabClick = onTabClick,
                        onTabLongClick = onTabLongClick,
                        onMove = onMove,
                    )
                }

                Page.SyncedTabs -> {
                    SyncedTabsPage(
                        syncedTabs = tabsTrayState.syncedTabs,
                        onTabClick = onSyncedTabClick,
                        onTabClose = onSyncedTabClose,
                    )
                }
            }
        }
    }
}

@PreviewLightDark
@Composable
private fun TabsTrayPreview() {
    val tabs = generateFakeTabsList()
    TabsTrayPreviewRoot(
        displayTabsInGrid = false,
        selectedTabId = tabs[0].id,
        normalTabs = tabs,
        privateTabs = generateFakeTabsList(
            tabCount = 7,
            isPrivate = true,
        ),
        syncedTabs = generateFakeSyncedTabsList(),
    )
}

@Suppress("MagicNumber")
@PreviewLightDark
@Composable
private fun TabsTrayMultiSelectPreview() {
    val tabs = generateFakeTabsList()
    TabsTrayPreviewRoot(
        selectedTabId = tabs[0].id,
        mode = TabsTrayState.Mode.Select(tabs.take(4).toSet()),
        normalTabs = tabs,
    )
}

@PreviewLightDark
@Composable
private fun TabsTrayInactiveTabsPreview() {
    TabsTrayPreviewRoot(
        normalTabs = generateFakeTabsList(tabCount = 3),
        inactiveTabs = generateFakeTabsList(),
        inactiveTabsExpanded = true,
        showInactiveTabsAutoCloseDialog = true,
    )
}

@PreviewLightDark
@Composable
private fun TabsTrayPrivateTabsPreview() {
    TabsTrayPreviewRoot(
        selectedPage = Page.PrivateTabs,
        privateTabs = generateFakeTabsList(isPrivate = true),
    )
}

@PreviewLightDark
@Composable
private fun TabsTraySyncedTabsPreview() {
    TabsTrayPreviewRoot(
        selectedPage = Page.SyncedTabs,
        syncedTabs = generateFakeSyncedTabsList(deviceCount = 3),
    )
}

@PreviewLightDark
@Composable
private fun TabsTrayAutoCloseBannerPreview() {
    TabsTrayPreviewRoot(
        normalTabs = generateFakeTabsList(),
        showTabAutoCloseBanner = true,
    )
}

@Suppress("LongMethod")
@Composable
private fun TabsTrayPreviewRoot(
    displayTabsInGrid: Boolean = true,
    selectedPage: Page = Page.NormalTabs,
    selectedTabId: String? = null,
    mode: TabsTrayState.Mode = TabsTrayState.Mode.Normal,
    normalTabs: List<TabSessionState> = emptyList(),
    inactiveTabs: List<TabSessionState> = emptyList(),
    privateTabs: List<TabSessionState> = emptyList(),
    syncedTabs: List<SyncedTabsListItem> = emptyList(),
    inactiveTabsExpanded: Boolean = false,
    showInactiveTabsAutoCloseDialog: Boolean = false,
    showTabAutoCloseBanner: Boolean = false,
    isSignedIn: Boolean = true,
) {
    var showInactiveTabsAutoCloseDialogState by remember { mutableStateOf(showInactiveTabsAutoCloseDialog) }

    val tabsTrayStore = remember {
        TabsTrayStore(
            initialState = TabsTrayState(
                selectedPage = selectedPage,
                mode = mode,
                inactiveTabs = inactiveTabs,
                inactiveTabsExpanded = inactiveTabsExpanded,
                normalTabs = normalTabs,
                privateTabs = privateTabs,
                syncedTabs = syncedTabs,
                selectedTabId = selectedTabId,
            ),
        )
    }

    FirefoxTheme {
        TabsTray(
            tabsTrayStore = tabsTrayStore,
            displayTabsInGrid = displayTabsInGrid,
            isInDebugMode = false,
            shouldShowInactiveTabsAutoCloseDialog = { true },
            shouldShowTabAutoCloseBanner = showTabAutoCloseBanner,
            isSignedIn = isSignedIn,
            onTabPageClick = { page ->
                tabsTrayStore.dispatch(TabsTrayAction.PageSelected(page))
            },
            onTabClose = { tab ->
                if (tab.isNormalTab()) {
                    val newTabs = tabsTrayStore.state.normalTabs - tab
                    tabsTrayStore.dispatch(TabsTrayAction.UpdateNormalTabs(newTabs))
                } else {
                    val newTabs = tabsTrayStore.state.privateTabs - tab
                    tabsTrayStore.dispatch(TabsTrayAction.UpdatePrivateTabs(newTabs))
                }
            },
            onTabClick = { tab ->
                when (tabsTrayStore.state.mode) {
                    TabsTrayState.Mode.Normal -> {
                        tabsTrayStore.dispatch(TabsTrayAction.UpdateSelectedTabId(tabId = tab.id))
                    }

                    is TabsTrayState.Mode.Select -> {
                        if (tabsTrayStore.state.mode.selectedTabs.contains(tab)) {
                            tabsTrayStore.dispatch(TabsTrayAction.RemoveSelectTab(tab))
                        } else {
                            tabsTrayStore.dispatch(TabsTrayAction.AddSelectTab(tab))
                        }
                    }
                }
            },
            onTabLongClick = { tab ->
                tabsTrayStore.dispatch(TabsTrayAction.AddSelectTab(tab))
            },
            onInactiveTabsHeaderClick = { expanded ->
                tabsTrayStore.dispatch(TabsTrayAction.UpdateInactiveExpanded(expanded))
            },
            onDeleteAllInactiveTabsClick = {
                tabsTrayStore.dispatch(TabsTrayAction.UpdateInactiveTabs(emptyList()))
            },
            onInactiveTabsAutoCloseDialogShown = {},
            onInactiveTabAutoCloseDialogCloseButtonClick = {
                showInactiveTabsAutoCloseDialogState = !showInactiveTabsAutoCloseDialogState
            },
            onEnableInactiveTabAutoCloseClick = {
                showInactiveTabsAutoCloseDialogState = !showInactiveTabsAutoCloseDialogState
            },
            onInactiveTabClick = {},
            onInactiveTabClose = { tab ->
                val newTabs = tabsTrayStore.state.inactiveTabs - tab
                tabsTrayStore.dispatch(TabsTrayAction.UpdateInactiveTabs(newTabs))
            },
            onSyncedTabClick = {},
            onSyncedTabClose = { _, _ -> },
            onSaveToCollectionClick = {},
            onShareSelectedTabsClick = {},
            onShareAllTabsClick = {},
            onTabSettingsClick = {},
            onRecentlyClosedClick = {},
            onAccountSettingsClick = {},
            onDeleteAllTabsClick = {},
            onDeleteSelectedTabsClick = {},
            onBookmarkSelectedTabsClick = {},
            onForceSelectedTabsAsInactiveClick = {},
            onTabAutoCloseBannerViewOptionsClick = {},
            onTabsTrayPbmLockedClick = {},
            onTabsTrayPbmLockedDismiss = {},
            onTabAutoCloseBannerDismiss = {},
            onTabAutoCloseBannerShown = {},
            onMove = { _, _, _ -> },
            shouldShowInactiveTabsCFR = { false },
            onInactiveTabsCFRShown = {},
            onInactiveTabsCFRClick = {},
            onInactiveTabsCFRDismiss = {},
            shouldShowLockPbmBanner = false,
            onNormalTabsFabClicked = {
                val newTab = createTab(
                    url = "www.mozilla.com",
                    private = false,
                )
                val allTabs = tabsTrayStore.state.normalTabs + newTab
                tabsTrayStore.dispatch(TabsTrayAction.UpdateNormalTabs(allTabs))
            },
            onPrivateTabsFabClicked = {
                val newTab = createTab(
                    url = "www.mozilla.com",
                    private = true,
                )
                val allTabs = tabsTrayStore.state.privateTabs + newTab
                tabsTrayStore.dispatch(TabsTrayAction.UpdatePrivateTabs(allTabs))
            },
            onSyncedTabsFabClicked = {
                val newSyncedTabList = tabsTrayStore.state.syncedTabs + generateFakeSyncedTabsList()
                tabsTrayStore.dispatch(TabsTrayAction.UpdateSyncedTabs(newSyncedTabList))
            },
        )
    }
}

private fun generateFakeTabsList(tabCount: Int = 10, isPrivate: Boolean = false): List<TabSessionState> =
    List(tabCount) { index ->
        TabSessionState(
            id = "tabId$index-$isPrivate",
            content = ContentState(
                url = "www.mozilla.com",
                private = isPrivate,
            ),
        )
    }

private fun generateFakeSyncedTabsList(deviceCount: Int = 1): List<SyncedTabsListItem> =
    List(deviceCount) { index ->
        SyncedTabsListItem.DeviceSection(
            displayName = "Device $index",
            tabs = listOf(
                generateFakeSyncedTab("Mozilla", "www.mozilla.org"),
                generateFakeSyncedTab("Google", "www.google.com"),
                generateFakeSyncedTab("", "www.google.com"),
            ),
        )
    }

private fun generateFakeSyncedTab(
    tabName: String,
    tabUrl: String,
    action: SyncedTabsListItem.Tab.Action = SyncedTabsListItem.Tab.Action.None,
): SyncedTabsListItem.Tab =
    SyncedTabsListItem.Tab(
        tabName.ifEmpty { tabUrl },
        tabUrl,
        action,
        SyncTab(
            history = listOf(TabEntry(tabName, tabUrl, null)),
            active = 0,
            lastUsed = 0L,
            inactive = false,
        ),
    )
