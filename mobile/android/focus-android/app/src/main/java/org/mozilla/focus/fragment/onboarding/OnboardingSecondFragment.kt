/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.focus.fragment.onboarding

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.transition.TransitionInflater
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.ViewCompositionStrategy
import androidx.fragment.app.Fragment
import mozilla.components.support.utils.Browsers
import mozilla.telemetry.glean.private.NoExtras
import org.mozilla.focus.GleanMetrics.Onboarding
import org.mozilla.focus.R
import org.mozilla.focus.ext.requireComponents
import org.mozilla.focus.ui.theme.FocusTheme

class OnboardingSecondFragment : Fragment() {
    private lateinit var onboardingInteractor: OnboardingInteractor

    private var activityResultLauncher: ActivityResultLauncher<Intent> = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) {
        onboardingInteractor.onActivityResultImplementation(it)
    }

    override fun onAttach(context: Context) {
        super.onAttach(context)
        val transition =
            TransitionInflater.from(context).inflateTransition(R.transition.firstrun_exit)
        exitTransition = transition
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View {
        onboardingInteractor = DefaultOnboardingInteractor(
            DefaultOnboardingController(
                onboardingStorage = OnboardingStorage(requireContext()),
                appStore = requireComponents.appStore,
                context = requireActivity(),
                selectedTabId = requireComponents.store.state.selectedTabId,
            ),
        )
        return ComposeView(requireContext()).apply {
            isTransitionGroup = true
            setViewCompositionStrategy(ViewCompositionStrategy.DisposeOnViewTreeLifecycleDestroyed)
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        (view as ComposeView).setContent {
            FocusTheme {
                OnBoardingSecondScreenCompose(
                    setAsDefaultBrowser = {
                        Onboarding.defaultBrowserButton.record(NoExtras())
                        onboardingInteractor.onMakeFocusDefaultBrowserButtonClicked(activityResultLauncher)
                    },
                    skipScreen = {
                        Onboarding.skipButton.record(NoExtras())
                        onboardingInteractor.onFinishOnBoarding()
                    },
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // check if the default browser was changed from OS settings for devices with Android 7,8 and 9.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N &&
            Build.VERSION.SDK_INT < Build.VERSION_CODES.Q &&
            Browsers.all(requireContext()).isDefaultBrowser
        ) {
            onboardingInteractor.onFinishOnBoarding()
        }
    }
}
