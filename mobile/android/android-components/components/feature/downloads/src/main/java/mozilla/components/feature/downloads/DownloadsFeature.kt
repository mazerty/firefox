/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.feature.downloads

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.ResolveInfo
import android.widget.Toast
import androidx.annotation.ColorRes
import androidx.annotation.VisibleForTesting
import androidx.annotation.VisibleForTesting.Companion.PRIVATE
import androidx.core.net.toUri
import androidx.fragment.app.FragmentManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.distinctUntilChangedBy
import kotlinx.coroutines.flow.mapNotNull
import mozilla.components.browser.state.selector.findTabOrCustomTabOrSelectedTab
import mozilla.components.browser.state.state.SessionState
import mozilla.components.browser.state.state.content.DownloadState
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.feature.downloads.DownloadDialogFragment.Companion.FRAGMENT_TAG
import mozilla.components.feature.downloads.dialog.DeniedPermissionDialogFragment
import mozilla.components.feature.downloads.ext.realFilenameOrGuessed
import mozilla.components.feature.downloads.facts.emitPromptDismissedFact
import mozilla.components.feature.downloads.facts.emitPromptDisplayedFact
import mozilla.components.feature.downloads.manager.AndroidDownloadManager
import mozilla.components.feature.downloads.manager.DownloadManager
import mozilla.components.feature.downloads.manager.noop
import mozilla.components.feature.downloads.manager.onDownloadStopped
import mozilla.components.feature.downloads.ui.DownloadAppChooserDialog
import mozilla.components.feature.downloads.ui.DownloaderApp
import mozilla.components.lib.state.ext.flowScoped
import mozilla.components.support.base.feature.LifecycleAwareFeature
import mozilla.components.support.base.feature.OnNeedToRequestPermissions
import mozilla.components.support.base.feature.PermissionsFeature
import mozilla.components.support.base.log.logger.Logger
import mozilla.components.support.ktx.android.content.appName
import mozilla.components.support.ktx.android.content.isPermissionGranted
import mozilla.components.support.ktx.kotlin.isSameOriginAs
import mozilla.components.support.utils.Browsers

/**
 * The name of the file to be downloaded.
 */
@JvmInline
value class Filename(val value: String)

/**
 * The size of the file to be downloaded expressed as the number of `bytes`.
 * The value will be `0` if the size is unknown.
 */
@JvmInline
value class ContentSize(val value: Long)

/**
 * The list of all applications that can perform a download, including this application.
 */
@JvmInline
value class ThirdPartyDownloaderApps(val value: List<DownloaderApp>)

/**
 * Callback for when the user picked a certain application with which to download the current file.
 */
@JvmInline
value class ThirdPartyDownloaderAppChosenCallback(val value: (DownloaderApp) -> Unit)

/**
 * Callback for when the positive button of a download dialog was tapped.
 */
@JvmInline
value class PositiveActionCallback(val value: () -> Unit)

/**
 * Callback for when the negative button of a download dialog was tapped.
 */
@JvmInline
value class NegativeActionCallback(val value: () -> Unit)

/**
 * Feature implementation to provide download functionality for the selected
 * session. The feature will subscribe to the selected session and listen
 * for downloads.
 *
 * @property applicationContext a reference to the application context.
 * @property onNeedToRequestPermissions a callback invoked when permissions
 * need to be requested before a download can be performed. Once the request
 * is completed, [onPermissionsResult] needs to be invoked.
 * @property onDownloadStopped a callback invoked when a download is paused or completed.
 * @property downloadManager a reference to the [DownloadManager] which is
 * responsible for performing the downloads.
 * @property store a reference to the application's [BrowserStore].
 * @property useCases [DownloadsUseCases] instance for consuming processed downloads.
 * @property fileSystemHelper A helper for file system operations.
 * @property fragmentManager a reference to a [FragmentManager]. If a fragment
 * manager is provided, a dialog will be shown before every download.
 * @property promptsStyling styling properties for the dialog.
 * @property onDownloadStartedListener a callback invoked when a download is started.
 * @property shouldForwardToThirdParties Indicates if downloads should be forward to third party apps,
 * if there are multiple apps a chooser dialog will shown.
 * @property customFirstPartyDownloadDialog An optional delegate for showing a dialog for a download
 * that will be processed by the current application.
 * @property customThirdPartyDownloadDialog An optional delegate for showing a dialog for a download
 * that can be processed by multiple installed applications including the current one.
 * @property fileHasNotEnoughStorageDialog A callback invoked when there is not enough storage space.
 */
@Suppress("LargeClass")
class DownloadsFeature(
    private val applicationContext: Context,
    private val store: BrowserStore,
    @get:VisibleForTesting(otherwise = PRIVATE)
    internal val useCases: DownloadsUseCases,
    private val fileSystemHelper: FileSystemHelper = DefaultFileSystemHelper(),
    override var onNeedToRequestPermissions: OnNeedToRequestPermissions = { },
    onDownloadStopped: onDownloadStopped = noop,
    private val downloadManager: DownloadManager = AndroidDownloadManager(applicationContext, store),
    private val tabId: String? = null,
    private val fragmentManager: FragmentManager? = null,
    private val promptsStyling: PromptsStyling? = null,
    private val onDownloadStartedListener: ((String) -> Unit) = {},
    private val shouldForwardToThirdParties: () -> Boolean = { false },
    private val customFirstPartyDownloadDialog: (
        (Filename, ContentSize, PositiveActionCallback, NegativeActionCallback) -> Unit
    )? = null,
    private val customThirdPartyDownloadDialog: (
        (ThirdPartyDownloaderApps, ThirdPartyDownloaderAppChosenCallback, NegativeActionCallback) -> Unit
    )? = null,
    private val fileHasNotEnoughStorageDialog: ((Filename) -> Unit) = {},
) : LifecycleAwareFeature, PermissionsFeature {

    private val logger = Logger("DownloadsFeature")

    var onDownloadStopped: onDownloadStopped
        get() = downloadManager.onDownloadStopped
        set(value) { downloadManager.onDownloadStopped = value }

    init {
        this.onDownloadStopped = onDownloadStopped
    }

    private var scope: CoroutineScope? = null

    @VisibleForTesting
    internal var dismissPromptScope: CoroutineScope? = null

    @VisibleForTesting
    internal var previousTab: SessionState? = null

    /**
     * Starts observing downloads on the selected session and sends them to the [DownloadManager]
     * to be processed.
     */
    override fun start() {
        // Dismiss the previous prompts when the user navigates to another site.
        // This prevents prompts from the previous page from covering content.
        dismissPromptScope = store.flowScoped { flow ->
            flow.mapNotNull { state -> state.findTabOrCustomTabOrSelectedTab(tabId) }
                .distinctUntilChangedBy { it.content.url }
                .collect {
                    val currentHost = previousTab?.content?.url
                    val newHost = it.content.url

                    // The user is navigating to another site
                    if (currentHost?.isSameOriginAs(newHost) == false) {
                        previousTab?.let { tab ->
                            // We have an old download request.
                            tab.content.download?.let { download ->
                                useCases.cancelDownloadRequest.invoke(tab.id, download.id)
                                dismissAllDownloadDialogs()
                                previousTab = null
                            }
                        }
                    }
                }
        }

        scope = store.flowScoped { flow ->
            flow.mapNotNull { state -> state.findTabOrCustomTabOrSelectedTab(tabId) }
                .distinctUntilChangedBy { it.content.download }
                .collect { state ->
                    state.content.download?.let { downloadState ->
                        previousTab = state
                        processDownload(state, downloadState)
                    }
                }
        }
    }

    /**
     * Calls the tryAgain function of the corresponding [DownloadManager]
     */
    @Suppress("Unused")
    fun tryAgain(id: String) {
        downloadManager.tryAgain(id)
    }

    /**
     * Stops observing downloads on the selected session.
     */
    override fun stop() {
        scope?.cancel()
        dismissPromptScope?.cancel()
        downloadManager.unregisterListeners()
    }

    /**
     * Notifies the [DownloadManager] that a new download must be processed.
     */
    @VisibleForTesting
    internal fun processDownload(tab: SessionState, download: DownloadState): Boolean {
        val apps = getDownloaderApps(applicationContext, download)
        // We only show the dialog If we have multiple apps that can handle the download.
        val shouldShowAppDownloaderDialog = shouldForwardToThirdParties() && apps.size > 1

        return if (shouldShowAppDownloaderDialog) {
            when (customThirdPartyDownloadDialog) {
                null -> showAppDownloaderDialog(tab, download, apps)
                else -> customThirdPartyDownloadDialog.invoke(
                    ThirdPartyDownloaderApps(apps),
                    ThirdPartyDownloaderAppChosenCallback {
                        onDownloaderAppSelected(it, tab, download)
                    },
                    NegativeActionCallback {
                        useCases.cancelDownloadRequest.invoke(tab.id, download.id)
                    },
                )
            }

            false
        } else {
            if (applicationContext.isPermissionGranted(downloadManager.permissions.asIterable())) {
                when {
                    customFirstPartyDownloadDialog != null && !download.skipConfirmation -> {
                        customFirstPartyDownloadDialog.invoke(
                            Filename(download.realFilenameOrGuessed),
                            ContentSize(download.contentLength ?: 0),
                            PositiveActionCallback {
                                startDownload(download)
                                useCases.consumeDownload.invoke(tab.id, download.id)
                            },
                            NegativeActionCallback {
                                useCases.cancelDownloadRequest.invoke(tab.id, download.id)
                            },
                        )
                        false
                    }

                    fragmentManager != null && !download.skipConfirmation -> {
                        showDownloadDialog(tab, download)
                        false
                    }

                    else -> {
                        useCases.consumeDownload(tab.id, download.id)
                        startDownload(download)
                    }
                }
            } else {
                onNeedToRequestPermissions(downloadManager.permissions)
                false
            }
        }
    }

    @VisibleForTesting
    internal fun startDownload(download: DownloadState): Boolean {
        fileSystemHelper.createDirectoryIfNotExists(download.directoryPath)

        if (isDownloadBiggerThanAvailableSpace(download)) {
            fileHasNotEnoughStorageDialog.invoke(
                Filename(download.realFilenameOrGuessed),
            )
            download.sessionId?.let { useCases.cancelDownloadRequest.invoke(it, download.id) }
            return false
        }

        val id = downloadManager.download(download)
        return if (id != null) {
            onDownloadStartedListener.invoke(download.id)
            true
        } else {
            showDownloadNotSupportedError()
            false
        }
    }

    /**
     * Notifies the feature that the permissions request was completed. It will then
     * either trigger or clear the pending download.
     */
    override fun onPermissionsResult(permissions: Array<String>, grantResults: IntArray) {
        if (permissions.isEmpty()) {
            // If we are requesting permissions while a permission prompt is already being displayed
            // then Android seems to call `onPermissionsResult` immediately with an empty permissions
            // list. In this case just ignore it.
            return
        }

        withActiveDownload { (tab, download) ->
            if (applicationContext.isPermissionGranted(downloadManager.permissions.asIterable())) {
                if (shouldForwardToThirdParties()) {
                    startDownload(download)
                    useCases.consumeDownload(tab.id, download.id)
                } else {
                    processDownload(tab, download)
                }
            } else {
                useCases.cancelDownloadRequest.invoke(tab.id, download.id)
                showPermissionDeniedDialog()
            }
        }
    }

    @VisibleForTesting(otherwise = PRIVATE)
    internal fun showDownloadNotSupportedError() {
        Toast.makeText(
            applicationContext,
            applicationContext.getString(
                R.string.mozac_feature_downloads_file_not_supported2,
                applicationContext.appName,
            ),
            Toast.LENGTH_LONG,
        ).show()
    }

    internal fun showDownloadDialog(
        tab: SessionState,
        download: DownloadState,
        dialog: DownloadDialogFragment = getDownloadDialog(),
    ) {
        dialog.setDownload(download)

        dialog.onStartDownload = {
            startDownload(download)
            useCases.consumeDownload.invoke(tab.id, download.id)
        }

        dialog.onCancelDownload = {
            useCases.cancelDownloadRequest.invoke(tab.id, download.id)
        }

        if (!isAlreadyADownloadDialog() && fragmentManager != null && !fragmentManager.isDestroyed) {
            emitPromptDisplayedFact()
            dialog.showNow(fragmentManager, FRAGMENT_TAG)
        }
    }

    private fun getDownloadDialog(): DownloadDialogFragment {
        return findPreviousDownloadDialogFragment() ?: SimpleDownloadDialogFragment.newInstance(
            promptsStyling = promptsStyling,
        )
    }

    @VisibleForTesting
    internal fun isDownloadBiggerThanAvailableSpace(download: DownloadState): Boolean {
        download.contentLength?.let { downloadLength ->
            if (fileSystemHelper.isDirectory(download.directoryPath)) {
                try {
                    return downloadLength > fileSystemHelper.availableBytesInDirectory(download.directoryPath)
                } catch (e: IllegalArgumentException) {
                    logger.error("Invalid path for StatFs: ${download.directoryPath}", e)
                }
            }
        }
        return false
    }

    @VisibleForTesting
    internal fun showAppDownloaderDialog(
        tab: SessionState,
        download: DownloadState,
        apps: List<DownloaderApp>,
        appChooserDialog: DownloadAppChooserDialog = getAppDownloaderDialog(),
    ) {
        appChooserDialog.setApps(apps)
        appChooserDialog.onAppSelected = { app ->
            onDownloaderAppSelected(app, tab, download)
        }

        appChooserDialog.onDismiss = {
            emitPromptDismissedFact()
            useCases.cancelDownloadRequest.invoke(tab.id, download.id)
        }

        if (!isAlreadyAppDownloaderDialog() && fragmentManager != null && !fragmentManager.isDestroyed) {
            emitPromptDisplayedFact()
            appChooserDialog.showNow(fragmentManager, DownloadAppChooserDialog.FRAGMENT_TAG)
        }
    }

    @VisibleForTesting
    internal fun onDownloaderAppSelected(app: DownloaderApp, tab: SessionState, download: DownloadState) {
        if (app.packageName == applicationContext.packageName) {
            if (applicationContext.isPermissionGranted(downloadManager.permissions.asIterable())) {
                startDownload(download)
                useCases.consumeDownload(tab.id, download.id)
            } else {
                onNeedToRequestPermissions(downloadManager.permissions)
            }
        } else {
            try {
                applicationContext.startActivity(app.toIntent())
            } catch (error: ActivityNotFoundException) {
                val errorMessage = applicationContext.getString(
                    R.string.mozac_feature_downloads_unable_to_open_third_party_app,
                    app.name,
                )
                Toast.makeText(applicationContext, errorMessage, Toast.LENGTH_SHORT).show()
            }
            useCases.consumeDownload(tab.id, download.id)
        }
    }

    private fun getAppDownloaderDialog() = findPreviousAppDownloaderDialogFragment()
        ?: DownloadAppChooserDialog.newInstance(
            promptsStyling?.gravity,
            promptsStyling?.shouldWidthMatchParent,
        )

    @VisibleForTesting
    internal fun isAlreadyAppDownloaderDialog(): Boolean {
        return findPreviousAppDownloaderDialogFragment() != null
    }

    private fun findPreviousAppDownloaderDialogFragment(): DownloadAppChooserDialog? {
        return fragmentManager?.findFragmentByTag(DownloadAppChooserDialog.FRAGMENT_TAG) as? DownloadAppChooserDialog
    }

    @VisibleForTesting(otherwise = PRIVATE)
    internal fun isAlreadyADownloadDialog(): Boolean {
        return findPreviousDownloadDialogFragment() != null
    }

    private fun findPreviousDownloadDialogFragment(): DownloadDialogFragment? {
        return fragmentManager?.findFragmentByTag(FRAGMENT_TAG) as? DownloadDialogFragment
    }

    private fun withActiveDownload(block: (Pair<SessionState, DownloadState>) -> Unit) {
        val state = store.state.findTabOrCustomTabOrSelectedTab(tabId) ?: return
        val download = state.content.download ?: return
        block(Pair(state, download))
    }

    /**
     * Find all apps that can perform a download, including this app.
     */
    @VisibleForTesting
    internal fun getDownloaderApps(context: Context, download: DownloadState): List<DownloaderApp> {
        val packageManager = context.packageManager

        val browsers = Browsers.findResolvers(context, packageManager, includeThisApp = true)
            .associateBy { it.activityInfo.identifier }

        val thisApp = browsers.values
            .firstOrNull { it.activityInfo.packageName == context.packageName }
            ?.toDownloaderApp(context, download)

        // Check for data URL that can cause a TransactionTooLargeException when querying for apps
        // See https://github.com/mozilla-mobile/android-components/issues/9665
        if (download.url.startsWith("data:")) {
            return listOfNotNull(thisApp)
        }

        val apps = Browsers.findResolvers(
            context,
            packageManager,
            includeThisApp = false,
            url = download.url,
            contentType = download.contentType,
        )
        // Remove browsers and returns only the apps that can perform a download plus this app.
        return apps.filter { !browsers.contains(it.activityInfo.identifier) }
            .map { it.toDownloaderApp(context, download) } + listOfNotNull(thisApp)
    }

    @VisibleForTesting
    internal fun dismissAllDownloadDialogs() {
        findPreviousDownloadDialogFragment()?.dismiss()
        findPreviousAppDownloaderDialogFragment()?.dismiss()
    }

    private val ActivityInfo.identifier: String get() = packageName + name

    @VisibleForTesting
    internal fun DownloaderApp.toIntent(): Intent {
        return Intent(Intent.ACTION_VIEW).apply {
            setDataAndTypeAndNormalize(url.toUri(), contentType)
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
            setClassName(packageName, activityName)
            addCategory(Intent.CATEGORY_BROWSABLE)
        }
    }

    /**
     * Styling for the download dialog prompt
     */
    data class PromptsStyling(
        val gravity: Int,
        val shouldWidthMatchParent: Boolean = false,
        @param:ColorRes val positiveButtonBackgroundColor: Int? = null,
        @param:ColorRes val positiveButtonTextColor: Int? = null,
        val positiveButtonRadius: Float? = null,
        val fileNameEndMargin: Int? = null,
    )

    @VisibleForTesting
    internal fun showPermissionDeniedDialog() {
        fragmentManager?.let {
            val dialog = DeniedPermissionDialogFragment.newInstance(
                R.string.mozac_feature_downloads_write_external_storage_permissions_needed_message,
            )
            dialog.showNow(fragmentManager, DeniedPermissionDialogFragment.FRAGMENT_TAG)
        }
    }
}

@VisibleForTesting
internal fun ResolveInfo.toDownloaderApp(context: Context, download: DownloadState): DownloaderApp {
    return DownloaderApp(
        loadLabel(context.packageManager).toString(),
        this,
        activityInfo.packageName,
        activityInfo.name,
        download.url,
        download.contentType,
    )
}
