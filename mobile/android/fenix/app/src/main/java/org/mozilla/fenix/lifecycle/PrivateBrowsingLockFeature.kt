/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.lifecycle

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.Fragment
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.Lifecycle.State.RESUMED
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import mozilla.components.browser.state.selector.privateTabs
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.lib.state.ext.flow
import mozilla.components.lib.state.ext.flowScoped
import org.mozilla.fenix.GleanMetrics.PrivateBrowsingLocked
import org.mozilla.fenix.R
import org.mozilla.fenix.browser.browsingmode.BrowsingMode
import org.mozilla.fenix.components.AppStore
import org.mozilla.fenix.components.appstate.AppAction.PrivateBrowsingLockAction
import org.mozilla.fenix.components.appstate.AppState
import org.mozilla.fenix.ext.components
import org.mozilla.fenix.settings.biometric.BiometricPromptFeature
import org.mozilla.fenix.settings.biometric.BiometricUtils
import org.mozilla.fenix.settings.biometric.DefaultBiometricUtils

/**
 * An interface to access and observe the enabled/disabled state of the Private Browsing Lock feature.
 */
interface PrivateBrowsingLockStorage {
    /**
     * Returns the current enabled state of the private browsing lock feature.
     */
    val isFeatureEnabled: Boolean

    /**
     * Registers a listener that is invoked whenever the enabled state changes.
     *
     * @param listener A lambda that receives the new boolean value when it changes.
     */
    fun addFeatureStateListener(listener: (Boolean) -> Unit)

    /**
     * Starts observing shared preferences for changes in the feature flag.
     *
     * NB: some devices may garbage collect preference listeners very aggressively,
     * so this method should be invoked each time the feature becomes active.
     */
    fun startObservingSharedPrefs()
}

/**
 * A default implementation of `PrivateBrowsingLockStorage`.
 *
 * @param preferences The [SharedPreferences] instance from which to read the preference value.
 * @param privateBrowsingLockPrefKey The key in [SharedPreferences] representing the feature flag.
 */
class DefaultPrivateBrowsingLockStorage(
    private val preferences: SharedPreferences,
    private val privateBrowsingLockPrefKey: String,
) : PrivateBrowsingLockStorage {
    private var listener: ((Boolean) -> Unit)? = null
    private val onFeatureStateChanged = SharedPreferences.OnSharedPreferenceChangeListener { prefs, key ->
        if (key == privateBrowsingLockPrefKey) {
            listener?.invoke(prefs.getBoolean(privateBrowsingLockPrefKey, false))
        }
    }

    override val isFeatureEnabled: Boolean
        get() = preferences.getBoolean(privateBrowsingLockPrefKey, false)

    override fun addFeatureStateListener(listener: (Boolean) -> Unit) {
        this.listener = listener
    }

    override fun startObservingSharedPrefs() {
        preferences.registerOnSharedPreferenceChangeListener(onFeatureStateChanged)
    }
}

/**
 * A lifecycle-aware feature that locks private browsing mode behind authentication
 * when certain conditions are met (e.g., switching modes or backgrounding the app).
 */
class PrivateBrowsingLockFeature(
    private val appStore: AppStore,
    private val browserStore: BrowserStore,
    private val storage: PrivateBrowsingLockStorage,
) : DefaultLifecycleObserver {
    private var browserStoreScope: CoroutineScope? = null
    private var isFeatureEnabled = false

    init {
        isFeatureEnabled = storage.isFeatureEnabled

        // Use our app state during feature init which can happen after Activity recreation.
        val isLocked =
            browserStore.state.privateTabs.isNotEmpty() && appStore.state.isPrivateScreenLocked

        updateFeatureState(
            isFeatureEnabled = isFeatureEnabled,
            isLocked = isLocked,
        )

        observeFeatureStateUpdates()
    }

    private fun observeFeatureStateUpdates() {
        storage.addFeatureStateListener { isEnabled ->
            isFeatureEnabled = isEnabled

            updateFeatureState(
                isFeatureEnabled = isFeatureEnabled,
                isLocked = appStore.state.mode == BrowsingMode.Normal &&
                        browserStore.state.privateTabs.isNotEmpty(),
            )
        }
    }

    private fun updateFeatureState(
        isFeatureEnabled: Boolean,
        isLocked: Boolean,
    ) {
        if (isFeatureEnabled) {
            start(isLocked)
        } else {
            stop()
        }
    }

    private fun start(isLocked: Boolean) {
        observePrivateTabsClosure()

        appStore.dispatch(
            PrivateBrowsingLockAction.UpdatePrivateBrowsingLock(
                isLocked = isLocked,
            ),
        )
    }

    private fun stop() {
        browserStoreScope?.cancel()
        browserStoreScope = null

        appStore.dispatch(
            PrivateBrowsingLockAction.UpdatePrivateBrowsingLock(
                isLocked = false,
            ),
        )
    }

    private fun observePrivateTabsClosure() {
        browserStoreScope = browserStore.flowScoped { flow ->
            flow
                .map { it.privateTabs.size }
                .distinctUntilChanged()
                .filter { it == 0 }
                .collect {
                    // When all private tabs are closed, we don't need to lock the private mode.
                    appStore.dispatch(
                        PrivateBrowsingLockAction.UpdatePrivateBrowsingLock(
                            isLocked = false,
                        ),
                    )
                }
        }
    }

    override fun onPause(owner: LifecycleOwner) {
        super.onPause(owner)

        if (owner is Activity) {
            maybeLockPrivateMode(
                activity = owner,
                shouldLockFocusedWindow = false,
            )
        }
    }

    override fun onStop(owner: LifecycleOwner) {
        super.onStop(owner)

        if (owner is Activity) {
            maybeLockPrivateMode(
                activity = owner,
                shouldLockFocusedWindow = true,
            )
        }
    }

    override fun onResume(owner: LifecycleOwner) {
        super.onResume(owner)
        storage.startObservingSharedPrefs()
    }

    @Suppress("ReturnCount")
    private fun maybeLockPrivateMode(
        activity: Activity,
        shouldLockFocusedWindow: Boolean,
    ) {
        if (!isFeatureEnabled) return

        // Skip if the activity is being recreated (e.g. browsing mode change).
        if (activity.isChangingConfigurations) return

        // Avoid redundant locking if already locked during onStop; onStop handles an edge case when
        // the app is minimized while showing a system dialog (e.g. notification permission).
        if (appStore.state.isPrivateScreenLocked) return

        // Optionally skip if the window still has focus (e.g. while showing a system dialog).
        if (!shouldLockFocusedWindow && !activity.isFinishing) return

        // Skip locking private mode if there are no private tabs.
        if (browserStore.state.privateTabs.isEmpty()) return

        appStore.dispatch(
            PrivateBrowsingLockAction.UpdatePrivateBrowsingLock(
                isLocked = true,
            ),
        )
    }
}

/**
 * Use cases pertaining to [PrivateBrowsingLockFeature].
 *
 * @param appStore the application's [AppStore].
 */
class PrivateBrowsingLockUseCases(appStore: AppStore) {

    /**
     * Use case to be called at the end of a successful authentication.
     */
    class AuthenticatedUseCase internal constructor(private val appStore: AppStore) {
        /**
         * Handles a successful authentication event by unlocking the private browsing mode.
         *
         * This should be called by biometric or password authentication mechanisms (e.g., fingerprint,
         * face unlock, or PIN entry) once the user has successfully authenticated. It updates the app state
         * to reflect that private browsing tabs are now accessible.
         */
        operator fun invoke() {
            appStore.dispatch(
                PrivateBrowsingLockAction.UpdatePrivateBrowsingLock(isLocked = false),
            )
        }
    }

    val authenticatedUseCase by lazy { AuthenticatedUseCase(appStore) }
}

/**
 * Observes the app state and triggers a callback when the user enters a locked private browsing session.
 *
 * The observer is active in the [Lifecycle.State.RESUMED] state to ensure the UI state is sync with the app state (the
 * app state might be updated while the UI is no longer active.
 *
 * @param viewLifecycleOwner The [LifecycleOwner] used to control when the observation is active.
 * @param scope The [CoroutineScope] in which the coroutine will be launched.
 * @param appStore The [AppStore] to observe the [AppState].
 * @param lockNormalMode If true, the callback will also be triggered in normal mode.
 * @param onPrivateModeLocked A callback invoked when private browsing mode is locked.
 */
fun observePrivateModeLock(
    viewLifecycleOwner: LifecycleOwner,
    scope: CoroutineScope,
    appStore: AppStore,
    lockNormalMode: Boolean = false,
    onPrivateModeLocked: () -> Unit,
) {
    with(viewLifecycleOwner) {
        scope.launch {
            lifecycle.repeatOnLifecycle(RESUMED) {
                appStore.flow()
                    .filter { state ->
                        state.isPrivateScreenLocked && (state.mode == BrowsingMode.Private || lockNormalMode)
                    }
                    .distinctUntilChanged()
                    .collect {
                        onPrivateModeLocked()
                    }
            }
        }
    }
}

/**
 * Registers an [ActivityResultLauncher] that wraps handling of unlocking access to private mode
 * using the pin, pattern or password verification. This should be used in combination with
 * [verifyUser] to authenticate the user when private browsing mode is locked.
 *
 * @param onVerified an optional callback triggered on a successful authentication.
 * @return The configured [ActivityResultLauncher] to handle the pin, pattern or password verification.
 */
fun Fragment.registerForVerification(
    onVerified: (() -> Unit)? = null,
): ActivityResultLauncher<Intent> {
    return registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            handleVerificationSuccess(requireContext(), onVerified)
        } else {
            handleVerificationFailure()
        }
    }
}

/**
 * Triggers user verification to unlock access to private mode.
 *
 * Attempts biometric authentication first; falls back to launching the pin, pattern or password
 * verification. Upon success, records the telemetry and updates [AppState.isPrivateScreenLocked]
 *
 * @param biometricUtils A [BiometricPromptFeature] feature wrapper.
 * @param fallbackVerification The [ActivityResultLauncher] to handle the fallback verification.
 * @param onVerified an optional callback triggered on a successful authentication.
 */
fun Fragment.verifyUser(
    biometricUtils: BiometricUtils = DefaultBiometricUtils,
    fallbackVerification: ActivityResultLauncher<Intent>,
    onVerified: (() -> Unit)? = null,
) {
    biometricUtils.bindBiometricsCredentialsPromptOrShowWarning(
        titleRes = R.string.pbm_authentication_unlock_private_tabs,
        view = requireView(),
        onShowPinVerification = { intent -> fallbackVerification.launch(intent) },
        onAuthSuccess = { handleVerificationSuccess(requireContext(), onVerified) },
        onAuthFailure = ::handleVerificationFailure,
    )
}

private fun handleVerificationSuccess(
    context: Context,
    onVerified: (() -> Unit)? = null,
) {
    PrivateBrowsingLocked.authSuccess.record()
    context.components.useCases.privateBrowsingLockUseCases.authenticatedUseCase()

    onVerified?.invoke()
}

private fun handleVerificationFailure() {
    PrivateBrowsingLocked.authFailure.record()
}
