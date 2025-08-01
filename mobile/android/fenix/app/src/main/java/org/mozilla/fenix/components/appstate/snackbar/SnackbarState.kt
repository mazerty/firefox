/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components.appstate.snackbar

import mozilla.components.browser.state.state.content.DownloadState
import mozilla.components.concept.storage.BookmarkNode
import mozilla.components.concept.sync.TabData

/**
 * The state of the snackbar to display.
 */
sealed class SnackbarState {
    /**
     * There is no snackbar to display.
     *
     * @property previous The previous displayed snackbar, if any.
     */
    data class None(
        val previous: SnackbarState? = null,
    ) : SnackbarState()

    /**
     * Dismiss an existing snackbar that is displayed with an indefinite duration.
     *
     * @property previous The previous displayed snackbar, if any.
     */
    data class Dismiss(
        val previous: SnackbarState? = null,
    ) : SnackbarState()

    /**
     * Display a snackbar of the newly added shortcut.
     */
    data object ShortcutAdded : SnackbarState()

    /**
     * Display a snackbar of the removed shortcut.
     */
    data object ShortcutRemoved : SnackbarState()

    /**
     * Display a snackbar when deleting browsing data before quitting.
     */
    data object DeletingBrowserDataInProgress : SnackbarState()

    /**
     * Display a snackbar of the newly added bookmark.
     *
     * @property guidToEdit The guid of the newly added bookmark or null.
     * @property parentNode The [BookmarkNode] representing the folder the bookmark was added to, if any.
     */
    data class BookmarkAdded(
        val guidToEdit: String?,
        val parentNode: BookmarkNode?,
    ) : SnackbarState()

    /**
     * Display a snackbar informing of a bookmarks that has just been deleted.
     *
     * @property title The title of the bookmark that was deleted.
     */
    data class BookmarkDeleted(val title: String?) : SnackbarState()

    /**
     * There is a translation in progression for the given [sessionId].
     *
     * @property sessionId The ID of the session being translated.
     */
    data class TranslationInProgress(val sessionId: String?) : SnackbarState()

    /**
     * Display a snackbar when the user's account is authenticated.
     */
    data object UserAccountAuthenticated : SnackbarState()

    /**
     * Display a snackbar when sharing to another application failed.
     */
    data object ShareToAppFailed : SnackbarState()

    /**
     * Display a snackbar after sharing to whatsapp for the first time
     */
    data object ShareToWhatsApp : SnackbarState()

    /**
     * Display a snackbar when sharing tabs was successful.
     *
     * @property destination List of device IDs with which tabs were shared.
     * @property tabs List of tabs that were shared.
     */
    data class SharedTabsSuccessfully(
        val destination: List<String>,
        val tabs: List<TabData>,
    ) : SnackbarState()

    /**
     * Display a snackbar when sharing to another application failed.
     *
     * @property destination List of device IDs with which tabs were tried to be shared.
     * @property tabs List of tabs that were tried to be shared.
     */
    data class ShareTabsFailed(
        val destination: List<String>,
        val tabs: List<TabData>,
    ) : SnackbarState()

    /**
     * Display a snackbar when copying a link to the clipboard.
     */
    data object CopyLinkToClipboard : SnackbarState()

    /**
     * Display a snackbar when a crash report is sent
     */
    data object ReportSent : SnackbarState()

    /**
     * Display a snackbar when an URL has been copied to the clipboard.
     */
    data object URLCopiedToClipboard : SnackbarState()

    /**
     * Display a snackbar when the WebCompat report has been successfully submitted.
     */
    data object WebCompatReportSent : SnackbarState()

    /**
     * Display a snackbar when the current site's data has been deleted.
     */
    data object SiteDataCleared : SnackbarState()

    /**
     * Display a snackbar when the current tab has been closed.
     *
     * @property isPrivate Whether closed tab was private or not.
     */
    data class CurrentTabClosed(
        val isPrivate: Boolean,
    ) : SnackbarState()

    /**
     * Display a snackbar when a download is in progress.
     *
     * @property downloadId The unique identifier for the ongoing download.
     */
    data class DownloadInProgress(val downloadId: String) : SnackbarState()

    /**
     * Display a snackbar when a download has failed.
     *
     * @property fileName The name of the file that failed to download.
     */
    data class DownloadFailed(val fileName: String?) : SnackbarState()

    /**
     * Display a snackbar when a download has completed.
     *
     * @property downloadState The state object containing information about the completed download.
     */
    data class DownloadCompleted(val downloadState: DownloadState) : SnackbarState()

    /**
     * Display a snackbar when there is an error opening a file.
     *
     * @property downloadState The state object containing information about the failed download.
     */
    data class CannotOpenFileError(val downloadState: DownloadState) : SnackbarState()
}
