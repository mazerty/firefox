/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.focus.fragment.onboarding

import android.content.Context
import android.os.Bundle
import android.transition.TransitionInflater
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.ViewCompositionStrategy
import androidx.fragment.app.Fragment
import mozilla.components.browser.state.state.ExternalAppType
import mozilla.telemetry.glean.private.NoExtras
import org.mozilla.focus.GleanMetrics.Onboarding
import org.mozilla.focus.R
import org.mozilla.focus.ext.requireComponents
import org.mozilla.focus.ui.theme.FocusTheme
import org.mozilla.focus.utils.SupportUtils

class OnboardingFirstFragment : Fragment() {
    private lateinit var onboardingInteractor: OnboardingInteractor

    private val termsOfServiceUrl by lazy {
        SupportUtils.getMozillaPageUrl(SupportUtils.MozillaPage.TERMS_OF_SERVICE)
    }
    private val privacyNoticeUrl by lazy {
        SupportUtils.getMozillaPageUrl(SupportUtils.MozillaPage.PRIVATE_NOTICE)
    }

    private fun openLearnMore(url: String) {
        SupportUtils.openUrlInCustomTab(
            activity = requireActivity(),
            destinationUrl = url,
            externalAppType = ExternalAppType.ONBOARDING_CUSTOM_TAB,
        )
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
                OnBoardingFirstScreenCompose(
                    termsOfServiceOnClick = { openLearnMore(termsOfServiceUrl) },
                    privacyNoticeOnClick = { openLearnMore(privacyNoticeUrl) },
                    buttonOnClick = {
                        Onboarding.getStartedButton.record(NoExtras())
                        onboardingInteractor.onGetStartedButtonClicked()
                    },
                )
            }
        }
    }
}
