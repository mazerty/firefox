/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.home.toolbar

import android.content.Context
import androidx.core.graphics.drawable.toDrawable
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import mozilla.components.browser.state.search.SearchEngine
import mozilla.components.browser.state.state.BrowserState
import mozilla.components.browser.state.state.selectedOrDefaultSearchEngine
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.concept.menu.Orientation
import mozilla.components.lib.state.helpers.AbstractBinding
import mozilla.components.support.ktx.android.content.getColorFromAttr
import mozilla.telemetry.glean.private.NoExtras
import org.mozilla.fenix.GleanMetrics.UnifiedSearch
import org.mozilla.fenix.R
import org.mozilla.fenix.ext.increaseTapAreaVertically
import org.mozilla.fenix.ext.pixelSizeFor
import org.mozilla.fenix.ext.settings
import org.mozilla.fenix.search.toolbar.SearchSelectorMenu

/**
 * A binding that shows the search engine in the search selector button.
 */
internal class SearchSelectorBinding(
    private val context: Context,
    private val toolbarView: HomeToolbarView,
    private val searchSelectorMenu: SearchSelectorMenu,
    browserStore: BrowserStore,
) : AbstractBinding<BrowserState>(browserStore) {

    override fun start() {
        super.start()

        toolbarView.configureSearchSelector {
            setOnClickListener {
                val orientation = if (context.settings().shouldUseBottomToolbar) {
                    Orientation.UP
                } else {
                    Orientation.DOWN
                }

                UnifiedSearch.searchMenuTapped.record(NoExtras())

                searchSelectorMenu.menuController.show(
                    anchor = it.findViewById(R.id.search_selector),
                    orientation = orientation,
                )
            }

            increaseTapAreaVertically(SEARCH_SELECTOR_INCREASE_HEIGHT_DPS)
        }
    }

    override suspend fun onState(flow: Flow<BrowserState>) {
        flow.map { state -> state.search.selectedOrDefaultSearchEngine }
            .distinctUntilChanged()
            .collect { searchEngine ->
                val name = searchEngine?.name
                val icon = searchEngine?.let {
                    val iconSize =
                        context.pixelSizeFor(R.dimen.preference_icon_drawable_size)
                    searchEngine.icon.toDrawable(context.resources).apply {
                        setBounds(0, 0, iconSize, iconSize)
                        // Setting tint manually for icons that were converted from Drawable
                        // to Bitmap. Search Engine icons are stored as Bitmaps, hence
                        // theming/attribute mechanism won't work.
                        if (searchEngine.type == SearchEngine.Type.APPLICATION) {
                            setTint(context.getColorFromAttr(R.attr.textPrimary))
                        }
                    }
                }
                val contentDescription = context.getString(
                    R.string.search_engine_selector_content_description,
                    name ?: "",
                )

                toolbarView.configureSearchSelector { setIcon(icon, contentDescription) }
            }
    }

    companion object {
        const val SEARCH_SELECTOR_INCREASE_HEIGHT_DPS = 10
    }
}
