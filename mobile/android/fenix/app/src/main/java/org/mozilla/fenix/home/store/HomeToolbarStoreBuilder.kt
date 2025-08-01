/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.home.store

import android.content.Context
import androidx.fragment.app.Fragment
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.navigation.NavController
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.compose.browser.toolbar.store.BrowserToolbarState
import mozilla.components.compose.browser.toolbar.store.BrowserToolbarStore
import mozilla.components.compose.browser.toolbar.store.EnvironmentCleared
import mozilla.components.compose.browser.toolbar.store.EnvironmentRehydrated
import org.mozilla.fenix.browser.browsingmode.BrowsingModeManager
import org.mozilla.fenix.components.AppStore
import org.mozilla.fenix.components.StoreProvider
import org.mozilla.fenix.components.toolbar.BrowserToolbarEnvironment
import org.mozilla.fenix.ext.components
import org.mozilla.fenix.home.toolbar.BrowserToolbarMiddleware
import org.mozilla.fenix.home.toolbar.BrowserToolbarTelemetryMiddleware
import org.mozilla.fenix.search.BrowserToolbarSearchMiddleware
import org.mozilla.fenix.search.BrowserToolbarSearchStatusSyncMiddleware

/**
 * Delegate for building the [BrowserToolbarStore] used in the home screen.
 */
object HomeToolbarStoreBuilder {
    /**
     * Build the [BrowserToolbarStore] used in the home screen.
     *
     * @param context [Context] used for various system interactions.
     * @param lifecycleOwner [Fragment] as a [LifecycleOwner] to used to organize lifecycle dependent operations.
     * @param navController [NavController] to use for navigating to other in-app destinations.
     * @param appStore [AppStore] to sync from.
     * @param browserStore [BrowserStore] to sync from.
     * @param browsingModeManager [BrowsingModeManager] for querying the current browsing mode.
     */
    fun build(
        context: Context,
        lifecycleOwner: Fragment,
        navController: NavController,
        appStore: AppStore,
        browserStore: BrowserStore,
        browsingModeManager: BrowsingModeManager,
    ) = StoreProvider.get(lifecycleOwner) {
        BrowserToolbarStore(
            initialState = BrowserToolbarState(),
            middleware = listOf(
                BrowserToolbarSearchStatusSyncMiddleware(appStore),
                BrowserToolbarMiddleware(
                    appStore = appStore,
                    browserStore = browserStore,
                    clipboard = context.components.clipboardHandler,
                    useCases = context.components.useCases,
                ),
                BrowserToolbarSearchMiddleware(
                    appStore = appStore,
                    browserStore = browserStore,
                    components = context.components,
                    settings = context.components.settings,
                ),
                BrowserToolbarTelemetryMiddleware(),
            ),
        )
    }.also {
        it.dispatch(
            EnvironmentRehydrated(
                BrowserToolbarEnvironment(
                    context = context,
                    viewLifecycleOwner = lifecycleOwner.viewLifecycleOwner,
                    navController = navController,
                    browsingModeManager = browsingModeManager,
                ),
            ),
        )

        lifecycleOwner.viewLifecycleOwner.lifecycle.addObserver(
            object : DefaultLifecycleObserver {
                override fun onDestroy(owner: LifecycleOwner) {
                    it.dispatch(EnvironmentCleared)
                }
            },
        )
    }
}
