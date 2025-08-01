/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.browser

import android.app.KeyguardManager
import android.content.Context
import android.content.DialogInterface
import android.content.Intent
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.os.storage.StorageManager
import android.provider.Settings
import android.util.Log
import android.view.Gravity
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityManager
import androidx.activity.result.ActivityResultLauncher
import androidx.annotation.CallSuper
import androidx.annotation.VisibleForTesting
import androidx.appcompat.app.AlertDialog
import androidx.biometric.BiometricManager
import androidx.compose.foundation.layout.Column
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.coordinatorlayout.widget.CoordinatorLayout
import androidx.core.content.getSystemService
import androidx.core.text.HtmlCompat
import androidx.core.view.OnApplyWindowInsetsListener
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.lifecycleScope
import androidx.navigation.NavController
import androidx.navigation.fragment.findNavController
import androidx.preference.PreferenceManager
import com.google.android.material.snackbar.Snackbar.LENGTH_LONG
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers.IO
import kotlinx.coroutines.Dispatchers.Main
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.distinctUntilChangedBy
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.mapNotNull
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import mozilla.appservices.places.BookmarkRoot
import mozilla.appservices.places.uniffi.PlacesApiException
import mozilla.components.browser.menu.view.MenuButton
import mozilla.components.browser.state.selector.findCustomTab
import mozilla.components.browser.state.selector.findCustomTabOrSelectedTab
import mozilla.components.browser.state.selector.findTab
import mozilla.components.browser.state.selector.findTabOrCustomTab
import mozilla.components.browser.state.selector.findTabOrCustomTabOrSelectedTab
import mozilla.components.browser.state.selector.getNormalOrPrivateTabs
import mozilla.components.browser.state.selector.selectedTab
import mozilla.components.browser.state.state.CustomTabSessionState
import mozilla.components.browser.state.state.SessionState
import mozilla.components.browser.state.state.TabSessionState
import mozilla.components.browser.state.state.content.DownloadState
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.browser.thumbnails.BrowserThumbnails
import mozilla.components.browser.toolbar.BrowserToolbar
import mozilla.components.compose.base.Divider
import mozilla.components.compose.browser.toolbar.store.BrowserToolbarStore
import mozilla.components.concept.base.crash.Breadcrumb
import mozilla.components.concept.engine.permission.SitePermissions
import mozilla.components.concept.engine.prompt.ShareData
import mozilla.components.concept.storage.Address
import mozilla.components.concept.storage.CreditCardEntry
import mozilla.components.concept.storage.Login
import mozilla.components.concept.storage.LoginEntry
import mozilla.components.feature.accounts.push.SendTabUseCases
import mozilla.components.feature.app.links.AppLinksFeature
import mozilla.components.feature.contextmenu.ContextMenuCandidate
import mozilla.components.feature.contextmenu.ContextMenuFeature
import mozilla.components.feature.downloads.DownloadsFeature
import mozilla.components.feature.downloads.manager.FetchDownloadManager
import mozilla.components.feature.downloads.temporary.CopyDownloadFeature
import mozilla.components.feature.downloads.temporary.ShareResourceFeature
import mozilla.components.feature.findinpage.view.FindInPageBar
import mozilla.components.feature.intent.ext.EXTRA_SESSION_ID
import mozilla.components.feature.media.fullscreen.MediaSessionFullscreenFeature
import mozilla.components.feature.privatemode.feature.SecureWindowFeature
import mozilla.components.feature.prompts.PromptFeature
import mozilla.components.feature.prompts.PromptFeature.Companion.PIN_REQUEST
import mozilla.components.feature.prompts.address.AddressDelegate
import mozilla.components.feature.prompts.address.AddressSelectBar
import mozilla.components.feature.prompts.concept.AutocompletePrompt
import mozilla.components.feature.prompts.concept.PasswordPromptView
import mozilla.components.feature.prompts.creditcard.CreditCardDelegate
import mozilla.components.feature.prompts.creditcard.CreditCardSelectBar
import mozilla.components.feature.prompts.dialog.FullScreenNotificationToast
import mozilla.components.feature.prompts.dialog.GestureNavUtils
import mozilla.components.feature.prompts.file.AndroidPhotoPicker
import mozilla.components.feature.prompts.identitycredential.DialogColors
import mozilla.components.feature.prompts.identitycredential.DialogColorsProvider
import mozilla.components.feature.prompts.login.LoginDelegate
import mozilla.components.feature.prompts.login.LoginSelectBar
import mozilla.components.feature.prompts.login.PasswordGeneratorDialogColors
import mozilla.components.feature.prompts.login.PasswordGeneratorDialogColorsProvider
import mozilla.components.feature.prompts.login.SuggestStrongPasswordBar
import mozilla.components.feature.prompts.login.SuggestStrongPasswordDelegate
import mozilla.components.feature.prompts.share.ShareDelegate
import mozilla.components.feature.readerview.ReaderViewFeature
import mozilla.components.feature.search.SearchFeature
import mozilla.components.feature.session.FullScreenFeature
import mozilla.components.feature.session.PictureInPictureFeature
import mozilla.components.feature.session.ScreenOrientationFeature
import mozilla.components.feature.session.SessionFeature
import mozilla.components.feature.session.SwipeRefreshFeature
import mozilla.components.feature.sitepermissions.SitePermissionsFeature
import mozilla.components.feature.tabs.LastTabFeature
import mozilla.components.feature.webauthn.WebAuthnFeature
import mozilla.components.lib.state.ext.consumeFlow
import mozilla.components.lib.state.ext.consumeFrom
import mozilla.components.lib.state.ext.flowScoped
import mozilla.components.service.sync.autofill.DefaultCreditCardValidationDelegate
import mozilla.components.service.sync.logins.DefaultLoginValidationDelegate
import mozilla.components.service.sync.logins.LoginsApiException
import mozilla.components.service.sync.logins.SyncableLoginsStorage
import mozilla.components.support.base.feature.ActivityResultHandler
import mozilla.components.support.base.feature.PermissionsFeature
import mozilla.components.support.base.feature.UserInteractionHandler
import mozilla.components.support.base.feature.ViewBoundFeatureWrapper
import mozilla.components.support.ktx.android.view.ImeInsetsSynchronizer
import mozilla.components.support.ktx.android.view.enterImmersiveMode
import mozilla.components.support.ktx.android.view.exitImmersiveMode
import mozilla.components.support.ktx.android.view.hideKeyboard
import mozilla.components.support.ktx.kotlin.getOrigin
import mozilla.components.support.ktx.kotlinx.coroutines.flow.ifAnyChanged
import mozilla.components.support.locale.ActivityContextWrapper
import mozilla.components.ui.widgets.behavior.EngineViewClippingBehavior
import mozilla.components.ui.widgets.withCenterAlignedButtons
import mozilla.telemetry.glean.private.NoExtras
import org.mozilla.fenix.BuildConfig
import org.mozilla.fenix.FeatureFlags
import org.mozilla.fenix.GleanMetrics.Events
import org.mozilla.fenix.GleanMetrics.MediaState
import org.mozilla.fenix.GleanMetrics.PullToRefreshInBrowser
import org.mozilla.fenix.HomeActivity
import org.mozilla.fenix.IntentReceiverActivity
import org.mozilla.fenix.NavGraphDirections
import org.mozilla.fenix.OnLongPressedListener
import org.mozilla.fenix.OpenInFirefoxBinding
import org.mozilla.fenix.R
import org.mozilla.fenix.ReaderViewBinding
import org.mozilla.fenix.bindings.FindInPageBinding
import org.mozilla.fenix.biometricauthentication.AuthenticationStatus
import org.mozilla.fenix.biometricauthentication.BiometricAuthenticationManager
import org.mozilla.fenix.biometricauthentication.NavigationOrigin
import org.mozilla.fenix.browser.browsingmode.BrowsingMode
import org.mozilla.fenix.browser.readermode.DefaultReaderModeController
import org.mozilla.fenix.browser.readermode.ReaderModeController
import org.mozilla.fenix.browser.store.BrowserScreenStore
import org.mozilla.fenix.browser.tabstrip.TabStrip
import org.mozilla.fenix.components.Components
import org.mozilla.fenix.components.FenixAutocompletePrompt
import org.mozilla.fenix.components.FenixSuggestStrongPasswordPrompt
import org.mozilla.fenix.components.FindInPageIntegration
import org.mozilla.fenix.components.accounts.FxaWebChannelIntegration
import org.mozilla.fenix.components.appstate.AppAction
import org.mozilla.fenix.components.appstate.AppAction.MessagingAction
import org.mozilla.fenix.components.appstate.AppAction.MessagingAction.MicrosurveyAction
import org.mozilla.fenix.components.metrics.MetricsUtils
import org.mozilla.fenix.components.toolbar.BottomToolbarContainerIntegration
import org.mozilla.fenix.components.toolbar.BottomToolbarContainerView
import org.mozilla.fenix.components.toolbar.BrowserNavigationBar
import org.mozilla.fenix.components.toolbar.BrowserToolbarComposable
import org.mozilla.fenix.components.toolbar.BrowserToolbarMenuController
import org.mozilla.fenix.components.toolbar.BrowserToolbarView
import org.mozilla.fenix.components.toolbar.DefaultBrowserToolbarController
import org.mozilla.fenix.components.toolbar.DefaultBrowserToolbarMenuController
import org.mozilla.fenix.components.toolbar.FenixBrowserToolbarView
import org.mozilla.fenix.components.toolbar.ToolbarContainerView
import org.mozilla.fenix.components.toolbar.ToolbarIntegration
import org.mozilla.fenix.components.toolbar.ToolbarPosition
import org.mozilla.fenix.components.toolbar.interactor.BrowserToolbarInteractor
import org.mozilla.fenix.components.toolbar.interactor.DefaultBrowserToolbarInteractor
import org.mozilla.fenix.compose.core.Action
import org.mozilla.fenix.compose.snackbar.DefaultSnackbarFactory
import org.mozilla.fenix.compose.snackbar.Snackbar
import org.mozilla.fenix.compose.snackbar.SnackbarState
import org.mozilla.fenix.crashes.CrashContentIntegration
import org.mozilla.fenix.crashes.CrashContentView
import org.mozilla.fenix.customtabs.ExternalAppBrowserActivity
import org.mozilla.fenix.databinding.FragmentBrowserBinding
import org.mozilla.fenix.downloads.DownloadService
import org.mozilla.fenix.downloads.dialog.createDownloadAppDialog
import org.mozilla.fenix.ext.accessibilityManager
import org.mozilla.fenix.ext.breadcrumb
import org.mozilla.fenix.ext.components
import org.mozilla.fenix.ext.getPreferenceKey
import org.mozilla.fenix.ext.hideToolbar
import org.mozilla.fenix.ext.isToolbarAtBottom
import org.mozilla.fenix.ext.nav
import org.mozilla.fenix.ext.navigateWithBreadcrumb
import org.mozilla.fenix.ext.pixelSizeFor
import org.mozilla.fenix.ext.registerForActivityResult
import org.mozilla.fenix.ext.requireComponents
import org.mozilla.fenix.ext.runIfFragmentIsAttached
import org.mozilla.fenix.ext.secure
import org.mozilla.fenix.ext.settings
import org.mozilla.fenix.ext.tabClosedUndoMessage
import org.mozilla.fenix.ext.updateMicrosurveyPromptForConfigurationChange
import org.mozilla.fenix.home.HomeScreenViewModel
import org.mozilla.fenix.library.bookmarks.friendlyRootTitle
import org.mozilla.fenix.lifecycle.observePrivateModeLock
import org.mozilla.fenix.messaging.FenixMessageSurfaceId
import org.mozilla.fenix.messaging.MessagingFeature
import org.mozilla.fenix.microsurvey.ui.MicrosurveyRequestPrompt
import org.mozilla.fenix.microsurvey.ui.ext.MicrosurveyUIData
import org.mozilla.fenix.microsurvey.ui.ext.toMicrosurveyUIData
import org.mozilla.fenix.perf.MarkersFragmentLifecycleCallbacks
import org.mozilla.fenix.search.awesomebar.AwesomeBarComposable
import org.mozilla.fenix.settings.SupportUtils
import org.mozilla.fenix.settings.biometric.BiometricPromptFeature
import org.mozilla.fenix.snackbar.FenixSnackbarDelegate
import org.mozilla.fenix.snackbar.SnackbarBinding
import org.mozilla.fenix.tabstray.DefaultTabManagementFeatureHelper
import org.mozilla.fenix.tabstray.Page
import org.mozilla.fenix.tabstray.ext.toDisplayTitle
import org.mozilla.fenix.theme.FirefoxTheme
import org.mozilla.fenix.theme.ThemeManager
import org.mozilla.fenix.utils.allowUndo
import org.mozilla.fenix.wifi.SitePermissionsWifiIntegration
import java.lang.ref.WeakReference
import kotlin.coroutines.cancellation.CancellationException

/**
 * Base fragment extended by [BrowserFragment].
 * This class only contains shared code focused on the main browsing content.
 * UI code specific to the app or to custom tabs can be found in the subclasses.
 */
@Suppress("TooManyFunctions", "LargeClass")
abstract class BaseBrowserFragment :
    Fragment(),
    UserInteractionHandler,
    ActivityResultHandler,
    OnLongPressedListener,
    AccessibilityManager.AccessibilityStateChangeListener {

    private var _binding: FragmentBrowserBinding? = null
    internal val binding get() = _binding!!

    private var loginSelectBar: AutocompletePrompt<Login>? = null
    private var addressSelectBar: AutocompletePrompt<Address>? = null
    private var creditCardSelectBar: AutocompletePrompt<CreditCardEntry>? = null
    private var suggestStrongPasswordBar: PasswordPromptView? = null

    private lateinit var browserAnimator: BrowserAnimator
    private lateinit var startForResult: ActivityResultLauncher<Intent>

    private var _browserToolbarInteractor: BrowserToolbarInteractor? = null

    @VisibleForTesting(otherwise = VisibleForTesting.PROTECTED)
    internal val browserToolbarInteractor: BrowserToolbarInteractor
        get() = _browserToolbarInteractor!!

    @VisibleForTesting
    @Suppress("VariableNaming")
    internal var _browserToolbarView: FenixBrowserToolbarView? = null
    private var awesomeBarComposable: AwesomeBarComposable? = null

    @VisibleForTesting
    internal val browserToolbarView: FenixBrowserToolbarView
        get() = _browserToolbarView!!

    @VisibleForTesting(otherwise = VisibleForTesting.PROTECTED)
    internal var browserNavigationBar: BrowserNavigationBar? = null

    @VisibleForTesting(otherwise = VisibleForTesting.PROTECTED)
    @Suppress("VariableNaming")
    internal var _bottomToolbarContainerView: BottomToolbarContainerView? = null
    protected val bottomToolbarContainerView: BottomToolbarContainerView
        get() = _bottomToolbarContainerView!!

    private var _findInPageLauncher: (() -> Unit)? = null
    private val findInPageLauncher: () -> Unit
        get() = _findInPageLauncher!!

    private var _browserToolbarMenuController: BrowserToolbarMenuController? = null
    private val browserToolbarMenuController: BrowserToolbarMenuController
        get() = _browserToolbarMenuController!!

    @Suppress("VariableNaming")
    @VisibleForTesting
    internal var _menuButtonView: MenuButton? = null

    protected val readerViewFeature = ViewBoundFeatureWrapper<ReaderViewFeature>()
    protected val thumbnailsFeature = ViewBoundFeatureWrapper<BrowserThumbnails>()

    @VisibleForTesting
    internal val messagingFeatureMicrosurvey = ViewBoundFeatureWrapper<MessagingFeature>()

    private val sessionFeature = ViewBoundFeatureWrapper<SessionFeature>()
    private val lastTabFeature = ViewBoundFeatureWrapper<LastTabFeature>()
    private val contextMenuFeature = ViewBoundFeatureWrapper<ContextMenuFeature>()
    private val downloadsFeature = ViewBoundFeatureWrapper<DownloadsFeature>()
    private val appLinksFeature = ViewBoundFeatureWrapper<AppLinksFeature>()
    private val shareResourceFeature = ViewBoundFeatureWrapper<ShareResourceFeature>()
    private val copyDownloadsFeature = ViewBoundFeatureWrapper<CopyDownloadFeature>()
    private val promptsFeature = ViewBoundFeatureWrapper<PromptFeature>()

    @VisibleForTesting
    internal val findInPageIntegration = ViewBoundFeatureWrapper<FindInPageIntegration>()
    private val toolbarIntegration = ViewBoundFeatureWrapper<ToolbarIntegration>()
    private val bottomToolbarContainerIntegration = ViewBoundFeatureWrapper<BottomToolbarContainerIntegration>()
    private val sitePermissionsFeature = ViewBoundFeatureWrapper<SitePermissionsFeature>()
    private val fullScreenFeature = ViewBoundFeatureWrapper<FullScreenFeature>()
    private val swipeRefreshFeature = ViewBoundFeatureWrapper<SwipeRefreshFeature>()
    private val webchannelIntegration = ViewBoundFeatureWrapper<FxaWebChannelIntegration>()
    private val sitePermissionWifiIntegration =
        ViewBoundFeatureWrapper<SitePermissionsWifiIntegration>()
    private val secureWindowFeature = ViewBoundFeatureWrapper<SecureWindowFeature>()
    private var fullScreenMediaSessionFeature =
        ViewBoundFeatureWrapper<MediaSessionFullscreenFeature>()
    private val searchFeature = ViewBoundFeatureWrapper<SearchFeature>()
    private val webAuthnFeature = ViewBoundFeatureWrapper<WebAuthnFeature>()
    private val screenOrientationFeature = ViewBoundFeatureWrapper<ScreenOrientationFeature>()
    private val biometricPromptFeature = ViewBoundFeatureWrapper<BiometricPromptFeature>()
    private val crashContentIntegration = ViewBoundFeatureWrapper<CrashContentIntegration>()
    private val readerViewBinding = ViewBoundFeatureWrapper<ReaderViewBinding>()
    private val openInFirefoxBinding = ViewBoundFeatureWrapper<OpenInFirefoxBinding>()
    private val findInPageBinding = ViewBoundFeatureWrapper<FindInPageBinding>()
    private val snackbarBinding = ViewBoundFeatureWrapper<SnackbarBinding>()
    private val standardSnackbarErrorBinding = ViewBoundFeatureWrapper<StandardSnackbarErrorBinding>()

    private var pipFeature: PictureInPictureFeature? = null

    var customTabSessionId: String? = null

    @VisibleForTesting
    internal var browserInitialized: Boolean = false

    @VisibleForTesting(otherwise = VisibleForTesting.PROTECTED)
    internal var webAppToolbarShouldBeVisible = true

    protected lateinit var browserScreenStore: BrowserScreenStore
    private val homeViewModel: HomeScreenViewModel by activityViewModels()

    private var downloadDialog: AlertDialog? = null

    private var lastSavedGeneratedPassword: String? = null

    // Registers a photo picker activity launcher in single-select mode.
    private val singleMediaPicker =
        AndroidPhotoPicker.singleMediaPicker(
            { getFragment() },
            { getPromptsFeature() },
        )

    // Registers a photo picker activity launcher in multi-select mode.
    private val multipleMediaPicker =
        AndroidPhotoPicker.multipleMediaPicker(
            { getFragment() },
            { getPromptsFeature() },
        )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
    }

    private fun getFragment(): Fragment {
        return this
    }

    private fun getPromptsFeature(): PromptFeature? {
        return promptsFeature.get()
    }

    @CallSuper
    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View {
        // DO NOT ADD ANYTHING ABOVE THIS getProfilerTime CALL!
        val profilerStartTime = requireComponents.core.engine.profiler?.getProfilerTime()

        customTabSessionId = requireArguments().getString(EXTRA_SESSION_ID)

        // Diagnostic breadcrumb for "Display already aquired" crash:
        // https://github.com/mozilla-mobile/android-components/issues/7960
        breadcrumb(
            message = "onCreateView()",
            data = mapOf(
                "customTabSessionId" to customTabSessionId.toString(),
            ),
        )

        _binding = FragmentBrowserBinding.inflate(inflater, container, false)

        val activity = activity as HomeActivity
        val originalContext = ActivityContextWrapper.getOriginalContext(activity)
        binding.engineView.setActivityContext(originalContext)

        startForResult = registerForActivityResult { result ->
            listOf(
                promptsFeature,
                webAuthnFeature,
            ).any {
                it.onActivityResult(PIN_REQUEST, result.data, result.resultCode)
            }
        }

        // DO NOT MOVE ANYTHING BELOW THIS addMarker CALL!
        requireComponents.core.engine.profiler?.addMarker(
            MarkersFragmentLifecycleCallbacks.MARKER_NAME,
            profilerStartTime,
            "BaseBrowserFragment.onCreateView",
        )
        return binding.root
    }

    final override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        // DO NOT ADD ANYTHING ABOVE THIS getProfilerTime CALL!
        val profilerStartTime = requireComponents.core.engine.profiler?.getProfilerTime()

        initializeUI(view)

        if (customTabSessionId == null) {
            // We currently only need this observer to navigate to home
            // in case all tabs have been removed on startup. No need to
            // this if we have a known session to display.
            observeRestoreComplete(requireComponents.core.store, findNavController())
        }

        observeTabSelection(
            requireComponents.core.store,
            isCustomTabSession = customTabSessionId != null,
        )

        observePrivateModeLock(
            viewLifecycleOwner = viewLifecycleOwner,
            scope = viewLifecycleOwner.lifecycleScope,
            appStore = requireComponents.appStore,
            onPrivateModeLocked = {
                if (customTabSessionId == null || requireContext().settings().openLinksInAPrivateTab) {
                    findNavController().navigate(
                        NavGraphDirections.actionGlobalUnlockPrivateTabsFragment(
                            if (customTabSessionId != null) NavigationOrigin.CUSTOM_TAB else NavigationOrigin.TAB,
                        ),
                    )
                }
            },
        )

        if (!requireComponents.fenixOnboarding.userHasBeenOnboarded()) {
            observeTabSource(requireComponents.core.store)
        }

        requireContext().accessibilityManager.addAccessibilityStateChangeListener(this)

        requireComponents.backgroundServices.closeSyncedTabsCommandReceiver.register(
            observer = CloseLastSyncedTabObserver(
                scope = viewLifecycleOwner.lifecycleScope,
                navController = findNavController(),
            ),
            view = view,
        )

        // DO NOT MOVE ANYTHING BELOW THIS addMarker CALL!
        requireComponents.core.engine.profiler?.addMarker(
            MarkersFragmentLifecycleCallbacks.MARKER_NAME,
            profilerStartTime,
            "BaseBrowserFragment.onViewCreated",
        )
    }

    private fun initializeUI(view: View) {
        val tab = getCurrentTab()
        browserInitialized = if (tab != null) {
            initializeUI(view, tab)
            setupIMEInsetsHandling(view)
            true
        } else {
            false
        }
    }

    @Suppress("ComplexMethod", "LongMethod", "DEPRECATION")
    // https://github.com/mozilla-mobile/fenix/issues/19920
    @CallSuper
    internal open fun initializeUI(view: View, tab: SessionState) {
        val context = requireContext()
        val store = context.components.core.store
        val activity = requireActivity() as HomeActivity

        browserAnimator = BrowserAnimator(
            fragment = WeakReference(this),
            engineView = WeakReference(binding.engineView),
            swipeRefresh = WeakReference(binding.swipeRefresh),
            viewLifecycleScope = WeakReference(viewLifecycleOwner.lifecycleScope),
        ).apply {
            beginAnimateInIfNecessary()
        }

        val openInFenixIntent = Intent(context, IntentReceiverActivity::class.java).apply {
            action = Intent.ACTION_VIEW
            putExtra(HomeActivity.OPEN_TO_BROWSER, true)
        }

        val readerMenuController = DefaultReaderModeController(
            readerViewFeature,
            binding.readerViewControlsBar,
            isPrivate = activity.browsingModeManager.mode.isPrivate,
            onReaderModeChanged = { activity.finishActionMode() },
        )
        val browserToolbarController = DefaultBrowserToolbarController(
            store = store,
            appStore = context.components.appStore,
            tabsUseCases = requireComponents.useCases.tabsUseCases,
            fenixBrowserUseCases = requireComponents.useCases.fenixBrowserUseCases,
            activity = activity,
            settings = context.settings(),
            navController = findNavController(),
            readerModeController = readerMenuController,
            engineView = binding.engineView,
            homeViewModel = homeViewModel,
            customTabSessionId = customTabSessionId,
            browserAnimator = browserAnimator,
            onTabCounterClicked = {
                onTabCounterClicked(activity.browsingModeManager.mode)
            },
            onCloseTab = { closedSession ->
                val closedTab = store.state.findTab(closedSession.id) ?: return@DefaultBrowserToolbarController
                showUndoSnackbar(context.tabClosedUndoMessage(closedTab.content.private))
            },
        )

        _findInPageLauncher = {
            launchFindInPageFeature(view, store)
        }
        _browserToolbarMenuController = DefaultBrowserToolbarMenuController(
            fragment = this,
            store = store,
            appStore = requireComponents.appStore,
            activity = activity,
            navController = findNavController(),
            settings = context.settings(),
            readerModeController = readerMenuController,
            sessionFeature = sessionFeature,
            findInPageLauncher = findInPageLauncher,
            browserAnimator = browserAnimator,
            customTabSessionId = customTabSessionId,
            openInFenixIntent = openInFenixIntent,
            bookmarkTapped = { url: String, title: String ->
                viewLifecycleOwner.lifecycleScope.launch {
                    bookmarkTapped(url, title)
                }
            },
            scope = viewLifecycleOwner.lifecycleScope,
            tabCollectionStorage = requireComponents.core.tabCollectionStorage,
            topSitesStorage = requireComponents.core.topSitesStorage,
            pinnedSiteStorage = requireComponents.core.pinnedSiteStorage,
        )

        _browserToolbarInteractor = DefaultBrowserToolbarInteractor(
            browserToolbarController,
            browserToolbarMenuController,
        )

        _browserToolbarView = initializeBrowserToolbar(activity, store, readerMenuController)

        if (context.settings().microsurveyFeatureEnabled) {
            listenForMicrosurveyMessage(context)
        }

        (browserToolbarView as? BrowserToolbarView)?.toolbarIntegration?.let {
            toolbarIntegration.set(
                feature = it,
                owner = this,
                view = view,
            )
        }

        findInPageBinding.set(
            feature = FindInPageBinding(
                appStore = context.components.appStore,
                onFindInPageLaunch = findInPageLauncher,
            ),
            owner = this,
            view = view,
        )

        readerViewBinding.set(
            feature = ReaderViewBinding(
                appStore = context.components.appStore,
                readerMenuController = readerMenuController,
            ),
            owner = this,
            view = view,
        )

        openInFirefoxBinding.set(
            feature = OpenInFirefoxBinding(
                activity = activity,
                appStore = context.components.appStore,
                customTabSessionId = customTabSessionId,
                customTabsUseCases = context.components.useCases.customTabsUseCases,
                openInFenixIntent = openInFenixIntent,
                sessionFeature = sessionFeature,
            ),
            owner = this,
            view = view,
        )

        (browserToolbarView as? BrowserToolbarView)?.toolbar?.display?.setOnSiteInfoClickedListener {
            showQuickSettingsDialog()
            Events.browserToolbarSecurityIndicatorTapped.record()
        }

        contextMenuFeature.set(
            feature = ContextMenuFeature(
                fragmentManager = parentFragmentManager,
                store = store,
                candidates = getContextMenuCandidates(context, binding.dynamicSnackbarContainer),
                engineView = binding.engineView,
                useCases = context.components.useCases.contextMenuUseCases,
                tabId = customTabSessionId,
            ),
            owner = this,
            view = view,
        )

        snackbarBinding.set(
            feature = SnackbarBinding(
                context = context,
                browserStore = context.components.core.store,
                appStore = context.components.appStore,
                snackbarDelegate = FenixSnackbarDelegate(binding.dynamicSnackbarContainer),
                navController = findNavController(),
                tabsUseCases = context.components.useCases.tabsUseCases,
                sendTabUseCases = SendTabUseCases(requireComponents.backgroundServices.accountManager),
                customTabSessionId = customTabSessionId,
            ),
            owner = this,
            view = view,
        )

        standardSnackbarErrorBinding.set(
            feature = StandardSnackbarErrorBinding(
                snackbarParent = binding.dynamicSnackbarContainer,
                appStore = requireActivity().components.appStore,
                snackbarFactory = DefaultSnackbarFactory(),
                dismissLabel = getString(R.string.standard_snackbar_error_dismiss),
            ),
            owner = viewLifecycleOwner,
            view = binding.root,
        )

        val allowScreenshotsInPrivateMode = context.settings().allowScreenshotsInPrivateMode
        secureWindowFeature.set(
            feature = SecureWindowFeature(
                window = requireActivity().window,
                store = store,
                customTabId = customTabSessionId,
                isSecure = { !allowScreenshotsInPrivateMode && it.content.private },
                clearFlagOnStop = false,
            ),
            owner = this,
            view = view,
        )

        fullScreenMediaSessionFeature.set(
            feature = MediaSessionFullscreenFeature(
                requireActivity(),
                context.components.core.store,
                customTabSessionId,
            ),
            owner = this,
            view = view,
        )

        val shareResourceFeature = ShareResourceFeature(
            context = context.applicationContext,
            httpClient = context.components.core.client,
            store = store,
            tabId = customTabSessionId,
        )

        val copyDownloadFeature = CopyDownloadFeature(
            context = context.applicationContext,
            httpClient = context.components.core.client,
            store = store,
            tabId = customTabSessionId,
            onCopyConfirmation = {
                showSnackbarForClipboardCopy()
            },
        )

        val downloadFeature = DownloadsFeature(
            context.applicationContext,
            store = store,
            useCases = context.components.useCases.downloadUseCases,
            fragmentManager = childFragmentManager,
            tabId = customTabSessionId,
            downloadManager = FetchDownloadManager(
                context.applicationContext,
                store,
                DownloadService::class,
                notificationsDelegate = context.components.notificationsDelegate,
            ),
            shouldForwardToThirdParties = {
                PreferenceManager.getDefaultSharedPreferences(context).getBoolean(
                    context.getPreferenceKey(R.string.pref_key_external_download_manager),
                    false,
                )
            },
            promptsStyling = DownloadsFeature.PromptsStyling(
                gravity = Gravity.BOTTOM,
                shouldWidthMatchParent = true,
                positiveButtonBackgroundColor = ThemeManager.resolveAttribute(
                    R.attr.accent,
                    context,
                ),
                positiveButtonTextColor = ThemeManager.resolveAttribute(
                    R.attr.textOnColorPrimary,
                    context,
                ),
                positiveButtonRadius = pixelSizeFor(R.dimen.tab_corner_radius).toFloat(),
            ),
            onDownloadStartedListener = { downloadId ->
                context.components.appStore.dispatch(
                    AppAction.DownloadAction.DownloadInProgress(
                        downloadId,
                    ),
                )
            },
            onNeedToRequestPermissions = { permissions ->
                requestPermissions(permissions, REQUEST_CODE_DOWNLOAD_PERMISSIONS)
            },
            customFirstPartyDownloadDialog = { filename, contentSize, positiveAction, negativeAction ->
                run {
                    if (downloadDialog == null) {
                        requireContext().components.analytics.crashReporter.recordCrashBreadcrumb(
                            Breadcrumb("FirstPartyDownloadDialog created"),
                        )

                        val title = if (contentSize.value > 0L) {
                            val contentSizeInBytes =
                                requireComponents.core.fileSizeFormatter.formatSizeInBytes(
                                    contentSize.value,
                                )
                            getString(
                                R.string.mozac_feature_downloads_dialog_title_3,
                                contentSizeInBytes,
                            )
                        } else {
                            getString(R.string.mozac_feature_downloads_dialog_title_with_unknown_size)
                        }

                        downloadDialog = AlertDialog.Builder(requireContext())
                            .setTitle(title)
                            .setMessage(filename.value)
                            .setPositiveButton(R.string.mozac_feature_downloads_dialog_download) { dialog, _ ->
                                positiveAction.value.invoke()
                                dialog.dismiss()
                            }
                            .setNegativeButton(R.string.mozac_feature_downloads_dialog_cancel) { dialog, _ ->
                                negativeAction.value.invoke()
                                dialog.dismiss()
                            }.setOnDismissListener {
                                downloadDialog = null
                                context.components.analytics.crashReporter.recordCrashBreadcrumb(
                                    Breadcrumb("FirstPartyDownloadDialog onDismiss"),
                                )
                            }.show()
                    }
                }
            },
            customThirdPartyDownloadDialog = { downloaderApps, onAppSelected, negativeActionCallback ->
                run {
                    if (downloadDialog == null) {
                        context.components.analytics.crashReporter.recordCrashBreadcrumb(
                            Breadcrumb("DownloaderAppDialog created"),
                        )
                        downloadDialog = createDownloadAppDialog(
                            context = context,
                            downloaderApps = downloaderApps.value,
                            onAppSelected = onAppSelected.value,
                            onDismiss = {
                                downloadDialog = null
                                context.components.analytics.crashReporter.recordCrashBreadcrumb(
                                    Breadcrumb("DownloaderAppDialog onDismiss"),
                                )
                            },
                        )
                        downloadDialog?.show()
                    }
                }
            },
            fileHasNotEnoughStorageDialog = { filename ->
                AlertDialog.Builder(requireContext())
                    .setTitle(R.string.download_file_has_not_enough_storage_dialog_title)
                    .setMessage(
                        HtmlCompat.fromHtml(
                            getString(
                                R.string.download_file_has_not_enough_storage_dialog_message,
                                filename.value,
                            ),
                            HtmlCompat.FROM_HTML_MODE_COMPACT,
                        ),
                    )
                    .setPositiveButton(
                        R.string.download_file_has_not_enough_storage_dialog_confirm_button_text,
                    ) { dialog, _ ->
                        openManageStorageSettings()
                        dialog.dismiss()
                    }
                    .setNegativeButton(
                        R.string.download_file_has_not_enough_storage_dialog_cancel_button_text,
                    ) { dialog, _ ->
                        dialog.dismiss()
                    }.show()
            },
        )

        val passwordGeneratorColorsProvider = PasswordGeneratorDialogColorsProvider {
            PasswordGeneratorDialogColors(
                title = ThemeManager.resolveAttributeColor(attribute = R.attr.textPrimary),
                description = ThemeManager.resolveAttributeColor(attribute = R.attr.textSecondary),
                background = ThemeManager.resolveAttributeColor(attribute = R.attr.layer1),
                cancelText = ThemeManager.resolveAttributeColor(attribute = R.attr.textAccent),
                confirmButton = ThemeManager.resolveAttributeColor(attribute = R.attr.actionPrimary),
                passwordBox = ThemeManager.resolveAttributeColor(attribute = R.attr.layer2),
                boxBorder = ThemeManager.resolveAttributeColor(attribute = R.attr.textDisabled),
            )
        }

        val bottomToolbarHeight = context.settings().getBottomToolbarHeight()

        downloadFeature.onDownloadStopped = { downloadState, _, downloadJobStatus ->
            handleOnDownloadFinished(
                appStore = requireComponents.appStore,
                downloadState = downloadState,
                downloadJobStatus = downloadJobStatus,
                browserToolbars = listOfNotNull(
                    browserToolbarView,
                    _bottomToolbarContainerView?.toolbarContainerView,
                ),
            )
        }

        this.shareResourceFeature.set(
            shareResourceFeature,
            owner = this,
            view = view,
        )

        copyDownloadsFeature.set(
            copyDownloadFeature,
            owner = this,
            view = view,
        )

        downloadsFeature.set(
            downloadFeature,
            owner = this,
            view = view,
        )

        pipFeature = PictureInPictureFeature(
            store = store,
            activity = requireActivity(),
            crashReporting = context.components.analytics.crashReporter,
            tabId = customTabSessionId,
        )

        biometricPromptFeature.set(
            feature = BiometricPromptFeature(
                context = context,
                fragment = this,
                onAuthFailure = {
                    promptsFeature.get()?.onBiometricResult(isAuthenticated = false)
                },
                onAuthSuccess = {
                    promptsFeature.get()?.onBiometricResult(isAuthenticated = true)
                },
            ),
            owner = this,
            view = view,
        )

        val colorsProvider = DialogColorsProvider {
            DialogColors(
                title = ThemeManager.resolveAttributeColor(attribute = R.attr.textPrimary),
                description = ThemeManager.resolveAttributeColor(attribute = R.attr.textSecondary),
            )
        }

        loginSelectBar = FenixAutocompletePrompt(
            viewProvider = {
                view.findViewById(R.id.loginSelectBar) ?: (binding.loginSelectBarStub.inflate() as LoginSelectBar)
            },
            toolbarPositionProvider = {
                requireContext().settings().toolbarPosition
            },
            onShow = ::onAutocompleteBarShow,
            onHide = ::onAutocompleteBarHide,
        )

        addressSelectBar = FenixAutocompletePrompt(
            viewProvider = {
                view.findViewById(R.id.addressSelectBar)
                    ?: binding.addressSelectBarStub.inflate() as AddressSelectBar
            },
            toolbarPositionProvider = {
                requireContext().settings().toolbarPosition
            },
            onShow = ::onAutocompleteBarShow,
            onHide = ::onAutocompleteBarHide,
        )

        creditCardSelectBar = FenixAutocompletePrompt(
            viewProvider = {
                view.findViewById(R.id.creditCardSelectBar)
                    ?: binding.creditCardSelectBarStub.inflate() as CreditCardSelectBar
            },
            toolbarPositionProvider = {
                requireContext().settings().toolbarPosition
            },
            onShow = ::onAutocompleteBarShow,
            onHide = ::onAutocompleteBarHide,
        )

        suggestStrongPasswordBar = FenixSuggestStrongPasswordPrompt(
            viewProvider = {
                view.findViewById(R.id.suggestStrongPasswordBar)
                    ?: binding.suggestStrongPasswordBarStub.inflate() as SuggestStrongPasswordBar
            },
            toolbarPositionProvider = {
                requireContext().settings().toolbarPosition
            },
            onShow = ::onAutocompleteBarShow,
            onHide = ::onAutocompleteBarHide,
        )

        appLinksFeature.set(
            feature = AppLinksFeature(
                context = requireContext(),
                store = store,
                sessionId = tab.id,
                fragmentManager = parentFragmentManager,
                launchInApp = { context.settings().shouldOpenLinksInApp(customTabSessionId != null) },
                loadUrlUseCase = requireComponents.useCases.sessionUseCases.loadUrl,
                shouldPrompt = { context.settings().shouldPromptOpenLinksInApp() },
                alwaysOpenCheckboxAction = {
                    context.settings().openLinksInExternalApp =
                        context.getString(R.string.pref_key_open_links_in_apps_always)
                },
                failedToLaunchAction = { fallbackUrl ->
                    fallbackUrl?.let {
                        val appLinksUseCases = requireComponents.useCases.appLinksUseCases
                        val getRedirect = appLinksUseCases.appLinkRedirect
                        val redirect = getRedirect.invoke(fallbackUrl)
                        redirect.appIntent?.flags = Intent.FLAG_ACTIVITY_NEW_TASK
                        appLinksUseCases.openAppLink.invoke(redirect.appIntent)
                    }
                },
            ),
            owner = this,
            view = binding.root,
        )

        promptsFeature.set(
            feature = PromptFeature(
                activity = activity,
                store = store,
                customTabId = customTabSessionId,
                fragmentManager = parentFragmentManager,
                identityCredentialColorsProvider = colorsProvider,
                tabsUseCases = requireComponents.useCases.tabsUseCases,
                fileUploadsDirCleaner = requireComponents.core.fileUploadsDirCleaner,
                creditCardValidationDelegate = DefaultCreditCardValidationDelegate(
                    context.components.core.lazyAutofillStorage,
                ),
                loginValidationDelegate = DefaultLoginValidationDelegate(
                    context.components.core.lazyPasswordsStorage,
                ),
                isLoginAutofillEnabled = {
                    context.settings().shouldAutofillLogins
                },
                isSaveLoginEnabled = {
                    context.settings().shouldPromptToSaveLogins
                },
                isCreditCardAutofillEnabled = {
                    context.settings().shouldAutofillCreditCardDetails
                },
                isAddressAutofillEnabled = {
                    context.settings().addressFeature && context.settings().shouldAutofillAddressDetails
                },
                loginExceptionStorage = context.components.core.loginExceptionStorage,
                shareDelegate = object : ShareDelegate {
                    override fun showShareSheet(
                        context: Context,
                        shareData: ShareData,
                        onDismiss: () -> Unit,
                        onSuccess: () -> Unit,
                    ) {
                        val directions = NavGraphDirections.actionGlobalShareFragment(
                            data = arrayOf(shareData),
                            showPage = true,
                            sessionId = getCurrentTab()?.id,
                        )
                        findNavController().navigate(directions)
                    }
                },
                onNeedToRequestPermissions = { permissions ->
                    requestPermissions(permissions, REQUEST_CODE_PROMPT_PERMISSIONS)
                },
                loginDelegate = object : LoginDelegate {
                    override val loginPickerView
                        get() = loginSelectBar
                    override val onManageLogins = {
                        browserAnimator.captureEngineViewAndDrawStatically {
                            val directions =
                                NavGraphDirections.actionGlobalSavedLoginsAuthFragment()
                            findNavController().navigate(directions)
                        }
                    }
                },
                suggestStrongPasswordDelegate = object : SuggestStrongPasswordDelegate {
                    override val strongPasswordPromptViewListenerView
                        get() = suggestStrongPasswordBar
                },
                shouldAutomaticallyShowSuggestedPassword = { context.settings().isFirstTimeEngagingWithSignup },
                onFirstTimeEngagedWithSignup = {
                    context.settings().isFirstTimeEngagingWithSignup = false
                },
                onSaveLoginWithStrongPassword = { url, password ->
                    handleOnSaveLoginWithGeneratedStrongPassword(
                        passwordsStorage = context.components.core.passwordsStorage,
                        url = url,
                        password = password,
                    )
                },
                passwordGeneratorColorsProvider = passwordGeneratorColorsProvider,
                hideUpdateFragmentAfterSavingGeneratedPassword = { username, password ->
                    hideUpdateFragmentAfterSavingGeneratedPassword(
                        username,
                        password,
                    )
                },
                removeLastSavedGeneratedPassword = { removeLastSavedGeneratedPassword() },
                creditCardDelegate = object : CreditCardDelegate {
                    override val creditCardPickerView
                        get() = creditCardSelectBar
                    override val onManageCreditCards = {
                        val directions =
                            NavGraphDirections.actionGlobalAutofillSettingFragment()
                        findNavController().navigate(directions)
                    }
                    override val onSelectCreditCard = {
                        showBiometricPrompt(context)
                    }
                },
                addressDelegate = object : AddressDelegate {
                    override val addressPickerView
                        get() = addressSelectBar
                    override val onManageAddresses = {
                        val directions = NavGraphDirections.actionGlobalAutofillSettingFragment()
                        findNavController().navigate(directions)
                    }
                },
                androidPhotoPicker = AndroidPhotoPicker(
                    requireContext(),
                    singleMediaPicker,
                    multipleMediaPicker,
                ),
            ),
            owner = this,
            view = view,
        )

        sessionFeature.set(
            feature = SessionFeature(
                requireComponents.core.store,
                requireComponents.useCases.sessionUseCases.goBack,
                requireComponents.useCases.sessionUseCases.goForward,
                binding.engineView,
                customTabSessionId,
            ),
            owner = this,
            view = view,
        )

        lastTabFeature.set(
            feature = LastTabFeature(
                requireComponents.core.store,
                customTabSessionId,
                requireComponents.useCases.tabsUseCases.removeTab,
                requireActivity(),
            ),
            owner = this,
            view = view,
        )

        crashContentIntegration.set(
            feature = CrashContentIntegration(
                browserStore = requireComponents.core.store,
                appStore = requireComponents.appStore,
                toolbar = browserToolbarView,
                components = requireComponents,
                settings = context.settings(),
                navController = findNavController(),
                sessionId = customTabSessionId,
            ).apply {
                viewProvider = {
                    view.findViewById(R.id.crash_reporter_view)
                        ?: binding.crashReporterViewStub.inflate() as CrashContentView
                }
            },
            owner = this,
            view = view,
        )

        searchFeature.set(
            feature = SearchFeature(store, customTabSessionId) { request, tabId ->
                val parentSession = store.state.findTabOrCustomTab(tabId)
                val useCase = if (request.isPrivate) {
                    requireComponents.useCases.searchUseCases.newPrivateTabSearch
                } else {
                    requireComponents.useCases.searchUseCases.newTabSearch
                }

                if (parentSession is CustomTabSessionState) {
                    useCase.invoke(request.query)
                    requireActivity().startActivity(openInFenixIntent)
                } else {
                    useCase.invoke(request.query, parentSessionId = parentSession?.id)
                }
            },
            owner = this,
            view = view,
        )

        val accentHighContrastColor =
            ThemeManager.resolveAttribute(R.attr.actionPrimary, context)

        sitePermissionsFeature.set(
            feature = SitePermissionsFeature(
                context = context,
                storage = context.components.core.geckoSitePermissionsStorage,
                fragmentManager = parentFragmentManager,
                promptsStyling = SitePermissionsFeature.PromptsStyling(
                    gravity = getAppropriateLayoutGravity(),
                    shouldWidthMatchParent = true,
                    positiveButtonBackgroundColor = accentHighContrastColor,
                    positiveButtonTextColor = R.color.fx_mobile_text_color_action_primary,
                ),
                sessionId = customTabSessionId,
                onNeedToRequestPermissions = { permissions ->
                    requestPermissions(permissions, REQUEST_CODE_APP_PERMISSIONS)
                },
                onShouldShowRequestPermissionRationale = {
                    shouldShowRequestPermissionRationale(
                        it,
                    )
                },
                store = store,
            ),
            owner = this,
            view = view,
        )

        sitePermissionWifiIntegration.set(
            feature = SitePermissionsWifiIntegration(
                settings = context.settings(),
                wifiConnectionMonitor = context.components.wifiConnectionMonitor,
            ),
            owner = this,
            view = view,
        )

        // This component feature only works on Fenix when built on Mozilla infrastructure.
        if (BuildConfig.MOZILLA_OFFICIAL) {
            webAuthnFeature.set(
                feature = WebAuthnFeature(
                    engine = requireComponents.core.engine,
                    activity = requireActivity(),
                    exitFullScreen = requireComponents.useCases.sessionUseCases.exitFullscreen::invoke,
                    currentTab = { store.state.selectedTabId },
                ),
                owner = this,
                view = view,
            )
        }

        screenOrientationFeature.set(
            feature = ScreenOrientationFeature(
                engine = requireComponents.core.engine,
                activity = requireActivity(),
            ),
            owner = this,
            view = view,
        )

        context.settings().setSitePermissionSettingListener(viewLifecycleOwner) {
            // If the user connects to WIFI while on the BrowserFragment, this will update the
            // SitePermissionsRules (specifically autoplay) accordingly
            runIfFragmentIsAttached {
                assignSitePermissionsRules()
            }
        }
        assignSitePermissionsRules()

        fullScreenFeature.set(
            feature = FullScreenFeature(
                requireComponents.core.store,
                requireComponents.useCases.sessionUseCases,
                customTabSessionId,
                ::viewportFitChange,
                ::fullScreenChanged,
            ),
            owner = this,
            view = view,
        )

        closeFindInPageBarOnNavigation(store)

        store.flowScoped(viewLifecycleOwner) { flow ->
            flow.mapNotNull { state -> state.findTabOrCustomTabOrSelectedTab(customTabSessionId) }
                .distinctUntilChangedBy { tab -> tab.content.pictureInPictureEnabled }
                .collect { tab -> pipModeChanged(tab) }
        }

        binding.swipeRefresh.isEnabled = shouldPullToRefreshBeEnabled(false)

        if (binding.swipeRefresh.isEnabled) {
            val primaryTextColor = ThemeManager.resolveAttribute(R.attr.textPrimary, context)
            val primaryBackgroundColor = ThemeManager.resolveAttribute(R.attr.layer2, context)
            binding.swipeRefresh.apply {
                setColorSchemeResources(primaryTextColor)
                setProgressBackgroundColorSchemeResource(primaryBackgroundColor)
            }
            swipeRefreshFeature.set(
                feature = SwipeRefreshFeature(
                    requireComponents.core.store,
                    context.components.useCases.sessionUseCases.reload,
                    binding.swipeRefresh,
                    { PullToRefreshInBrowser.executed.record(NoExtras()) },
                    customTabSessionId,
                ),
                owner = this,
                view = view,
            )
        }

        webchannelIntegration.set(
            feature = FxaWebChannelIntegration(
                customTabSessionId = customTabSessionId,
                runtime = requireComponents.core.engine,
                store = requireComponents.core.store,
                accountManager = requireComponents.backgroundServices.accountManager,
                serverConfig = requireComponents.backgroundServices.serverConfig,
                activityRef = WeakReference(getActivity()),
            ),
            owner = this,
            view = view,
        )

        initializeEngineView(
            topToolbarHeight = context.settings().getTopToolbarHeight(
                includeTabStrip = customTabSessionId == null && context.settings().isTabStripEnabled,
            ),
            bottomToolbarHeight = bottomToolbarHeight,
        )

        initializeMicrosurveyFeature(context)
    }

    private fun initializeBrowserToolbar(
        activity: HomeActivity,
        store: BrowserStore,
        readerMenuController: DefaultReaderModeController,
    ) = when (activity.settings().shouldUseComposableToolbar) {
        true -> initializeBrowserToolbarComposable(activity, store, readerMenuController)
        false -> initializeBrowserToolbarView(activity, store)
    }

    private fun initializeBrowserToolbarComposable(
        activity: HomeActivity,
        store: BrowserStore,
        readerModeController: DefaultReaderModeController,
    ): BrowserToolbarComposable {
        browserScreenStore = buildBrowserScreenStore()
        val toolbarStore = buildToolbarStore(activity, readerModeController)

        browserNavigationBar =
             BrowserNavigationBar(
                context = activity,
                container = binding.browserLayout,
                toolbarStore = toolbarStore,
                browserStore = store,
                settings = activity.settings(),
                hideWhenKeyboardShown = true,
                customTabSession = customTabSessionId?.let { store.state.findCustomTab(it) },
            )

        return BrowserToolbarComposable(
            activity = activity,
            container = binding.browserLayout,
            toolbarStore = toolbarStore,
            browserScreenStore = browserScreenStore,
            appStore = requireComponents.appStore,
            browserStore = store,
            settings = activity.settings(),
            customTabSession = customTabSessionId?.let { store.state.findCustomTab(it) },
            tabStripContent = buildTabStrip(activity),
            searchSuggestionsContent = { modifier ->
                (awesomeBarComposable ?: buildAwesomeBar(activity, toolbarStore, modifier)).SearchSuggestions()
            },
            navigationBarContent = browserNavigationBar?.asComposable(),
        )
    }

    private fun initializeBrowserToolbarView(
        activity: HomeActivity,
        store: BrowserStore,
    ) = BrowserToolbarView(
        context = activity,
        container = binding.browserLayout,
        snackbarParent = binding.dynamicSnackbarContainer,
        settings = activity.settings(),
        interactor = browserToolbarInteractor,
        customTabSession = customTabSessionId?.let { store.state.findCustomTab(it) },
        lifecycleOwner = viewLifecycleOwner,
        tabStripContent = buildTabStrip(activity),
    )

    private fun buildTabStrip(
        activity: HomeActivity,
    ): @Composable () -> Unit = {
        FirefoxTheme {
            TabStrip(
                showActionButtons = context?.settings()?.shouldUseExpandedToolbar != true,
                onAddTabClick = {
                    if (activity.settings().enableHomepageAsNewTab) {
                        requireComponents.useCases.fenixBrowserUseCases.addNewHomepageTab(
                            private = activity.browsingModeManager.mode.isPrivate,
                        )
                    } else {
                        findNavController().navigate(
                            NavGraphDirections.actionGlobalHome(
                                focusOnAddressBar = true,
                            ),
                        )
                    }
                },
                onLastTabClose = { isPrivate ->
                    requireComponents.appStore.dispatch(
                        AppAction.TabStripAction.UpdateLastTabClosed(isPrivate),
                    )
                    findNavController().navigate(
                        BrowserFragmentDirections.actionGlobalHome(),
                    )
                },
                onSelectedTabClick = {},
                onCloseTabClick = { isPrivate ->
                    showUndoSnackbar(activity.tabClosedUndoMessage(isPrivate))
                },
                onTabCounterClick = { onTabCounterClicked(activity.browsingModeManager.mode) },
            )
        }
    }

    private fun buildAwesomeBar(
        activity: HomeActivity,
        toolbarStore: BrowserToolbarStore,
        modifier: Modifier,
    ) = AwesomeBarComposable(
        activity = activity,
        modifier = modifier,
        components = requireComponents,
        appStore = requireComponents.appStore,
        browserStore = requireComponents.core.store,
        toolbarStore = toolbarStore,
        navController = findNavController(),
        lifecycleOwner = this,
        showScrimWhenNoSuggestions = true,
    ).also {
        awesomeBarComposable = it
    }

    private fun buildBrowserScreenStore() = BrowserScreenStoreBuilder.build(
        context = requireContext(),
        lifecycleOwner = this,
        fragmentManager = childFragmentManager,
    )

    private fun buildToolbarStore(
        activity: HomeActivity,
        readerModeController: ReaderModeController,
    ) = BrowserToolbarStoreBuilder.build(
        activity = activity,
        lifecycleOwner = this,
        navController = findNavController(),
        appStore = activity.components.appStore,
        browserStore = activity.components.core.store,
        browserScreenStore = browserScreenStore,
        components = activity.components,
        browsingModeManager = activity.browsingModeManager,
        browserAnimator = browserAnimator,
        thumbnailsFeature = thumbnailsFeature.get(),
        readerModeController = readerModeController,
        settings = activity.settings(),
        customTabSession = customTabSessionId?.let { activity.components.core.store.state.findCustomTab(it) },
    )

    private fun showUndoSnackbar(message: String) {
        viewLifecycleOwner.lifecycleScope.allowUndo(
            binding.dynamicSnackbarContainer,
            message,
            requireContext().getString(R.string.snackbar_deleted_undo),
            {
                requireComponents.useCases.tabsUseCases.undo.invoke()
            },
            operation = { },
        )
    }

    private fun onAutocompleteBarShow() {
        removeBottomToolbarDivider()
    }

    private fun onAutocompleteBarHide() {
        restoreBottomToolbarDivider()
    }

    /**
     * Show a [Snackbar] when data is set to the device clipboard. To avoid duplicate displays of
     * information only show a [Snackbar] for Android 12 and lower.
     *
     * [See details](https://developer.android.com/develop/ui/views/touch-and-input/copy-paste#duplicate-notifications).
     */
    private fun showSnackbarForClipboardCopy() {
        if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.S_V2) {
            ContextMenuSnackbarDelegate().show(
                snackBarParentView = binding.dynamicSnackbarContainer,
                text = R.string.snackbar_copy_image_to_clipboard_confirmation,
                duration = LENGTH_LONG,
            )
        }
    }

    /**
     * Shows a biometric prompt and fallback to prompting for the password.
     */
    private fun showBiometricPrompt(context: Context) {
        if (BiometricPromptFeature.canUseFeature(BiometricManager.from(context))) {
            biometricPromptFeature.get()
                ?.requestAuthentication(getString(R.string.credit_cards_biometric_prompt_unlock_message_2))
            return
        }

        // Fallback to prompting for password with the KeyguardManager
        val manager = context.getSystemService<KeyguardManager>()
        if (manager?.isKeyguardSecure == true) {
            showPinVerification(manager)
        } else {
            // Warn that the device has not been secured
            if (context.settings().shouldShowSecurityPinWarning) {
                showPinDialogWarning(context)
            } else {
                promptsFeature.get()?.onBiometricResult(isAuthenticated = true)
            }
        }
    }

    /**
     * Shows a pin request prompt. This is only used when BiometricPrompt is unavailable.
     */
    @Suppress("DEPRECATION")
    private fun showPinVerification(manager: KeyguardManager) {
        val intent = manager.createConfirmDeviceCredentialIntent(
            getString(R.string.credit_cards_biometric_prompt_message_pin),
            getString(R.string.credit_cards_biometric_prompt_unlock_message_2),
        )

        startForResult.launch(intent)
    }

    /**
     * Shows a dialog warning about setting up a device lock PIN.
     */
    private fun showPinDialogWarning(context: Context) {
        AlertDialog.Builder(context).apply {
            setTitle(getString(R.string.credit_cards_warning_dialog_title_2))
            setMessage(getString(R.string.credit_cards_warning_dialog_message_3))

            setNegativeButton(getString(R.string.credit_cards_warning_dialog_later)) { _: DialogInterface, _ ->
                promptsFeature.get()?.onBiometricResult(isAuthenticated = false)
            }

            setPositiveButton(getString(R.string.credit_cards_warning_dialog_set_up_now)) { it: DialogInterface, _ ->
                it.dismiss()
                promptsFeature.get()?.onBiometricResult(isAuthenticated = false)
                startActivity(Intent(Settings.ACTION_SECURITY_SETTINGS))
            }

            create()
        }.show().withCenterAlignedButtons().secure(activity)

        context.settings().incrementSecureWarningCount()
    }

    private fun closeFindInPageBarOnNavigation(store: BrowserStore) {
        consumeFlow(store) { flow ->
            flow.mapNotNull { state ->
                state.findCustomTabOrSelectedTab(customTabSessionId)
            }
                .ifAnyChanged { tab ->
                    arrayOf(tab.content.url, tab.content.loadRequest)
                }
                .collect {
                    findInPageIntegration.onBackPressed()
                }
        }
    }

    @VisibleForTesting
    internal fun shouldPullToRefreshBeEnabled(inFullScreen: Boolean): Boolean {
        return FeatureFlags.PULL_TO_REFRESH_ENABLED &&
            requireContext().settings().isPullToRefreshEnabledInBrowser &&
            !inFullScreen
    }

    /**
     * Sets up the necessary layout configurations for the engine view. If the toolbar is dynamic, this method sets a
     * [CoordinatorLayout.Behavior] that will adjust the top/bottom paddings when the tab content is being scrolled.
     * If the toolbar is not dynamic, it simply sets the top and bottom margins to ensure that content is always
     * displayed above or below the respective toolbars.
     *
     * @param topToolbarHeight The height of the top toolbar, which could be zero if the toolbar is positioned at the
     * bottom, or it could be equal to the height of [BrowserToolbar].
     * @param bottomToolbarHeight The height of the bottom toolbar, which could be equal to the height of
     * [BrowserToolbar] or [ToolbarContainerView], or zero if the toolbar is positioned at the top without a navigation
     * bar.
     */
    @VisibleForTesting
    internal fun initializeEngineView(
        topToolbarHeight: Int,
        bottomToolbarHeight: Int,
    ) {
        val context = requireContext()

        if (isToolbarDynamic(context) && webAppToolbarShouldBeVisible) {
            getEngineView().setDynamicToolbarMaxHeight(topToolbarHeight + bottomToolbarHeight)

            (getSwipeRefreshLayout().layoutParams as CoordinatorLayout.LayoutParams).behavior =
                EngineViewClippingBehavior(
                    context = context,
                    attrs = null,
                    engineViewParent = getSwipeRefreshLayout(),
                    topToolbarHeight = topToolbarHeight,
                    bottomToolbarHeight = bottomToolbarHeight,
                )
        } else {
            // Ensure webpage's bottom elements are aligned to the very bottom of the engineView.
            getEngineView().setDynamicToolbarMaxHeight(0)

            // Effectively place the engineView on top/below of the toolbars if that is not dynamic.
            val swipeRefreshParams = getSwipeRefreshLayout().layoutParams as CoordinatorLayout.LayoutParams
            swipeRefreshParams.topMargin = topToolbarHeight
            swipeRefreshParams.bottomMargin = bottomToolbarHeight
        }
    }

    private fun onTabCounterClicked(browsingMode: BrowsingMode) {
        thumbnailsFeature.get()?.requestScreenshot()
        if (DefaultTabManagementFeatureHelper.enhancementsEnabled) {
            findNavController().nav(
                R.id.browserFragment,
                BrowserFragmentDirections.actionGlobalTabManagementFragment(
                    page = when (browsingMode) {
                        BrowsingMode.Normal -> Page.NormalTabs
                        BrowsingMode.Private -> Page.PrivateTabs
                    },
                ),
            )
        } else {
            findNavController().nav(
                R.id.browserFragment,
                BrowserFragmentDirections.actionGlobalTabsTrayFragment(
                    page = when (browsingMode) {
                        BrowsingMode.Normal -> Page.NormalTabs
                        BrowsingMode.Private -> Page.PrivateTabs
                    },
                ),
            )
        }
    }

    @VisibleForTesting
    internal fun initializeMicrosurveyFeature(context: Context) {
        if (context.settings().isExperimentationEnabled && context.settings().microsurveyFeatureEnabled) {
            messagingFeatureMicrosurvey.set(
                feature = MessagingFeature(
                    appStore = requireComponents.appStore,
                    surface = FenixMessageSurfaceId.MICROSURVEY,
                    runWhenReadyQueue = requireComponents.performance.visualCompletenessQueue,
                ),
                owner = viewLifecycleOwner,
                view = binding.root,
            )
        }
    }

    @Suppress("LongMethod")
    private fun initializeMicrosurveyPrompt() {
        val context = requireContext()
        val view = requireView()

        val isToolbarAtBottom = context.isToolbarAtBottom()
        val browserToolbar = (browserToolbarView as? BrowserToolbarView)?.toolbar
            ?: (browserToolbarView as BrowserToolbarComposable).layout
        val navigationBar = browserNavigationBar?.layout
        // The toolbar view has already been added directly to the container.
        if (isToolbarAtBottom) {
            binding.browserLayout.removeView(browserToolbar)
        } else if (navigationBar != null) {
            binding.browserLayout.removeView(navigationBar)
        }

        _bottomToolbarContainerView = BottomToolbarContainerView(
            context = context,
            parent = binding.browserLayout,
            hideOnScroll = isToolbarDynamic(context),
            content = {
                FirefoxTheme {
                    Column {
                        val activity = requireActivity() as HomeActivity

                        if (!activity.isMicrosurveyPromptDismissed.value) {
                            currentMicrosurvey?.let {
                                if (isToolbarAtBottom) {
                                    removeBottomToolbarDivider()
                                }

                                Divider()

                                MicrosurveyRequestPrompt(
                                    microsurvey = it,
                                    activity = activity,
                                    onStartSurveyClicked = {
                                        context.components.appStore.dispatch(MicrosurveyAction.Started(it.id))
                                        findNavController().nav(
                                            R.id.browserFragment,
                                            BrowserFragmentDirections.actionGlobalMicrosurveyDialog(it.id),
                                        )
                                    },
                                    onCloseButtonClicked = {
                                        context.components.appStore.dispatch(
                                            MicrosurveyAction.Dismissed(it.id),
                                        )

                                        context.settings().shouldShowMicrosurveyPrompt = false
                                        activity.isMicrosurveyPromptDismissed.value = true
                                    },
                                )
                            }
                        } else {
                            restoreBottomToolbarDivider()
                        }

                        if (isToolbarAtBottom) {
                            AndroidView(factory = { _ -> browserToolbar })
                        } else if (navigationBar != null) {
                            AndroidView(factory = { _ -> navigationBar })
                        }
                    }
                }
            },
        ).apply {
            // This covers the usecase when the app goes into fullscreen mode from portrait orientation.
            // Transition to fullscreen happens first, and orientation change follows. Microsurvey container is getting
            // reinitialized when going into landscape mode, but it shouldn't be visible if the app is already in the
            // fullscreen mode. It still has to be initialized to be shown after the user exits the fullscreen.
            val isFullscreen = fullScreenFeature.get()?.isFullScreen == true
            toolbarContainerView.isVisible = !isFullscreen
        }

        bottomToolbarContainerIntegration.set(
            feature = BottomToolbarContainerIntegration(
                toolbar = bottomToolbarContainerView.toolbarContainerView,
                store = requireComponents.core.store,
                sessionId = customTabSessionId,
            ),
            owner = this,
            view = view,
        )

        reinitializeEngineView()
    }

    private fun removeBottomToolbarDivider() {
        browserToolbarView.updateDividerVisibility(false)
        browserToolbarView.layout.elevation = 0.0f
    }

    private fun restoreBottomToolbarDivider() {
        browserToolbarView.updateDividerVisibility(true)
    }

    private var currentMicrosurvey: MicrosurveyUIData? = null

    /**
     * Listens for the microsurvey message and initializes the microsurvey prompt if one is available.
     */
    private fun listenForMicrosurveyMessage(context: Context) {
        binding.root.consumeFrom(context.components.appStore, viewLifecycleOwner) { state ->
            state.messaging.messageToShow[FenixMessageSurfaceId.MICROSURVEY]?.let { message ->
                if (message.id != currentMicrosurvey?.id) {
                    message.toMicrosurveyUIData()?.let { microsurvey ->
                        context.components.settings.shouldShowMicrosurveyPrompt = true
                        currentMicrosurvey = microsurvey

                        _bottomToolbarContainerView?.toolbarContainerView.let {
                            binding.browserLayout.removeView(it)
                        }

                        initializeMicrosurveyPrompt()
                    }
                }
            }
        }
    }

    private fun shouldShowMicrosurveyPrompt(context: Context) =
        context.components.settings.shouldShowMicrosurveyPrompt

    private fun isToolbarDynamic(context: Context) =
        !context.settings().shouldUseFixedTopToolbar && context.settings().isDynamicToolbarEnabled

    /**
     * Returns a list of context menu items [ContextMenuCandidate] for the context menu
     */
    protected abstract fun getContextMenuCandidates(
        context: Context,
        view: View,
    ): List<ContextMenuCandidate>

    @VisibleForTesting
    internal fun observeRestoreComplete(store: BrowserStore, navController: NavController) {
        val activity = activity as HomeActivity
        consumeFlow(store) { flow ->
            flow.map { state -> state.restoreComplete }
                .distinctUntilChanged()
                .collect { restored ->
                    if (restored) {
                        // Once tab restoration is complete, if there are no tabs to show in the browser, go home
                        val tabs =
                            store.state.getNormalOrPrivateTabs(
                                activity.browsingModeManager.mode.isPrivate,
                            )
                        if (tabs.isEmpty() || store.state.selectedTabId == null) {
                            navController.popBackStack(R.id.homeFragment, false)
                        }
                    }
                }
        }
    }

    @VisibleForTesting
    internal fun observeTabSelection(store: BrowserStore, isCustomTabSession: Boolean) {
        consumeFlow(store) { flow ->
            flow.distinctUntilChangedBy {
                it.selectedTabId
            }
                .mapNotNull {
                    it.selectedTab
                }
                .collect {
                    dismissDownloadDialogs()
                    handleTabSelected(it, isCustomTabSession)
            }
        }
    }

    private fun dismissDownloadDialogs() {
        downloadDialog?.dismiss()
    }

    @VisibleForTesting
    @Suppress("ComplexCondition")
    internal fun observeTabSource(store: BrowserStore) {
        consumeFlow(store) { flow ->
            flow.mapNotNull { state ->
                state.selectedTab
            }
                .collect {
                    if (!requireComponents.fenixOnboarding.userHasBeenOnboarded() &&
                        it.content.loadRequest?.triggeredByRedirect != true &&
                        it.source !is SessionState.Source.External &&
                        it.content.url !in onboardingLinksList
                    ) {
                        requireComponents.fenixOnboarding.finish()
                    }
                }
        }
    }

    private fun handleTabSelected(selectedTab: TabSessionState, isCustomTabSession: Boolean) {
        if (!this.isRemoving && !isCustomTabSession) {
            updateThemeForSession(selectedTab)
        }

        if (browserInitialized) {
            view?.let {
                fullScreenChanged(false)
                browserToolbarView.expand()

                @Suppress("DEPRECATION")
                it.announceForAccessibility(selectedTab.toDisplayTitle())
            }
        } else {
            view?.let { view -> initializeUI(view) }
        }
    }

    @CallSuper
    override fun onResume() {
        super.onResume()
        val components = requireComponents

        val preferredColorScheme = components.core.getPreferredColorScheme()
        if (components.core.engine.settings.preferredColorScheme != preferredColorScheme) {
            components.core.engine.settings.preferredColorScheme = preferredColorScheme
            components.useCases.sessionUseCases.reload()
        }
        hideToolbar()

        evaluateMessagesForMicrosurvey(components)

        BiometricAuthenticationManager.biometricAuthenticationNeededInfo.shouldShowAuthenticationPrompt =
            true
        BiometricAuthenticationManager.biometricAuthenticationNeededInfo.authenticationStatus =
            AuthenticationStatus.NOT_AUTHENTICATED
    }

    private fun evaluateMessagesForMicrosurvey(components: Components) =
        components.appStore.dispatch(MessagingAction.Evaluate(FenixMessageSurfaceId.MICROSURVEY))

    @CallSuper
    override fun onPause() {
        super.onPause()
        if (findNavController().currentDestination?.id != R.id.searchDialogFragment) {
            view?.hideKeyboard()
        }
    }

    @CallSuper
    override fun onStop() {
        super.onStop()
        dismissDownloadDialogs()

        requireComponents.core.store.state.findTabOrCustomTabOrSelectedTab(customTabSessionId)
            ?.let { session ->
                // If we didn't enter PiP, exit full screen on stop
                if (!session.content.pictureInPictureEnabled && fullScreenFeature.onBackPressed()) {
                    fullScreenChanged(false)
                }
            }
    }

    @CallSuper
    override fun onBackPressed(): Boolean {
        return findInPageIntegration.onBackPressed() ||
                fullScreenFeature.onBackPressed() ||
                promptsFeature.onBackPressed() ||
                sessionFeature.onBackPressed() ||
                lastTabFeature.onBackPressed()
    }

    @CallSuper
    override fun onForwardPressed(): Boolean {
        return sessionFeature.onForwardPressed()
    }

    /**
     * Forwards activity results to the [ActivityResultHandler] features.
     */
    override fun onActivityResult(requestCode: Int, data: Intent?, resultCode: Int): Boolean {
        return listOf(
            promptsFeature,
            webAuthnFeature,
        ).any { it.onActivityResult(requestCode, data, resultCode) }
    }

    /**
     * Navigate to GlobalTabHistoryDialogFragment.
     */
    private fun navigateToGlobalTabHistoryDialogFragment() {
        findNavController().navigate(
            NavGraphDirections.actionGlobalTabHistoryDialogFragment(
                activeSessionId = customTabSessionId,
            ),
        )
    }

    override fun onBackLongPressed(): Boolean {
        navigateToGlobalTabHistoryDialogFragment()
        return true
    }

    override fun onForwardLongPressed(): Boolean {
        navigateToGlobalTabHistoryDialogFragment()
        return true
    }

    /**
     * Saves the external app session ID to be restored later in [onViewStateRestored].
     */
    final override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(KEY_CUSTOM_TAB_SESSION_ID, customTabSessionId)
        outState.putString(LAST_SAVED_GENERATED_PASSWORD, lastSavedGeneratedPassword)
    }

    /**
     * Retrieves the external app session ID saved by [onSaveInstanceState].
     */
    final override fun onViewStateRestored(savedInstanceState: Bundle?) {
        super.onViewStateRestored(savedInstanceState)
        savedInstanceState?.getString(KEY_CUSTOM_TAB_SESSION_ID)?.let {
            if (requireComponents.core.store.state.findCustomTab(it) != null) {
                customTabSessionId = it
            }
        }
        lastSavedGeneratedPassword = savedInstanceState?.getString(LAST_SAVED_GENERATED_PASSWORD)
    }

    /**
     * Forwards permission grant results to one of the features.
     */
    @Suppress("OVERRIDE_DEPRECATION")
    final override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<String>,
        grantResults: IntArray,
    ) {
        val feature: PermissionsFeature? = when (requestCode) {
            REQUEST_CODE_DOWNLOAD_PERMISSIONS -> downloadsFeature.get()
            REQUEST_CODE_PROMPT_PERMISSIONS -> promptsFeature.get()
            REQUEST_CODE_APP_PERMISSIONS -> sitePermissionsFeature.get()
            else -> null
        }
        feature?.onPermissionsResult(permissions, grantResults)
    }

    protected abstract fun navToQuickSettingsSheet(
        tab: SessionState,
        sitePermissions: SitePermissions?,
    )

    /**
     * Returns the layout [android.view.Gravity] for the quick settings and ETP dialog.
     */
    protected fun getAppropriateLayoutGravity(): Int =
        requireComponents.settings.toolbarPosition.androidGravity

    /**
     * Configure the engine view to know where to place website's dynamic elements
     * depending on the space taken by any dynamic toolbar.
     */
    @VisibleForTesting(otherwise = VisibleForTesting.PROTECTED)
    internal fun configureEngineViewWithDynamicToolbarsMaxHeight() {
        val currentTab = requireComponents.core.store.state.findCustomTabOrSelectedTab(customTabSessionId)
        if (currentTab?.content?.isPdf == true) return
        if (findInPageIntegration.get()?.isFeatureActive == true) return
        val toolbarHeights = view?.let { probeToolbarHeights(it) } ?: return

        context?.also {
            if (isToolbarDynamic(it)) {
                if (!requireComponents.core.geckoRuntime.isInteractiveWidgetDefaultResizesVisual) {
                    getEngineView().setDynamicToolbarMaxHeight(toolbarHeights.first + toolbarHeights.second)
                }
            } else {
                (getSwipeRefreshLayout().layoutParams as? CoordinatorLayout.LayoutParams)?.apply {
                    bottomMargin = toolbarHeights.second
                }
            }
        }
    }

    /**
     * Get an instant reading of the top toolbar height and the bottom toolbar height.
     */
    private fun probeToolbarHeights(rootView: View): Pair<Int, Int> {
        val context = rootView.context
        // Avoid any change for scenarios where the toolbar is not shown
        if (fullScreenFeature.get()?.isFullScreen == true) return 0 to 0

        val topToolbarHeight = context.settings().getTopToolbarHeight(
            includeTabStrip = customTabSessionId == null && context.settings().isTabStripEnabled,
        )
        val bottomToolbarHeight = context.settings().getBottomToolbarHeight()

        return topToolbarHeight to bottomToolbarHeight
    }

    /**
     * Updates the site permissions rules based on user settings.
     */
    private fun assignSitePermissionsRules() {
        val rules = requireComponents.settings.getSitePermissionsCustomSettingsRules()

        sitePermissionsFeature.withFeature {
            it.sitePermissionsRules = rules
        }
    }

    /**
     * Displays the quick settings dialog,
     * which lets the user control tracking protection and site settings.
     */
    private fun showQuickSettingsDialog() {
        val tab = getCurrentTab() ?: return
        viewLifecycleOwner.lifecycleScope.launch(Main) {
            val sitePermissions: SitePermissions? = tab.content.url.getOrigin()?.let { origin ->
                val storage = requireComponents.core.permissionStorage
                storage.findSitePermissionsBy(origin, tab.content.private)
            }

            view?.let {
                navToQuickSettingsSheet(tab, sitePermissions)
            }
        }
    }

    /**
     * Set the activity normal/private theme to match the current session.
     */
    @VisibleForTesting
    internal fun updateThemeForSession(session: SessionState) {
        val sessionMode = BrowsingMode.fromBoolean(session.content.private)
        (activity as HomeActivity).browsingModeManager.mode = sessionMode
    }

    /**
     * A safe version of [getCurrentTab] that safely checks for context nullability.
     */
    protected fun getSafeCurrentTab(): SessionState? {
        return context?.components?.core?.store?.state?.findCustomTabOrSelectedTab(customTabSessionId)
    }

    @VisibleForTesting
    internal fun getCurrentTab(): SessionState? {
        return requireComponents.core.store.state.findCustomTabOrSelectedTab(customTabSessionId)
    }

    private suspend fun bookmarkTapped(sessionUrl: String, sessionTitle: String) = withContext(IO) {
        val bookmarksStorage = requireComponents.core.bookmarksStorage
        val existing =
            bookmarksStorage.getBookmarksWithUrl(sessionUrl).firstOrNull { it.url == sessionUrl }
        if (existing != null) {
            // Bookmark exists, go to edit fragment
            withContext(Main) {
                nav(
                    R.id.browserFragment,
                    BrowserFragmentDirections.actionGlobalBookmarkEditFragment(existing.guid, true),
                )
            }
        } else {
            // Save bookmark, then go to edit fragment
            try {
                val parentNode = Result.runCatching {
                    val parentGuid = bookmarksStorage
                        .getRecentBookmarks(1)
                        .firstOrNull()
                        ?.parentGuid
                        ?: BookmarkRoot.Mobile.id

                    bookmarksStorage.getBookmark(parentGuid)!!
                }.getOrElse {
                    // this should be a temporary hack until the menu redesign is completed
                    // see MenuDialogMiddleware for the updated version
                    throw PlacesApiException.UrlParseFailed(reason = "no parent node")
                }

                val guid = bookmarksStorage.addItem(
                    parentNode.guid,
                    url = sessionUrl,
                    title = sessionTitle,
                    position = null,
                )

                MetricsUtils.recordBookmarkMetrics(MetricsUtils.BookmarkAction.ADD, METRIC_SOURCE)
                showBookmarkSavedSnackbar(
                    message = getString(
                        R.string.bookmark_saved_in_folder_snackbar,
                        friendlyRootTitle(requireContext(), parentNode),
                    ),
                    onClick = {
                        MetricsUtils.recordBookmarkMetrics(
                            MetricsUtils.BookmarkAction.EDIT,
                            TOAST_METRIC_SOURCE,
                        )
                        findNavController().navigateWithBreadcrumb(
                            directions = BrowserFragmentDirections.actionGlobalBookmarkEditFragment(
                                guid,
                                true,
                            ),
                            navigateFrom = "BrowserFragment",
                            navigateTo = "ActionGlobalBookmarkEditFragment",
                            crashReporter = requireContext().components.analytics.crashReporter,
                        )
                    },
                )
            } catch (e: PlacesApiException.UrlParseFailed) {
                withContext(Main) {
                    view?.let {
                        Snackbar.make(
                            snackBarParentView = binding.dynamicSnackbarContainer,
                            snackbarState = SnackbarState(
                                message = getString(R.string.bookmark_invalid_url_error),
                                duration = SnackbarState.Duration.Preset.Long,
                            ),
                        ).show()
                    }
                }
            }
        }
    }

    private fun showBookmarkSavedSnackbar(message: String, onClick: () -> Unit) {
        Snackbar.make(
            snackBarParentView = binding.dynamicSnackbarContainer,
            snackbarState = SnackbarState(
                message = message,
                duration = SnackbarState.Duration.Preset.Long,
                action = Action(
                    label = getString(R.string.edit_bookmark_snackbar_action),
                    onClick = onClick,
                ),
            ),
        ).show()
    }

    override fun onHomePressed() = pipFeature?.onHomePressed() ?: false

    /**
     * Exit fullscreen mode when exiting PIP mode
     */
    private fun pipModeChanged(session: SessionState) {
        if (!session.content.pictureInPictureEnabled && session.content.fullScreen && isAdded) {
            onBackPressed()
            fullScreenChanged(false)
        }
    }

    final override fun onPictureInPictureModeChanged(enabled: Boolean) {
        if (enabled) MediaState.pictureInPicture.record(NoExtras())
        pipFeature?.onPictureInPictureModeChanged(enabled)
    }

    private fun viewportFitChange(layoutInDisplayCutoutMode: Int) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            val layoutParams = activity?.window?.attributes
            layoutParams?.layoutInDisplayCutoutMode = layoutInDisplayCutoutMode
            activity?.window?.attributes = layoutParams
        }
    }

    @VisibleForTesting
    internal fun fullScreenChanged(inFullScreen: Boolean) {
        val activity = activity ?: return
        if (inFullScreen) {
            // Close find in page bar if opened
            findInPageIntegration.onBackPressed()

            FullScreenNotificationToast(
                activity = activity,
                gestureNavString = getString(R.string.exit_fullscreen_with_gesture_short),
                backButtonString = getString(R.string.exit_fullscreen_with_back_button_short),
                GestureNavUtils,
            ).show()

            activity.enterImmersiveMode(
                setOnApplyWindowInsetsListener = { key: String, listener: OnApplyWindowInsetsListener ->
                    binding.engineView.addWindowInsetsListener(key, listener)
                },
            )
            (view as? SwipeGestureLayout)?.isSwipeEnabled = false
            expandBrowserView()

            MediaState.fullscreen.record(NoExtras())
        } else {
            activity.exitImmersiveMode(
                unregisterOnApplyWindowInsetsListener = binding.engineView::removeWindowInsetsListener,
            )

            (view as? SwipeGestureLayout)?.isSwipeEnabled = true
            (activity as? HomeActivity)?.let { homeActivity ->
                // ExternalAppBrowserActivity exclusively handles it's own theming unless in private mode.
                if (homeActivity !is ExternalAppBrowserActivity || homeActivity.browsingModeManager.mode.isPrivate) {
                    homeActivity.themeManager.applyStatusBarTheme(
                        homeActivity,
                        requireContext().settings().isTabStripEnabled,
                    )
                }
            }
            collapseBrowserView()
        }

        binding.swipeRefresh.isEnabled = shouldPullToRefreshBeEnabled(inFullScreen)
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PROTECTED)
    internal fun expandBrowserView() {
        browserToolbarView.apply {
            collapse()
            gone()
        }

        browserNavigationBar?.apply {
            collapse()
            gone()
        }

        _bottomToolbarContainerView?.toolbarContainerView?.apply {
            collapse()
            isVisible = false
        }
        val browserEngine = getSwipeRefreshLayout().layoutParams as CoordinatorLayout.LayoutParams
        browserEngine.behavior = null
        browserEngine.bottomMargin = 0
        browserEngine.topMargin = 0
        getSwipeRefreshLayout().apply {
            translationY = 0f
            requestLayout()
        }

        getEngineView().apply {
            setDynamicToolbarMaxHeight(0)
            setVerticalClipping(0)
        }
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PROTECTED)
    internal fun collapseBrowserView() {
        if (webAppToolbarShouldBeVisible) {
            browserToolbarView.visible()
            browserNavigationBar?.visible()
            _bottomToolbarContainerView?.toolbarContainerView?.isVisible = true
            reinitializeEngineView()
            browserToolbarView.expand()
            browserNavigationBar?.expand()
            _bottomToolbarContainerView?.toolbarContainerView?.expand()
        }
    }

    @CallSuper
    internal open fun onUpdateToolbarForConfigurationChange(toolbar: FenixBrowserToolbarView) {
        (toolbar as? BrowserToolbarView)?.dismissMenu()

        reinitializeEngineView()

        // If the microsurvey feature is visible, we should update it's state.
        if (shouldShowMicrosurveyPrompt(requireContext())) {
            updateMicrosurveyPromptForConfigurationChange(
                parent = binding.browserLayout,
                bottomToolbarContainerView = _bottomToolbarContainerView?.toolbarContainerView,
                reinitializeMicrosurveyPrompt = ::initializeMicrosurveyPrompt,
            )
        }

        view?.let { setupIMEInsetsHandling(it) }
    }

    @VisibleForTesting
    internal fun reinitializeEngineView() {
        val isFullscreen = fullScreenFeature.get()?.isFullScreen == true
        val shouldToolbarsBeHidden = isFullscreen || !webAppToolbarShouldBeVisible
        val topToolbarHeight = requireContext().settings().getTopToolbarHeight(
            includeTabStrip = customTabSessionId == null && requireContext().settings().isTabStripEnabled,
        )
        val bottomToolbarHeight = requireContext().settings().getBottomToolbarHeight()

        initializeEngineView(
            topToolbarHeight = if (shouldToolbarsBeHidden) 0 else topToolbarHeight,
            bottomToolbarHeight = if (shouldToolbarsBeHidden) 0 else bottomToolbarHeight,
        )
    }

    /*
     * Dereference these views when the fragment view is destroyed to prevent memory leaks
     */
    override fun onDestroyView() {
        super.onDestroyView()

        // Diagnostic breadcrumb for "Display already aquired" crash:
        // https://github.com/mozilla-mobile/android-components/issues/7960
        breadcrumb(
            message = "onDestroyView()",
        )

        binding.engineView.setActivityContext(null)
        requireContext().accessibilityManager.removeAccessibilityStateChangeListener(this)

        loginSelectBar = null
        addressSelectBar = null
        creditCardSelectBar = null
        suggestStrongPasswordBar = null

        _findInPageLauncher = null
        _browserToolbarMenuController = null

        _menuButtonView = null

        _bottomToolbarContainerView = null
        _browserToolbarView = null
        awesomeBarComposable = null
        browserNavigationBar = null
        _browserToolbarInteractor = null
        _binding = null
    }

    override fun onAttach(context: Context) {
        super.onAttach(context)

        // Diagnostic breadcrumb for "Display already aquired" crash:
        // https://github.com/mozilla-mobile/android-components/issues/7960
        breadcrumb(
            message = "onAttach()",
        )
    }

    override fun onDetach() {
        super.onDetach()

        // Diagnostic breadcrumb for "Display already aquired" crash:
        // https://github.com/mozilla-mobile/android-components/issues/7960
        breadcrumb(
            message = "onDetach()",
        )
    }

    companion object {
        private const val KEY_CUSTOM_TAB_SESSION_ID = "custom_tab_session_id"
        private const val REQUEST_CODE_DOWNLOAD_PERMISSIONS = 1
        private const val REQUEST_CODE_PROMPT_PERMISSIONS = 2
        private const val REQUEST_CODE_APP_PERMISSIONS = 3
        private const val METRIC_SOURCE = "page_action_menu"
        private const val TOAST_METRIC_SOURCE = "add_bookmark_toast"
        private const val LAST_SAVED_GENERATED_PASSWORD = "last_saved_generated_password"

        val onboardingLinksList: List<String> = listOf(
            SupportUtils.getMozillaPageUrl(SupportUtils.MozillaPage.PRIVATE_NOTICE),
            SupportUtils.FXACCOUNT_SUMO_URL,
        )
    }

    override fun onAccessibilityStateChanged(enabled: Boolean) {
        if (_browserToolbarView != null) {
            browserToolbarView.setToolbarBehavior(requireContext().settings().toolbarPosition, enabled)
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)

        if (findInPageIntegration.get()?.isFeatureActive != true &&
            fullScreenFeature.get()?.isFullScreen != true
        ) {
            _browserToolbarView?.let {
                onUpdateToolbarForConfigurationChange(it)
            }
        }
    }

    // This method is called in response to native web extension messages from
    // content scripts (e.g the reader view extension). By the time these
    // messages are processed the fragment/view may no longer be attached.
    internal fun safeInvalidateBrowserToolbarView() {
        runIfFragmentIsAttached {
            val toolbarView = _browserToolbarView as? BrowserToolbarView
            if (toolbarView != null) {
                toolbarView.toolbar.invalidateActions()
                toolbarView.toolbarIntegration.invalidateMenu()
            }
            _menuButtonView?.setHighlightStatus()
        }
    }

    /**
     * Convenience method for replacing EngineView (id/engineView) in unit tests.
     */
    @VisibleForTesting
    internal fun getEngineView() = binding.engineView

    /**
     * Convenience method for replacing SwipeRefreshLayout (id/swipeRefresh) in unit tests.
     */
    @VisibleForTesting
    internal fun getSwipeRefreshLayout() = binding.swipeRefresh

    internal fun shouldShowCompletedDownloadDialog(
        downloadState: DownloadState,
        status: DownloadState.Status,
    ): Boolean {
        val isValidStatus = status in listOf(DownloadState.Status.COMPLETED, DownloadState.Status.FAILED)
        val isSameTab = downloadState.sessionId == (getCurrentTab()?.id ?: false)

        return isValidStatus && isSameTab
    }

    private fun handleOnSaveLoginWithGeneratedStrongPassword(
        passwordsStorage: SyncableLoginsStorage,
        url: String,
        password: String,
    ) {
        lastSavedGeneratedPassword = password
        val loginToSave = LoginEntry(
            origin = url,
            httpRealm = url,
            username = "",
            password = password,
        )
        var saveLoginJob: Deferred<Unit>? = null
        lifecycleScope.launch(IO) {
            saveLoginJob = async {
                try {
                    passwordsStorage.add(loginToSave)
                } catch (loginException: LoginsApiException) {
                    loginException.printStackTrace()
                    Log.e(
                        "Add new login",
                        "Failed to add new login with generated password.",
                        loginException,
                    )
                }
                saveLoginJob?.await()
            }
            saveLoginJob?.invokeOnCompletion {
                if (it is CancellationException) {
                    saveLoginJob?.cancel()
                }
            }
        }
    }

    private fun hideUpdateFragmentAfterSavingGeneratedPassword(
        username: String,
        password: String,
    ): Boolean {
        return username.isEmpty() && password == lastSavedGeneratedPassword
    }

    private fun removeLastSavedGeneratedPassword() {
        lastSavedGeneratedPassword = null
    }

    private fun launchFindInPageFeature(view: View, store: BrowserStore) {
        val findInPageBar = view.findViewById(R.id.findInPageView)
            ?: (binding.findInPageViewStub.inflate() as FindInPageBar)
        findInPageIntegration.set(
            feature = FindInPageIntegration(
                store = store,
                appStore = requireComponents.appStore,
                sessionId = customTabSessionId,
                view = findInPageBar,
                engineView = binding.engineView,
                toolbarsHideCallback = {
                    expandBrowserView()
                },
                toolbarsResetCallback = {
                    onUpdateToolbarForConfigurationChange(browserToolbarView)
                    collapseBrowserView()
                },
            ),
            owner = this,
            view = view,
        )
        findInPageIntegration.withFeature { it.launch() }
    }

    private fun setupIMEInsetsHandling(view: View) {
        when (context?.settings()?.toolbarPosition) {
            ToolbarPosition.BOTTOM -> {
                val toolbar = listOf(
                    _bottomToolbarContainerView?.toolbarContainerView,
                    _browserToolbarView?.layout,
                ).firstOrNull { it != null } ?: return

                ImeInsetsSynchronizer.setup(
                    targetView = toolbar,
                    onIMEAnimationStarted = { isKeyboardShowingUp, keyboardHeight ->
                        // If the keyboard is hiding have the engine view immediately expand to the entire height of the
                        // screen and ensure the toolbar is shown above keyboard before both would be animated down.
                        if (!isKeyboardShowingUp) {
                            (view.layoutParams as? ViewGroup.MarginLayoutParams)?.bottomMargin = 0
                            (toolbar.layoutParams as? ViewGroup.MarginLayoutParams)?.bottomMargin = keyboardHeight
                            view.requestLayout()
                        }
                    },
                    onIMEAnimationFinished = { isKeyboardShowingUp, keyboardHeight ->
                        // If the keyboard is showing up keep the engine view covering the entire height
                        // of the screen until the animation is finished to avoid reflowing the web content
                        // together with the keyboard animation in a short burst of updates.
                        if (isKeyboardShowingUp || keyboardHeight == 0) {
                            (view.layoutParams as? ViewGroup.MarginLayoutParams)?.bottomMargin = keyboardHeight
                            (toolbar.layoutParams as? ViewGroup.MarginLayoutParams)?.bottomMargin = 0
                            view.requestLayout()
                        }
                    },
                )
            }

            ToolbarPosition.TOP -> {
                ImeInsetsSynchronizer.setup(
                    targetView = view,
                    synchronizeViewWithIME = false,
                    onIMEAnimationStarted = { isKeyboardShowingUp, _ ->
                        if (!isKeyboardShowingUp) {
                            (view.layoutParams as? ViewGroup.MarginLayoutParams)?.bottomMargin = 0
                            view.requestLayout()
                        }
                    },
                    onIMEAnimationFinished = { isKeyboardShowingUp, keyboardHeight ->
                        if (isKeyboardShowingUp || keyboardHeight == 0) {
                            (view.layoutParams as? ViewGroup.MarginLayoutParams)?.bottomMargin = keyboardHeight
                            view.requestLayout()
                        }
                    },
                )
            }

            else -> {
                // no-op
            }
        }
    }

    private fun openManageStorageSettings() {
        val intent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Intent(StorageManager.ACTION_MANAGE_STORAGE)
        } else {
            Intent("android.intent.action.INTERNAL_STORAGE_SETTINGS")
        }

        if (intent.resolveActivity(requireContext().packageManager) != null) {
            requireContext().startActivity(intent)
        }
    }
}
