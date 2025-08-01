/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.browser.state.action

import android.graphics.Bitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.AutoPlayAudibleBlockingAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.AutoPlayAudibleChangedAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.AutoPlayInAudibleBlockingAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.AutoPlayInAudibleChangedAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.CameraChangedAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.LocationChangedAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.MediaKeySystemAccesChangedAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.MicrophoneChangedAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.NotificationChangedAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.PersistentStorageChangedAction
import mozilla.components.browser.state.action.ContentAction.UpdatePermissionHighlightsStateAction.Reset
import mozilla.components.browser.state.selector.findCustomTab
import mozilla.components.browser.state.state.AppIntentState
import mozilla.components.browser.state.state.BrowserState
import mozilla.components.browser.state.state.LoadRequestState
import mozilla.components.browser.state.state.SecurityInfoState
import mozilla.components.browser.state.state.TabSessionState
import mozilla.components.browser.state.state.content.DownloadState
import mozilla.components.browser.state.state.content.FindResultState
import mozilla.components.browser.state.state.content.HistoryState
import mozilla.components.browser.state.state.content.PermissionHighlightsState
import mozilla.components.browser.state.state.createCustomTab
import mozilla.components.browser.state.state.createTab
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.concept.engine.HitResult
import mozilla.components.concept.engine.history.HistoryItem
import mozilla.components.concept.engine.manifest.WebAppManifest
import mozilla.components.concept.engine.permission.Permission.AppLocationCoarse
import mozilla.components.concept.engine.permission.Permission.ContentGeoLocation
import mozilla.components.concept.engine.permission.PermissionRequest
import mozilla.components.concept.engine.prompt.PromptRequest
import mozilla.components.concept.engine.window.WindowRequest
import mozilla.components.support.test.ext.joinBlocking
import mozilla.components.support.test.libstate.ext.waitUntilIdle
import mozilla.components.support.test.mock
import mozilla.components.support.test.whenever
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.mockito.Mockito.spy
import org.mockito.Mockito.verify

@RunWith(AndroidJUnit4::class)
class ContentActionTest {
    private lateinit var store: BrowserStore
    private lateinit var tabId: String
    private lateinit var otherTabId: String

    private val tab: TabSessionState
        get() = store.state.tabs.find { it.id == tabId }!!

    private val otherTab: TabSessionState
        get() = store.state.tabs.find { it.id == otherTabId }!!

    @Before
    fun setUp() {
        val state = BrowserState(
            tabs = listOf(
                createTab(url = "https://www.mozilla.org").also {
                    tabId = it.id
                },
                createTab(url = "https://www.firefox.com").also {
                    otherTabId = it.id
                },
            ),
        )

        store = BrowserStore(state)
    }

    @Test
    fun `UpdateUrlAction updates URL`() {
        val newUrl = "https://www.example.org"

        assertNotEquals(newUrl, tab.content.url)
        assertNotEquals(newUrl, otherTab.content.url)

        store.dispatch(
            ContentAction.UpdateUrlAction(tab.id, newUrl),
        ).joinBlocking()

        assertEquals(newUrl, tab.content.url)
        assertNotEquals(newUrl, otherTab.content.url)
    }

    @Test
    fun `UpdateUrlAction clears icon`() {
        val icon = spy(Bitmap::class.java)

        assertNotEquals(icon, tab.content.icon)
        assertNotEquals(icon, otherTab.content.icon)

        store.dispatch(
            ContentAction.UpdateIconAction(tab.id, tab.content.url, icon),
        ).joinBlocking()

        assertEquals(icon, tab.content.icon)

        store.dispatch(
            ContentAction.UpdateUrlAction(tab.id, "https://www.example.org"),
        ).joinBlocking()

        assertNull(tab.content.icon)
    }

    @Test
    fun `UpdateUrlAction does not clear icon if host is the same`() {
        val icon = spy(Bitmap::class.java)

        assertNotEquals(icon, tab.content.icon)
        assertNotEquals(icon, otherTab.content.icon)

        store.dispatch(
            ContentAction.UpdateIconAction(tab.id, tab.content.url, icon),
        ).joinBlocking()

        assertEquals(icon, tab.content.icon)

        store.dispatch(
            ContentAction.UpdateUrlAction(tab.id, "https://www.mozilla.org/firefox"),
        ).joinBlocking()

        assertEquals(icon, tab.content.icon)
    }

    @Test
    fun `WHEN UpdateUrlAction is dispatched by user gesture THEN the search terms are cleared`() {
        val searchTerms = "Firefox"
        store.dispatch(
            ContentAction.UpdateSearchTermsAction(tab.id, searchTerms),
        ).joinBlocking()

        assertEquals(searchTerms, tab.content.searchTerms)

        store.dispatch(
            ContentAction.UpdateUrlAction(tab.id, "https://www.mozilla.org", false),
        ).joinBlocking()

        assertEquals(searchTerms, tab.content.searchTerms)

        store.dispatch(
            ContentAction.UpdateUrlAction(tab.id, "https://www.mozilla.org/firefox", true),
        ).joinBlocking()

        assertEquals("", tab.content.searchTerms)
    }

    @Test
    fun `UpdateLoadingStateAction updates loading state`() {
        assertFalse(tab.content.loading)
        assertFalse(otherTab.content.loading)

        store.dispatch(
            ContentAction.UpdateLoadingStateAction(tab.id, true),
        ).joinBlocking()

        assertTrue(tab.content.loading)
        assertFalse(otherTab.content.loading)

        store.dispatch(
            ContentAction.UpdateLoadingStateAction(tab.id, false),
        ).joinBlocking()

        assertFalse(tab.content.loading)
        assertFalse(otherTab.content.loading)

        store.dispatch(
            ContentAction.UpdateLoadingStateAction(tab.id, true),
        ).joinBlocking()

        store.dispatch(
            ContentAction.UpdateLoadingStateAction(otherTab.id, true),
        ).joinBlocking()

        assertTrue(tab.content.loading)
        assertTrue(otherTab.content.loading)
    }

    @Test
    fun `UpdateRefreshCanceledStateAction updates refreshCanceled state`() {
        assertFalse(tab.content.refreshCanceled)
        assertFalse(otherTab.content.refreshCanceled)

        store.dispatch(ContentAction.UpdateRefreshCanceledStateAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.refreshCanceled)
        assertFalse(otherTab.content.refreshCanceled)

        store.dispatch(ContentAction.UpdateRefreshCanceledStateAction(tab.id, false)).joinBlocking()

        assertFalse(tab.content.refreshCanceled)
        assertFalse(otherTab.content.refreshCanceled)

        store.dispatch(ContentAction.UpdateRefreshCanceledStateAction(tab.id, true)).joinBlocking()
        store.dispatch(ContentAction.UpdateRefreshCanceledStateAction(otherTab.id, true)).joinBlocking()

        assertTrue(tab.content.refreshCanceled)
        assertTrue(otherTab.content.refreshCanceled)
    }

    @Test
    fun `UpdateTitleAction updates title`() {
        val newTitle = "This is a title"

        assertNotEquals(newTitle, tab.content.title)
        assertNotEquals(newTitle, otherTab.content.title)

        store.dispatch(
            ContentAction.UpdateTitleAction(tab.id, newTitle),
        ).joinBlocking()

        assertEquals(newTitle, tab.content.title)
        assertNotEquals(newTitle, otherTab.content.title)
    }

    @Test
    fun `UpdatePreviewImageAction updates previewImageUrl state`() {
        val newPreviewImageUrl = "https://test.com/og-image-url"

        assertNotEquals(newPreviewImageUrl, tab.content.previewImageUrl)
        assertNotEquals(newPreviewImageUrl, otherTab.content.previewImageUrl)

        store.dispatch(
            ContentAction.UpdatePreviewImageAction(tab.id, newPreviewImageUrl),
        ).joinBlocking()

        assertEquals(newPreviewImageUrl, tab.content.previewImageUrl)
        assertNotEquals(newPreviewImageUrl, otherTab.content.previewImageUrl)
    }

    @Test
    fun `UpdateProgressAction updates progress`() {
        assertEquals(0, tab.content.progress)
        assertEquals(0, otherTab.content.progress)

        store.dispatch(ContentAction.UpdateProgressAction(tab.id, 75)).joinBlocking()

        assertEquals(75, tab.content.progress)
        assertEquals(0, otherTab.content.progress)

        store.dispatch(ContentAction.UpdateProgressAction(otherTab.id, 25)).joinBlocking()
        store.dispatch(ContentAction.UpdateProgressAction(tab.id, 85)).joinBlocking()

        assertEquals(85, tab.content.progress)
        assertEquals(25, otherTab.content.progress)
    }

    @Test
    fun `UpdateSearchTermsAction updates URL`() {
        val searchTerms = "Hello World"

        assertNotEquals(searchTerms, tab.content.searchTerms)
        assertNotEquals(searchTerms, otherTab.content.searchTerms)

        store.dispatch(
            ContentAction.UpdateSearchTermsAction(tab.id, searchTerms),
        ).joinBlocking()

        assertEquals(searchTerms, tab.content.searchTerms)
        assertNotEquals(searchTerms, otherTab.content.searchTerms)
    }

    @Test
    fun `UpdateSecurityInfo updates securityInfo`() {
        val newSecurityInfo = SecurityInfoState(true, "mozilla.org", "The Mozilla Team")

        assertNotEquals(newSecurityInfo, tab.content.securityInfo)
        assertNotEquals(newSecurityInfo, otherTab.content.securityInfo)

        store.dispatch(
            ContentAction.UpdateSecurityInfoAction(tab.id, newSecurityInfo),
        ).joinBlocking()

        assertEquals(newSecurityInfo, tab.content.securityInfo)
        assertNotEquals(newSecurityInfo, otherTab.content.securityInfo)

        assertEquals(true, tab.content.securityInfo.secure)
        assertEquals("mozilla.org", tab.content.securityInfo.host)
        assertEquals("The Mozilla Team", tab.content.securityInfo.issuer)
    }

    @Test
    fun `UpdateIconAction updates icon`() {
        val icon = spy(Bitmap::class.java)

        assertNotEquals(icon, tab.content.icon)
        assertNotEquals(icon, otherTab.content.icon)

        store.dispatch(
            ContentAction.UpdateIconAction(tab.id, tab.content.url, icon),
        ).joinBlocking()

        assertEquals(icon, tab.content.icon)
        assertNotEquals(icon, otherTab.content.icon)
    }

    @Test
    fun `UpdateIconAction does not update icon if page URL is different`() {
        val icon = spy(Bitmap::class.java)

        assertNotEquals(icon, tab.content.icon)
        assertNotEquals(icon, otherTab.content.icon)

        store.dispatch(
            ContentAction.UpdateIconAction(tab.id, "https://different.example.org", icon),
        ).joinBlocking()

        assertNull(tab.content.icon)
    }

    @Test
    fun `RemoveIconAction removes icon`() {
        val icon = spy(Bitmap::class.java)

        assertNotEquals(icon, tab.content.icon)

        store.dispatch(
            ContentAction.UpdateIconAction(tab.id, tab.content.url, icon),
        ).joinBlocking()

        assertEquals(icon, tab.content.icon)

        store.dispatch(
            ContentAction.RemoveIconAction(tab.id),
        ).joinBlocking()

        assertNull(tab.content.icon)
    }

    @Test
    fun `Updating custom tab`() {
        val customTab = createCustomTab("https://getpocket.com")
        val otherCustomTab = createCustomTab("https://www.google.com")

        store.dispatch(CustomTabListAction.AddCustomTabAction(customTab)).joinBlocking()
        store.dispatch(CustomTabListAction.AddCustomTabAction(otherCustomTab)).joinBlocking()

        store.dispatch(ContentAction.UpdateUrlAction(customTab.id, "https://www.example.org")).joinBlocking()
        store.dispatch(ContentAction.UpdateTitleAction(customTab.id, "I am a custom tab")).joinBlocking()

        val updatedCustomTab = store.state.findCustomTab(customTab.id)!!
        val updatedOtherCustomTab = store.state.findCustomTab(otherCustomTab.id)!!

        assertEquals("https://www.example.org", updatedCustomTab.content.url)
        assertNotEquals("https://www.example.org", updatedOtherCustomTab.content.url)
        assertNotEquals("https://www.example.org", tab.content.url)
        assertNotEquals("https://www.example.org", otherTab.content.url)

        assertEquals("I am a custom tab", updatedCustomTab.content.title)
        assertNotEquals("I am a custom tab", updatedOtherCustomTab.content.title)
        assertNotEquals("I am a custom tab", tab.content.title)
        assertNotEquals("I am a custom tab", otherTab.content.title)
    }

    @Test
    fun `UpdateDownloadAction updates download`() {
        assertNull(tab.content.download)

        val download1 = DownloadState(
            url = "https://www.mozilla.org",
            sessionId = tab.id,
        )

        store.dispatch(
            ContentAction.UpdateDownloadAction(tab.id, download1),
        ).joinBlocking()

        assertEquals(download1.url, tab.content.download?.url)
        assertEquals(download1.sessionId, tab.content.download?.sessionId)

        val download2 = DownloadState(
            url = "https://www.wikipedia.org",
            sessionId = tab.id,
        )

        store.dispatch(
            ContentAction.UpdateDownloadAction(tab.id, download2),
        ).joinBlocking()

        assertEquals(download2.url, tab.content.download?.url)
        assertEquals(download2.sessionId, tab.content.download?.sessionId)
    }

    @Test
    fun `ConsumeDownloadAction removes download`() {
        val download = DownloadState(
            id = "1337",
            url = "https://www.mozilla.org",
            sessionId = tab.id,
        )

        store.dispatch(
            ContentAction.UpdateDownloadAction(tab.id, download),
        ).joinBlocking()

        assertEquals(download, tab.content.download)

        store.dispatch(
            ContentAction.ConsumeDownloadAction(tab.id, downloadId = "1337"),
        ).joinBlocking()

        assertNull(tab.content.download)
    }

    @Test
    fun `CancelDownloadAction removes download`() {
        val download = DownloadState(
            id = "1337",
            url = "https://www.mozilla.org",
            sessionId = tab.id,
        )

        store.dispatch(
            ContentAction.UpdateDownloadAction(tab.id, download),
        ).joinBlocking()

        assertEquals(download, tab.content.download)

        store.dispatch(
            ContentAction.CancelDownloadAction(tab.id, downloadId = "1337"),
        ).joinBlocking()

        assertNull(tab.content.download)
    }

    @Test
    fun `ConsumeDownloadAction does not remove download with different id`() {
        val download = DownloadState(
            id = "1337",
            url = "https://www.mozilla.org",
            sessionId = tab.id,
        )

        store.dispatch(
            ContentAction.UpdateDownloadAction(tab.id, download),
        ).joinBlocking()

        assertEquals(download, tab.content.download)

        store.dispatch(
            ContentAction.ConsumeDownloadAction(tab.id, downloadId = "4223"),
        ).joinBlocking()

        assertNotNull(tab.content.download)
    }

    @Test
    fun `UpdateHitResultAction updates hit result`() {
        assertNull(tab.content.hitResult)

        val hitResult1: HitResult = HitResult.UNKNOWN("file://foo")

        store.dispatch(
            ContentAction.UpdateHitResultAction(tab.id, hitResult1),
        ).joinBlocking()

        assertEquals(hitResult1, tab.content.hitResult)

        val hitResult2: HitResult = HitResult.UNKNOWN("file://bar")

        store.dispatch(
            ContentAction.UpdateHitResultAction(tab.id, hitResult2),
        ).joinBlocking()

        assertEquals(hitResult2, tab.content.hitResult)
    }

    @Test
    fun `ConsumeHitResultAction removes hit result`() {
        val hitResult: HitResult = HitResult.UNKNOWN("file://foo")

        store.dispatch(
            ContentAction.UpdateHitResultAction(tab.id, hitResult),
        ).joinBlocking()

        assertEquals(hitResult, tab.content.hitResult)

        store.dispatch(
            ContentAction.ConsumeHitResultAction(tab.id),
        ).joinBlocking()

        assertNull(tab.content.hitResult)
    }

    @Test
    fun `UpdatePromptRequestAction updates requests`() {
        assertTrue(tab.content.promptRequests.isEmpty())

        val promptRequest1: PromptRequest = mock<PromptRequest.SingleChoice>()

        store.dispatch(
            ContentAction.UpdatePromptRequestAction(tab.id, promptRequest1),
        ).joinBlocking()

        assertEquals(1, tab.content.promptRequests.size)
        assertEquals(promptRequest1, tab.content.promptRequests[0])

        val promptRequest2: PromptRequest = mock<PromptRequest.MultipleChoice>()

        store.dispatch(
            ContentAction.UpdatePromptRequestAction(tab.id, promptRequest2),
        ).joinBlocking()

        assertEquals(2, tab.content.promptRequests.size)
        assertEquals(promptRequest1, tab.content.promptRequests[0])
        assertEquals(promptRequest2, tab.content.promptRequests[1])
    }

    @Test
    fun `ConsumePromptRequestAction removes request`() {
        val promptRequest: PromptRequest = mock<PromptRequest.SingleChoice>()

        store.dispatch(
            ContentAction.UpdatePromptRequestAction(tab.id, promptRequest),
        ).joinBlocking()

        assertEquals(1, tab.content.promptRequests.size)
        assertEquals(promptRequest, tab.content.promptRequests[0])

        store.dispatch(
            ContentAction.ConsumePromptRequestAction(tab.id, promptRequest),
        ).joinBlocking()

        assertTrue(tab.content.promptRequests.isEmpty())
    }

    @Test
    fun `AddFindResultAction adds result`() {
        assertTrue(tab.content.findResults.isEmpty())

        val result: FindResultState = mock()
        store.dispatch(
            ContentAction.AddFindResultAction(tab.id, result),
        ).joinBlocking()

        assertEquals(1, tab.content.findResults.size)
        assertEquals(result, tab.content.findResults.last())

        val result2: FindResultState = mock()
        store.dispatch(
            ContentAction.AddFindResultAction(tab.id, result2),
        ).joinBlocking()

        assertEquals(2, tab.content.findResults.size)
        assertEquals(result2, tab.content.findResults.last())
    }

    @Test
    fun `ClearFindResultsAction removes all results`() {
        store.dispatch(
            ContentAction.AddFindResultAction(tab.id, mock()),
        ).joinBlocking()

        store.dispatch(
            ContentAction.AddFindResultAction(tab.id, mock()),
        ).joinBlocking()

        assertEquals(2, tab.content.findResults.size)

        store.dispatch(
            ContentAction.ClearFindResultsAction(tab.id),
        ).joinBlocking()

        assertTrue(tab.content.findResults.isEmpty())
    }

    @Test
    fun `UpdateWindowRequestAction updates request`() {
        assertNull(tab.content.windowRequest)

        val windowRequest1: WindowRequest = mock()

        store.dispatch(
            ContentAction.UpdateWindowRequestAction(tab.id, windowRequest1),
        ).joinBlocking()

        assertEquals(windowRequest1, tab.content.windowRequest)

        val windowRequest2: WindowRequest = mock()

        store.dispatch(
            ContentAction.UpdateWindowRequestAction(tab.id, windowRequest2),
        ).joinBlocking()

        assertEquals(windowRequest2, tab.content.windowRequest)
    }

    @Test
    fun `ConsumeWindowRequestAction removes request`() {
        val windowRequest: WindowRequest = mock()

        store.dispatch(
            ContentAction.UpdateWindowRequestAction(tab.id, windowRequest),
        ).joinBlocking()

        assertEquals(windowRequest, tab.content.windowRequest)

        store.dispatch(
            ContentAction.ConsumeWindowRequestAction(tab.id),
        ).joinBlocking()

        assertNull(tab.content.windowRequest)
    }

    @Test
    fun `UpdateBackNavigationStateAction updates canGoBack`() {
        assertFalse(tab.content.canGoBack)
        assertFalse(otherTab.content.canGoBack)

        store.dispatch(ContentAction.UpdateBackNavigationStateAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.canGoBack)
        assertFalse(otherTab.content.canGoBack)

        store.dispatch(ContentAction.UpdateBackNavigationStateAction(tab.id, false)).joinBlocking()

        assertFalse(tab.content.canGoBack)
        assertFalse(otherTab.content.canGoBack)
    }

    @Test
    fun `UpdateForwardNavigationStateAction updates canGoForward`() {
        assertFalse(tab.content.canGoForward)
        assertFalse(otherTab.content.canGoForward)

        store.dispatch(ContentAction.UpdateForwardNavigationStateAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.canGoForward)
        assertFalse(otherTab.content.canGoForward)

        store.dispatch(ContentAction.UpdateForwardNavigationStateAction(tab.id, false)).joinBlocking()

        assertFalse(tab.content.canGoForward)
        assertFalse(otherTab.content.canGoForward)
    }

    @Test
    fun `UpdateWebAppManifestAction updates web app manifest`() {
        val manifest = WebAppManifest(
            name = "Mozilla",
            startUrl = "https://mozilla.org",
        )

        assertNotEquals(manifest, tab.content.webAppManifest)
        assertNotEquals(manifest, otherTab.content.webAppManifest)

        store.dispatch(
            ContentAction.UpdateWebAppManifestAction(tab.id, manifest),
        ).joinBlocking()

        assertEquals(manifest, tab.content.webAppManifest)
        assertNotEquals(manifest, otherTab.content.webAppManifest)
    }

    @Test
    fun `RemoveWebAppManifestAction removes web app manifest`() {
        val manifest = WebAppManifest(
            name = "Mozilla",
            startUrl = "https://mozilla.org",
        )

        assertNotEquals(manifest, tab.content.webAppManifest)

        store.dispatch(
            ContentAction.UpdateWebAppManifestAction(tab.id, manifest),
        ).joinBlocking()

        assertEquals(manifest, tab.content.webAppManifest)

        store.dispatch(
            ContentAction.RemoveWebAppManifestAction(tab.id),
        ).joinBlocking()

        assertNull(tab.content.webAppManifest)
    }

    @Test
    fun `UpdateHistoryStateAction updates history state`() {
        val historyState = HistoryState(
            items = listOf(
                HistoryItem("Mozilla", "https://mozilla.org"),
                HistoryItem("Firefox", "https://firefox.com"),
            ),
            currentIndex = 1,
        )

        assertNotEquals(historyState, tab.content.history)
        assertNotEquals(historyState, otherTab.content.history)

        store.dispatch(
            ContentAction.UpdateHistoryStateAction(tab.id, historyState.items, historyState.currentIndex),
        ).joinBlocking()

        assertEquals(historyState, tab.content.history)
        assertNotEquals(historyState, otherTab.content.history)
    }

    @Test
    fun `UpdateLoadRequestAction updates load request state`() {
        val loadRequestUrl = "https://mozilla.org"

        store.dispatch(
            ContentAction.UpdateLoadRequestAction(tab.id, LoadRequestState(loadRequestUrl, true, false)),
        ).joinBlocking()

        assertNotNull(tab.content.loadRequest)
        assertEquals(loadRequestUrl, tab.content.loadRequest!!.url)
        assertTrue(tab.content.loadRequest!!.triggeredByRedirect)
        assertFalse(tab.content.loadRequest!!.triggeredByUser)
    }

    @Test
    fun `UpdateDesktopModeEnabledAction updates desktopModeEnabled`() {
        assertFalse(tab.content.desktopMode)
        assertFalse(otherTab.content.desktopMode)

        store.dispatch(ContentAction.UpdateTabDesktopMode(tab.id, true)).joinBlocking()

        assertTrue(tab.content.desktopMode)
        assertFalse(otherTab.content.desktopMode)

        store.dispatch(ContentAction.UpdateTabDesktopMode(tab.id, false)).joinBlocking()

        assertFalse(tab.content.desktopMode)
        assertFalse(otherTab.content.desktopMode)
    }

    @Test
    fun `WHEN dispatching NotificationChangedAction THEN notificationChanged state will be updated`() {
        assertFalse(tab.content.permissionHighlights.notificationChanged)

        store.dispatch(NotificationChangedAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.notificationChanged)
    }

    @Test
    fun `WHEN dispatching CameraChangedAction THEN cameraChanged state will be updated`() {
        assertFalse(tab.content.permissionHighlights.cameraChanged)

        store.dispatch(CameraChangedAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.cameraChanged)
    }

    @Test
    fun `WHEN dispatching LocationChangedAction THEN locationChanged state will be updated`() {
        assertFalse(tab.content.permissionHighlights.locationChanged)

        store.dispatch(LocationChangedAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.locationChanged)
    }

    @Test
    fun `WHEN dispatching MicrophoneChangedAction THEN locationChanged state will be updated`() {
        assertFalse(tab.content.permissionHighlights.microphoneChanged)

        store.dispatch(MicrophoneChangedAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.microphoneChanged)
    }

    @Test
    fun `WHEN dispatching PersistentStorageChangedAction THEN persistentStorageChanged state will be updated`() {
        assertFalse(tab.content.permissionHighlights.persistentStorageChanged)

        store.dispatch(PersistentStorageChangedAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.persistentStorageChanged)
    }

    @Test
    fun `WHEN dispatching MediaKeySystemAccesChangedAction THEN mediaKeySystemAccessChanged state will be updated`() {
        assertFalse(tab.content.permissionHighlights.mediaKeySystemAccessChanged)

        store.dispatch(MediaKeySystemAccesChangedAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.mediaKeySystemAccessChanged)
    }

    @Test
    fun `WHEN dispatching AutoPlayAudibleChangedAction THEN autoPlayAudibleChanged state will be updated`() {
        assertFalse(tab.content.permissionHighlights.autoPlayAudibleChanged)

        store.dispatch(AutoPlayAudibleChangedAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.autoPlayAudibleChanged)
    }

    @Test
    fun `WHEN dispatching AutoPlayInAudibleChangedAction THEN autoPlayAudibleChanged state will be updated`() {
        assertFalse(tab.content.permissionHighlights.autoPlayInaudibleChanged)

        store.dispatch(AutoPlayInAudibleChangedAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.autoPlayInaudibleChanged)
    }

    @Test
    fun `WHEN dispatching AutoPlayAudibleBlockingAction THEN autoPlayAudibleBlocking state will be updated`() {
        assertFalse(tab.content.permissionHighlights.autoPlayAudibleBlocking)

        store.dispatch(AutoPlayAudibleBlockingAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.autoPlayAudibleBlocking)
    }

    @Test
    fun `WHEN dispatching AutoPlayInAudibleBlockingAction THEN autoPlayInaudibleBlocking state will be updated`() {
        assertFalse(tab.content.permissionHighlights.autoPlayInaudibleBlocking)

        store.dispatch(AutoPlayInAudibleBlockingAction(tab.id, true)).joinBlocking()

        assertTrue(tab.content.permissionHighlights.autoPlayInaudibleBlocking)
    }

    @Test
    fun `WHEN dispatching Reset THEN permissionHighlights state will be update to its default value`() {
        store.dispatch(AutoPlayInAudibleBlockingAction(tab.id, true)).joinBlocking()

        assertEquals(
            PermissionHighlightsState(autoPlayInaudibleBlocking = true),
            tab.content.permissionHighlights,
        )

        with(store) { dispatch(Reset(tab.id)).joinBlocking() }

        assertEquals(PermissionHighlightsState(), tab.content.permissionHighlights)
    }

    @Test
    fun `UpdateAppIntentAction updates request`() {
        assertTrue(tab.content.promptRequests.isEmpty())

        val appIntent1: AppIntentState = mock()

        store.dispatch(
            ContentAction.UpdateAppIntentAction(tab.id, appIntent1),
        ).joinBlocking()

        assertEquals(appIntent1, tab.content.appIntent)

        val appIntent2: AppIntentState = mock()

        store.dispatch(
            ContentAction.UpdateAppIntentAction(tab.id, appIntent2),
        ).joinBlocking()

        assertEquals(appIntent2, tab.content.appIntent)
    }

    @Test
    fun `ConsumeAppIntentAction removes request`() {
        val appIntent: AppIntentState = mock()

        store.dispatch(
            ContentAction.UpdateAppIntentAction(tab.id, appIntent),
        ).joinBlocking()

        assertEquals(appIntent, tab.content.appIntent)

        store.dispatch(
            ContentAction.ConsumeAppIntentAction(tab.id),
        ).joinBlocking()

        assertNull(tab.content.appIntent)
    }

    @Test
    fun `CheckForFormDataAction updates hasFormData`() {
        assertFalse(tab.content.hasFormData)

        store.dispatch(
            ContentAction.UpdateHasFormDataAction(tab.id, true),
        ).joinBlocking()

        assertTrue(tab.content.hasFormData)

        store.dispatch(
            ContentAction.UpdateHasFormDataAction(tab.id, false),
        ).joinBlocking()

        assertFalse(tab.content.hasFormData)
    }

    @Test
    fun `merge permission request if same request`() {
        val url = "https://www.mozilla.org"

        val request1: PermissionRequest = mock {
            whenever(permissions).thenReturn(listOf(ContentGeoLocation(id = "permission")))
            whenever(uri).thenReturn(url)
        }
        val request2: PermissionRequest = mock {
            whenever(permissions).thenReturn(listOf(ContentGeoLocation(id = "permission")))
            whenever(uri).thenReturn(url)
        }

        store.dispatch(ContentAction.UpdatePermissionsRequest(tab.id, request1))
        store.dispatch(ContentAction.UpdatePermissionsRequest(tab.id, request2))
        store.waitUntilIdle()

        verify(request1).merge(request2)
    }

    @Test
    fun `merge app permission request if same request`() {
        val request1: PermissionRequest = mock {
            whenever(permissions).thenReturn(listOf(AppLocationCoarse(id = "permission")))
        }
        val request2: PermissionRequest = mock {
            whenever(permissions).thenReturn(listOf(AppLocationCoarse(id = "permission")))
        }

        store.dispatch(ContentAction.UpdateAppPermissionsRequest(tab.id, request1))
        store.dispatch(ContentAction.UpdateAppPermissionsRequest(tab.id, request2))
        store.waitUntilIdle()

        verify(request1).merge(request2)
    }
}
