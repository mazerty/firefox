/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.perf

import android.os.Bundle
import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.Toast
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.activityViewModels
import androidx.fragment.compose.content
import org.mozilla.fenix.R
import org.mozilla.fenix.ext.components
import org.mozilla.fenix.perf.ProfilerUtils.handleProfileSave

/**
 * Dialogue to stop the Gecko profiler without using ADB.
 */
class ProfilerStopDialogFragment : DialogFragment() {

    private val profilerViewModel: ProfilerViewModel by activityViewModels()

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ) = content {
        StopProfilerCard()
    }

    private fun setProfilerState() {
        profilerViewModel.setProfilerState(requireContext().components.core.engine.profiler!!.isProfilerActive())
    }

    @Composable
    private fun StopProfilerCard() {
        val viewStateObserver = remember { mutableStateOf(CardState.UrlWarningState) }
        Dialog(
            onDismissRequest = {
                // In the waiting state, we do not want the users to be able to click away from the dialogue
                // since the user needs to wait for the profiler data to be ready and we don't want to handle
                // the process in the background.
                if (viewStateObserver.value != CardState.WaitForProfilerGathering &&
                    viewStateObserver.value != CardState.WaitForProfilerStop
                ) {
                    this@ProfilerStopDialogFragment.dismiss()
                }
            },
        ) {
            when (viewStateObserver.value) {
                CardState.UrlWarningState -> {
                    UrlWarningCard(viewStateObserver)
                }
                CardState.WaitForProfilerGathering -> {
                    WaitForProfilerDialog(R.string.profiler_gathering)
                }
                CardState.WaitForProfilerStop -> {
                    WaitForProfilerDialog(R.string.profiler_stopping)
                }
            }
        }
    }

    @Composable
    private fun UrlWarningCard(
        viewStateObserver: MutableState<CardState>,
    ) {
        ProfilerDialogueCard {
            Column(modifier = Modifier.padding(8.dp)) {
                Text(
                    text = stringResource(R.string.profiler_url_warning),
                    fontWeight = FontWeight.ExtraBold,
                    fontSize = 20.sp,
                    modifier = Modifier.padding(8.dp),
                )
                Spacer(modifier = Modifier.height(10.dp))
                Text(
                    text = stringResource(R.string.profiler_url_warning_explained),
                    fontWeight = FontWeight.Medium,
                    fontSize = 15.sp,
                    modifier = Modifier.padding(8.dp),
                )
                Spacer(modifier = Modifier.height(30.dp))
                Row(
                    horizontalArrangement = Arrangement.End,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    TextButton(
                        onClick = {
                            viewStateObserver.value = CardState.WaitForProfilerStop
                            requireContext().components.core.engine.profiler?.stopProfiler(
                                onSuccess = {
                                    setProfilerState()
                                    dismiss()
                                },
                                onError = {
                                    setProfilerState()
                                    dismiss()
                                },
                            )
                        },
                    ) {
                        Text(stringResource(R.string.profiler_start_cancel))
                    }
                    Spacer(modifier = Modifier.width(4.dp))

                    TextButton(
                        onClick = {
                            viewStateObserver.value = CardState.WaitForProfilerGathering
                            stopProfiler()
                        },
                    ) {
                        Text(stringResource(R.string.profiler_as_url))
                    }
                }
            }
        }
    }

    private fun stopProfiler() {
        requireContext().components.core.engine.profiler!!.stopProfiler(
            onSuccess = {
                if (it != null) {
                    handleProfileSave(
                        requireContext(),
                        it,
                        ::displayToastAndDismiss,
                    )
                } else {
                    displayToastAndDismiss(R.string.profiler_no_info)
                }
            },
            onError = { error ->
                if (error.message != null) {
                    displayToastAndDismiss(R.string.profiler_error, " error: $error")
                } else {
                    displayToastAndDismiss(R.string.profiler_error)
                }
            },
        )
    }

    private fun displayToastAndDismiss(@StringRes message: Int, extra: String = "") {
        Toast.makeText(
            context,
            resources.getString(message) + extra,
            Toast.LENGTH_LONG,
        ).show()
        setProfilerState()
        dismiss()
    }

    /**
     * State that represents which card to display within the Stop dialogue.
     */
    enum class CardState {
        UrlWarningState,
        WaitForProfilerGathering,
        WaitForProfilerStop,
    }
}
