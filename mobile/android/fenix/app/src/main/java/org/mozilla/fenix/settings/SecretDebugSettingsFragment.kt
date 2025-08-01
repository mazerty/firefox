/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.settings

import android.os.Bundle
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.fragment.app.Fragment
import androidx.fragment.compose.content
import mozilla.components.browser.state.search.RegionState
import mozilla.components.lib.state.ext.observeAsState
import org.mozilla.fenix.R
import org.mozilla.fenix.components.components
import org.mozilla.fenix.distributions.DefaultDistributionProviderChecker
import org.mozilla.fenix.distributions.LegacyDistributionProviderChecker
import org.mozilla.fenix.ext.showToolbar
import org.mozilla.fenix.theme.FirefoxTheme

class SecretDebugSettingsFragment : Fragment() {

    override fun onResume() {
        super.onResume()

        showToolbar(getString(R.string.preferences_debug_info))
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ) = content {
        FirefoxTheme {
            SecretDebugSettingsScreen()
        }
    }
}

@Composable
private fun SecretDebugSettingsScreen() {
    val regionState: RegionState by components.core.store.observeAsState(
        initialValue = RegionState.Default,
        map = { it.search.region ?: RegionState.Default },
    )

    val distributionId: String by components.core.store.observeAsState(
        initialValue = "",
        map = { it.distributionId ?: "" },
    )

    val settings = components.settings

    val playInstallReferrer: String by remember {
        mutableStateOf(
            """
                utmTerm: ${settings.utmTerm}
                utmMedium: ${settings.utmMedium}
                utmSource: ${settings.utmSource}
                utmContent: ${settings.utmContent}
                utmCampaign: ${settings.utmCampaign}
            """.trimIndent(),
        )
    }

    DebugInfo(
        regionState = regionState,
        distributionId = distributionId,
        playInstallReferrer = playInstallReferrer,
    )
}

@Composable
private fun DebugInfo(
    regionState: RegionState,
    distributionId: String,
    playInstallReferrer: String,
) {
    Column(
        modifier = Modifier
            .padding(8.dp),
    ) {
        Text(
            text = stringResource(R.string.debug_info_region_home),
            color = FirefoxTheme.colors.textPrimary,
            style = FirefoxTheme.typography.headline6,
            modifier = Modifier.padding(4.dp),
        )
        Text(
            text = regionState.home,
            color = FirefoxTheme.colors.textPrimary,
            modifier = Modifier.padding(4.dp),
        )
        Text(
            text = stringResource(R.string.debug_info_region_current),
            color = FirefoxTheme.colors.textPrimary,
            style = FirefoxTheme.typography.headline6,
            modifier = Modifier.padding(4.dp),
        )
        Text(
            text = regionState.current,
            color = FirefoxTheme.colors.textPrimary,
            modifier = Modifier.padding(4.dp),
        )

        Text(
            text = stringResource(R.string.debug_info_distribution_id),
            color = FirefoxTheme.colors.textPrimary,
            style = FirefoxTheme.typography.headline6,
            modifier = Modifier.padding(4.dp),
        )
        Text(
            text = distributionId,
            color = FirefoxTheme.colors.textPrimary,
            modifier = Modifier.padding(4.dp),
        )

        Text(
            text = stringResource(R.string.debug_info_play_referrer),
            color = FirefoxTheme.colors.textPrimary,
            style = FirefoxTheme.typography.headline6,
            modifier = Modifier.padding(4.dp),
        )
        Text(
            text = playInstallReferrer,
            color = FirefoxTheme.colors.textPrimary,
            modifier = Modifier.padding(4.dp),
        )

        val context = LocalContext.current

        Button(
            onClick = {
                DefaultDistributionProviderChecker(context).queryProvider()
                LegacyDistributionProviderChecker(context).queryProvider()
            },
        ) {
            Text(
                text = stringResource(R.string.debug_info_run_query_provider_test),
                color = FirefoxTheme.colors.textOnColorPrimary,
            )
        }
    }
}
