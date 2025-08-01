/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.webcompat.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.error
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.semantics.testTagsAsResourceId
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.tooling.preview.PreviewLightDark
import androidx.compose.ui.tooling.preview.PreviewParameter
import androidx.compose.ui.tooling.preview.PreviewParameterProvider
import androidx.compose.ui.unit.dp
import mozilla.components.compose.base.Dropdown
import mozilla.components.compose.base.button.PrimaryButton
import mozilla.components.compose.base.button.TextButton
import mozilla.components.compose.base.menu.MenuItem
import mozilla.components.compose.base.modifier.thenConditional
import mozilla.components.compose.base.text.Text.Resource
import mozilla.components.compose.base.textfield.TextField
import mozilla.components.compose.base.textfield.TextFieldColors
import mozilla.components.lib.state.ext.observeAsState
import org.mozilla.fenix.Config
import org.mozilla.fenix.R
import org.mozilla.fenix.compose.LinkText
import org.mozilla.fenix.compose.LinkTextState
import org.mozilla.fenix.theme.FirefoxTheme
import org.mozilla.fenix.webcompat.BrokenSiteReporterTestTags.BROKEN_SITE_REPORTER_CHOOSE_REASON_BUTTON
import org.mozilla.fenix.webcompat.BrokenSiteReporterTestTags.BROKEN_SITE_REPORTER_SEND_BUTTON
import org.mozilla.fenix.webcompat.store.WebCompatReporterAction
import org.mozilla.fenix.webcompat.store.WebCompatReporterState
import org.mozilla.fenix.webcompat.store.WebCompatReporterState.BrokenSiteReason
import org.mozilla.fenix.webcompat.store.WebCompatReporterStore

private const val PROBLEM_DESCRIPTION_MAX_LINES = 5

/**
 * Top-level UI for the Web Compat Reporter feature.
 *
 * @param store [WebCompatReporterStore] used to manage the state of the Web Compat Reporter feature.
 */
@OptIn(ExperimentalComposeUiApi::class)
@Suppress("LongMethod")
@Composable
fun WebCompatReporter(
    store: WebCompatReporterStore,
) {
    val state by store.observeAsState(store.state) { it }

    BackHandler {
        store.dispatch(WebCompatReporterAction.BackPressed)
    }

    Scaffold(
        topBar = {
            TempAppBar(
                onBackClick = {
                    store.dispatch(WebCompatReporterAction.BackPressed)
                },
            )
        },
        containerColor = FirefoxTheme.colors.layer2,
    ) { paddingValues ->
        Column(
            modifier = Modifier.verticalScroll(rememberScrollState())
                .padding(paddingValues)
                .imePadding()
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            LinkText(
                text = stringResource(
                    R.string.webcompat_reporter_description_3,
                    stringResource(R.string.app_name),
                    stringResource(R.string.webcompat_reporter_learn_more),
                ),
                linkTextStates = listOf(
                    LinkTextState(
                        text = stringResource(R.string.webcompat_reporter_learn_more),
                        url = "",
                        onClick = {
                            store.dispatch(WebCompatReporterAction.LearnMoreClicked)
                        },
                    ),
                ),
                style = FirefoxTheme.typography.body2.copy(color = FirefoxTheme.colors.textPrimary),
                linkTextColor = FirefoxTheme.colors.textAccent,
                linkTextDecoration = TextDecoration.Underline,
                textAlign = TextAlign.Start,
            )

            Spacer(modifier = Modifier.height(32.dp))

            TextField(
                value = state.enteredUrl,
                onValueChange = {
                    store.dispatch(WebCompatReporterAction.BrokenSiteChanged(newUrl = it))
                },
                placeholder = "",
                errorText = stringResource(id = R.string.webcompat_reporter_url_error_invalid),
                modifier = Modifier.fillMaxWidth(),
                label = stringResource(id = R.string.webcompat_reporter_label_url),
                isError = state.hasUrlTextError,
                singleLine = true,
            )

            Spacer(modifier = Modifier.height(16.dp))

            val reasonErrorText = stringResource(R.string.webcompat_reporter_choose_reason_error)

            Dropdown(
                label = stringResource(id = R.string.webcompat_reporter_label_whats_broken_2),
                placeholder = stringResource(id = R.string.webcompat_reporter_choose_reason_2),
                dropdownItems = state.toDropdownItems(
                    onDropdownItemClick = {
                        store.dispatch(WebCompatReporterAction.ReasonChanged(newReason = it))
                    },
                ),
                modifier = Modifier.thenConditional(
                    modifier = Modifier.semantics { error(reasonErrorText) },
                ) { state.hasReasonDropdownError },
            )

            if (state.hasReasonDropdownError) {
                Spacer(modifier = Modifier.height(4.dp))

                Text(
                    text = reasonErrorText,
                    // The a11y for this is handled via the `Dropdown` modifier
                    modifier = Modifier.clearAndSetSemantics {
                        testTagsAsResourceId = true
                        testTag = BROKEN_SITE_REPORTER_CHOOSE_REASON_BUTTON
                    },
                    style = FirefoxTheme.typography.caption,
                    color = FirefoxTheme.colors.textCritical,
                )
            }

            Spacer(modifier = Modifier.height(16.dp))

            TextField(
                value = state.problemDescription,
                onValueChange = {
                    store.dispatch(WebCompatReporterAction.ProblemDescriptionChanged(newProblemDescription = it))
                },
                placeholder = stringResource(id = R.string.webcompat_reporter_problem_description_placeholder_text),
                errorText = "",
                label = stringResource(id = R.string.webcompat_reporter_label_description),
                singleLine = false,
                maxLines = PROBLEM_DESCRIPTION_MAX_LINES,
                colors = TextFieldColors.default(
                    inputColor = FirefoxTheme.colors.textSecondary,
                ),
            )

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (Config.channel.isBeta || Config.channel.isNightlyOrDebug) {
                    Text(
                        text = stringResource(id = R.string.webcompat_reporter_send_more_info),
                        modifier = Modifier
                            .clickable {
                                store.dispatch(WebCompatReporterAction.SendMoreInfoClicked)
                            },
                        style = FirefoxTheme.typography.body2,
                        color = FirefoxTheme.colors.textAccent,
                        textDecoration = TextDecoration.Underline,
                    )

                    Spacer(modifier = Modifier.width(24.dp))
                }

                Row(
                    modifier = Modifier.weight(1f),
                    horizontalArrangement = Arrangement.End,
                ) {
                    TextButton(
                        text = stringResource(id = R.string.webcompat_reporter_cancel),
                        onClick = {
                            store.dispatch(WebCompatReporterAction.CancelClicked)
                        },
                        upperCaseText = false,
                    )

                    Spacer(modifier = Modifier.width(10.dp))

                    PrimaryButton(
                        text = stringResource(id = R.string.webcompat_reporter_send),
                        modifier = Modifier
                            .wrapContentSize()
                            .semantics {
                                testTagsAsResourceId = true
                                testTag = BROKEN_SITE_REPORTER_SEND_BUTTON
                            },
                        enabled = state.isSubmitEnabled,
                    ) {
                        store.dispatch(WebCompatReporterAction.SendReportClicked)
                    }
                }
            }
        }
    }
}

/**
 * Helper function used to obtain the list of dropdown menu items derived from [BrokenSiteReason].
 *
 * @param onDropdownItemClick Callback invoked when the particular dropdown item is selected.
 * @return The list of [MenuItem.CheckableItem] to display in the dropdown.
 */
private fun WebCompatReporterState.toDropdownItems(
    onDropdownItemClick: (BrokenSiteReason) -> Unit,
): List<MenuItem.CheckableItem> {
    return BrokenSiteReason.entries.map { reason ->
        MenuItem.CheckableItem(
            text = Resource(reason.displayStringId),
            isChecked = this.reason == reason,
            onClick = {
                onDropdownItemClick(reason)
            },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TempAppBar(
    onBackClick: () -> Unit,
) {
    TopAppBar(
        colors = TopAppBarDefaults.topAppBarColors(containerColor = FirefoxTheme.colors.layer1),
        title = {
            Text(
                text = stringResource(id = R.string.webcompat_reporter_screen_title),
                color = FirefoxTheme.colors.textPrimary,
                style = FirefoxTheme.typography.headline6,
            )
        },
        navigationIcon = {
            IconButton(onClick = onBackClick) {
                Icon(
                    painter = painterResource(R.drawable.mozac_ic_back_24),
                    contentDescription = stringResource(R.string.bookmark_navigate_back_button_content_description),
                    tint = FirefoxTheme.colors.iconPrimary,
                )
            }
        },
        windowInsets = WindowInsets(
            top = 0.dp,
            bottom = 0.dp,
        ),
    )
}

private class WebCompatPreviewParameterProvider : PreviewParameterProvider<WebCompatReporterState> {
    override val values: Sequence<WebCompatReporterState>
        get() = sequenceOf(
            // Initial feature opening
            WebCompatReporterState(
                enteredUrl = "www.example.com/url_parameters_that_break_the_page",
            ),
            // Error in URL field
            WebCompatReporterState(
                enteredUrl = "",
            ),
            // Multi-line description
            WebCompatReporterState(
                enteredUrl = "www.example.com/url_parameters_that_break_the_page",
                reason = BrokenSiteReason.Slow,
                problemDescription = "The site wouldn’t load and after I tried xyz it still wouldn’t " +
                    "load and then again site wouldn’t load and after I tried xyz it still wouldn’t " +
                    "load and then again site wouldn’t load and after I tried xyz it still wouldn’t " +
                    "load and then again site wouldn’t load and after I tried xyz it still wouldn’t " +
                    "load and then again site wouldn’t load and after I tried xyz it still wouldn’t " +
                    "load and then again ",
            ),
        )
}

@PreviewLightDark
@Composable
private fun WebCompatReporterPreview(
    @PreviewParameter(WebCompatPreviewParameterProvider::class) initialState: WebCompatReporterState,
) {
    FirefoxTheme {
        WebCompatReporter(
            store = WebCompatReporterStore(
                initialState = initialState,
            ),
        )
    }
}
