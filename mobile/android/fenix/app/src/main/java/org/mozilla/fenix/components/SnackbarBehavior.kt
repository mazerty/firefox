/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components

import android.content.Context
import android.view.Gravity
import android.view.View
import androidx.annotation.VisibleForTesting
import androidx.coordinatorlayout.widget.CoordinatorLayout
import androidx.core.view.children
import androidx.core.view.isVisible
import org.mozilla.fenix.R
import org.mozilla.fenix.components.toolbar.ToolbarPosition

/**
 * [CoordinatorLayout.Behavior] to be used by a snackbar that want to ensure it it always positioned
 * such that it will be shown on top (vertically) of other siblings that may obstruct it's view.
 *
 * @param context [Context] used for various system interactions.
 * @property toolbarPosition Where the toolbar is positioned on the screen.
 * Depending on it's position (top / bottom) the snackbar will be shown below / above the toolbar.
 * @param shouldUseExpandedToolbar Whether the expanded toolbar layout should be used.
 */
class SnackbarBehavior<V : View>(
    context: Context,
    @get:VisibleForTesting internal val toolbarPosition: ToolbarPosition,
    private val shouldUseExpandedToolbar: Boolean,
) : CoordinatorLayout.Behavior<V>(context, null) {

    // Priority list of possible anchors for the snackbar.
    private val dependenciesIds = buildList {
        add(R.id.loginSelectBar)
        add(R.id.suggestStrongPasswordBar)
        add(R.id.creditCardSelectBar)
        add(R.id.addressSelectBar)
        add(R.id.navigation_bar)
        if (
            toolbarPosition == ToolbarPosition.BOTTOM ||
            (toolbarPosition == ToolbarPosition.TOP && shouldUseExpandedToolbar)
        ) {
            add(R.id.toolbarLayout)
            add(R.id.toolbar)
            add(R.id.composable_toolbar)
        }
    }

    private var currentAnchorId: Int? = View.NO_ID

    override fun layoutDependsOn(
        parent: CoordinatorLayout,
        child: V,
        dependency: View,
    ): Boolean {
        val anchorId = dependenciesIds
            .intersect(parent.children.filter { it.isVisible }.map { it.id }.toSet())
            .firstOrNull()

        // It is possible that previous anchor's visibility is changed.
        // The layout is updated and layoutDependsOn is called but onDependentViewChanged not.
        // We have to check here if a new anchor is available and reparent the snackbar.
        // This check also ensures we are not positioning the snackbar multiple times for the same anchor.
        return if (anchorId != currentAnchorId) {
            positionSnackbar(child, parent.children.firstOrNull { it.id == anchorId })
            true
        } else {
            false
        }
    }

    private fun positionSnackbar(snackbar: View, dependency: View?) {
        currentAnchorId = dependency?.id ?: View.NO_ID
        val params = snackbar.layoutParams as CoordinatorLayout.LayoutParams

        snackbar.post {
            if (dependency == null) {
                // Position the snackbar at the bottom of the screen.
                params.anchorId = View.NO_ID
                params.anchorGravity = Gravity.NO_GRAVITY
                params.gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
                snackbar.layoutParams = params
            } else {
                // Position the snackbar just above the anchor.
                params.anchorId = dependency.id
                params.anchorGravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
                params.gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
                snackbar.layoutParams = params
            }
        }
    }
}
