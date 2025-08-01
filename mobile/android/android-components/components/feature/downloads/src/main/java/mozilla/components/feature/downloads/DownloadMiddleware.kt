/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.feature.downloads

import android.app.DownloadManager
import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import androidx.annotation.VisibleForTesting
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import mozilla.components.browser.state.action.BrowserAction
import mozilla.components.browser.state.action.ContentAction
import mozilla.components.browser.state.action.DownloadAction
import mozilla.components.browser.state.action.TabListAction
import mozilla.components.browser.state.selector.findTabOrCustomTab
import mozilla.components.browser.state.selector.getNormalOrPrivateTabs
import mozilla.components.browser.state.state.BrowserState
import mozilla.components.browser.state.state.content.DownloadState
import mozilla.components.browser.state.state.content.DownloadState.Status.CANCELLED
import mozilla.components.browser.state.state.content.DownloadState.Status.COMPLETED
import mozilla.components.browser.state.state.content.DownloadState.Status.FAILED
import mozilla.components.feature.downloads.AbstractFetchDownloadService.Companion.ACTION_REMOVE_PRIVATE_DOWNLOAD
import mozilla.components.lib.state.Middleware
import mozilla.components.lib.state.MiddlewareContext
import mozilla.components.lib.state.Store
import mozilla.components.support.base.android.DefaultPowerManagerInfoProvider
import mozilla.components.support.base.android.StartForegroundService
import mozilla.components.support.base.log.logger.Logger
import java.io.File
import kotlin.coroutines.CoroutineContext

/**
 * [Middleware] implementation for managing downloads via the provided download service. Its
 * purpose is to react to global download state changes (e.g. of [BrowserState.downloads])
 * and notify the download service, as needed.
 */
@Suppress("ComplexMethod")
class DownloadMiddleware(
    private val applicationContext: Context,
    private val downloadServiceClass: Class<*>,
    private val deleteFileFromStorage: () -> Boolean,
    coroutineContext: CoroutineContext = Dispatchers.IO,
    @get:VisibleForTesting
    internal val downloadStorage: DownloadStorage = DownloadStorage(applicationContext),
    private val startForegroundService: StartForegroundService = StartForegroundService(
        powerManagerInfoProvider = DefaultPowerManagerInfoProvider(applicationContext),
    ),
) : Middleware<BrowserState, BrowserAction> {
    private val logger = Logger("DownloadMiddleware")

    private var scope = CoroutineScope(coroutineContext)

    override fun invoke(
        context: MiddlewareContext<BrowserState, BrowserAction>,
        next: (BrowserAction) -> Unit,
        action: BrowserAction,
    ) {
        when (action) {
            is DownloadAction.RemoveDownloadAction -> removeDownload(action.downloadId, context.store)
            is DownloadAction.RemoveAllDownloadsAction -> removeDownloads()
            is DownloadAction.UpdateDownloadAction -> updateDownload(action.download, context)
            is DownloadAction.RestoreDownloadsStateAction -> restoreDownloads(context.store)
            is DownloadAction.RemoveDeletedDownloads -> removeDeletedDownloads(context.store)
            is ContentAction.CancelDownloadAction -> closeDownloadResponse(context.store, action.sessionId)
            is DownloadAction.AddDownloadAction -> {
                if (!action.download.private && !saveDownload(context.store, action.download)) {
                    // The download was already added before, so we are ignoring this request.
                    logger.debug(
                        "Ignored add action for ${action.download.id} " +
                            "download already in store.downloads",
                    )
                    return
                }
            }
            else -> {
                // no-op
            }
        }

        next(action)

        when (action) {
            is TabListAction.RemoveAllTabsAction,
            is TabListAction.RemoveAllPrivateTabsAction,
            -> removePrivateNotifications(context.store)
            is TabListAction.RemoveTabsAction,
            is TabListAction.RemoveTabAction,
            -> {
                val privateTabs = context.store.state.getNormalOrPrivateTabs(private = true)
                if (privateTabs.isEmpty()) {
                    removePrivateNotifications(context.store)
                }
            }
            is DownloadAction.AddDownloadAction -> sendDownloadIntent(action.download)
            is DownloadAction.RestoreDownloadStateAction -> sendDownloadIntent(action.download)
            else -> {
                // no-op
            }
        }
    }

    private fun removeDownload(
        downloadId: String,
        store: Store<BrowserState, BrowserAction>,
    ) {
        val downloadToDelete = store.state.downloads[downloadId] ?: return

        scope.launch {
            if (deleteFileFromStorage()) {
                removeFileFromStorage(downloadToDelete.filePath)
            }

            downloadStorage.remove(downloadToDelete)
            logger.debug("Removed download ${downloadToDelete.fileName} from the storage")
        }
    }

    private fun removeFileFromStorage(filePath: String) {
        val file = File(filePath)
        if (!file.exists()) {
            logger.warn("File to delete not found: $filePath")
            return
        }

        val deletedSuccessfully = deleteMediaFile(applicationContext.contentResolver, file)

        if (deletedSuccessfully) {
            logger.debug("Successfully deleted file: $filePath")
        } else {
            logger.error("Failed to delete file: $filePath (OS Version: ${Build.VERSION.SDK_INT})")
        }
    }

    private fun removeDownloads() = scope.launch {
        downloadStorage.removeAllDownloads()
    }

    private fun updateDownload(updated: DownloadState, context: MiddlewareContext<BrowserState, BrowserAction>) {
        if (updated.private) return
        context.state.downloads[updated.id]?.let { old ->
            // To not overwhelm the storage, we only send updates that are relevant,
            // we only care about properties, that we are stored on the storage.
            if (!DownloadStorage.isSameDownload(old, updated)) {
                scope.launch {
                    downloadStorage.update(updated)
                }
                logger.debug("Updated download ${updated.fileName} on the storage")
            }
        }
    }

    private fun removeDeletedDownloads(store: Store<BrowserState, BrowserAction>) = scope.launch {
        downloadStorage.getDownloadsList()
            .filter { (it.status == COMPLETED || it.status == CANCELLED) && !File(it.filePath).exists() }
            .forEach { download ->
                store.dispatch(DownloadAction.RemoveDownloadAction(download.id))
            }
    }

    private fun restoreDownloads(store: Store<BrowserState, BrowserAction>) = scope.launch {
        downloadStorage.getDownloadsList().forEach { download ->
            if (!File(download.filePath).exists()) {
                downloadStorage.remove(download)
                logger.debug("Removed deleted download ${download.fileName} from the storage")
            } else if (!store.state.downloads.containsKey(download.id) && !download.private) {
                store.dispatch(DownloadAction.RestoreDownloadStateAction(download))
                logger.debug("Download restored from the storage ${download.fileName}")
            }
        }
    }

    @VisibleForTesting
    internal fun saveDownload(store: Store<BrowserState, BrowserAction>, download: DownloadState): Boolean {
        return if (!store.state.downloads.containsKey(download.id) && !download.private) {
            scope.launch {
                downloadStorage.add(download)
                logger.debug("Added download ${download.fileName} to the storage")
            }
            true
        } else {
            false
        }
    }

    @VisibleForTesting
    internal fun closeDownloadResponse(store: Store<BrowserState, BrowserAction>, tabId: String) {
        store.state.findTabOrCustomTab(tabId)?.let {
            it.content.download?.response?.close()
        }
    }

    @VisibleForTesting
    internal fun sendDownloadIntent(download: DownloadState) {
        if (download.status !in arrayOf(COMPLETED, CANCELLED, FAILED)) {
            val intent = Intent(applicationContext, downloadServiceClass)
            intent.putExtra(DownloadManager.EXTRA_DOWNLOAD_ID, download.id)
            startForegroundService(intent)
            logger.debug("Sending download intent ${download.fileName}")
        }
    }

    @VisibleForTesting
    internal fun startForegroundService(intent: Intent) {
        /**
         * @see [StartForegroundService]
         */
        startForegroundService {
            ContextCompat.startForegroundService(applicationContext, intent)
        }
    }

    @VisibleForTesting
    internal fun removeStatusBarNotification(store: Store<BrowserState, BrowserAction>, download: DownloadState) {
        download.notificationId?.let {
            val intent = Intent(applicationContext, downloadServiceClass)
            intent.action = ACTION_REMOVE_PRIVATE_DOWNLOAD
            intent.putExtra(DownloadManager.EXTRA_DOWNLOAD_ID, download.id)
            applicationContext.startService(intent)
            store.dispatch(DownloadAction.DismissDownloadNotificationAction(download.id))
        }
    }

    @VisibleForTesting
    internal fun removePrivateNotifications(store: Store<BrowserState, BrowserAction>) {
        val privateDownloads = store.state.downloads.filterValues { it.private }
        privateDownloads.forEach { removeStatusBarNotification(store, it.value) }
    }

    @Suppress("TooGenericExceptionCaught")
    @VisibleForTesting
    internal fun deleteMediaFile(contentResolver: ContentResolver, file: File): Boolean {
        val fileUri = getUriFromFile(contentResolver, file) ?: return false
        try {
            val rowsDeleted = contentResolver.delete(fileUri, null, null)
            return rowsDeleted > 0
        } catch (e: SecurityException) {
            logger.debug("SecurityException: ${e.message}")
        } catch (e: Exception) {
            logger.debug("Error deleting file: ${e.message}")
        }
        return false
    }

    private fun getUriFromFile(contentResolver: ContentResolver, file: File): Uri? {
        val cursor = contentResolver.query(
            MediaStore.Files.getContentUri("external"),
            arrayOf(MediaStore.Files.FileColumns._ID),
            "${MediaStore.Files.FileColumns.DATA}=?",
            arrayOf(file.absolutePath),
            null,
        )
        cursor?.use {
            if (it.moveToFirst()) {
                val id = it.getLong(it.getColumnIndexOrThrow(MediaStore.Files.FileColumns._ID))
                return Uri.withAppendedPath(MediaStore.Files.getContentUri("external"), "" + id)
            }
        }
        return null
    }
}
