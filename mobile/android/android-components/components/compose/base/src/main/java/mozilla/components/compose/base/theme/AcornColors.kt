/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

@file:Suppress("MagicNumber")

package mozilla.components.compose.base.theme

import androidx.compose.material3.ColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.Color
import mozilla.components.ui.colors.PhotonColors

/**
 * A custom Color Palette for Mozilla Firefox for Android (Fenix).
 */
@Suppress("LongParameterList")
@Stable
class AcornColors(
    layer1: Color,
    layer2: Color,
    layer3: Color,
    layer4Start: Color,
    layer4Center: Color,
    layer4End: Color,
    layerAccent: Color,
    layerAccentNonOpaque: Color,
    layerAccentOpaque: Color,
    layerScrim: Color,
    layerGradientStart: Color,
    layerGradientEnd: Color,
    layerWarning: Color,
    layerSuccess: Color,
    layerCritical: Color,
    layerInformation: Color,
    layerSearch: Color,
    layerAutofillText: Color,
    actionPrimary: Color,
    actionPrimaryDisabled: Color,
    actionSecondary: Color,
    actionTertiary: Color,
    actionQuarternary: Color,
    actionWarning: Color,
    actionSuccess: Color,
    actionCritical: Color,
    actionInformation: Color,
    actionChipSelected: Color,
    formDefault: Color,
    formSelected: Color,
    formSurface: Color,
    formDisabled: Color,
    formOn: Color,
    formOff: Color,
    indicatorActive: Color,
    indicatorInactive: Color,
    textPrimary: Color,
    textSecondary: Color,
    textDisabled: Color,
    textCritical: Color,
    textCriticalButton: Color,
    textAccent: Color,
    textAccentDisabled: Color,
    textInverted: Color,
    textOnColorPrimary: Color,
    textOnColorSecondary: Color,
    textActionPrimary: Color,
    textActionPrimaryDisabled: Color,
    textActionSecondary: Color,
    textActionTertiary: Color,
    textActionTertiaryActive: Color,
    iconPrimary: Color,
    iconPrimaryInactive: Color,
    iconSecondary: Color,
    iconActive: Color,
    iconDisabled: Color,
    iconOnColor: Color,
    iconOnColorDisabled: Color,
    iconNotice: Color,
    iconButton: Color,
    iconCritical: Color,
    iconCriticalButton: Color,
    iconAccentViolet: Color,
    iconAccentBlue: Color,
    iconAccentPink: Color,
    iconAccentGreen: Color,
    iconAccentYellow: Color,
    iconActionPrimary: Color,
    iconActionSecondary: Color,
    iconActionTertiary: Color,
    iconGradientStart: Color,
    iconGradientEnd: Color,
    borderPrimary: Color,
    borderSecondary: Color,
    borderInverted: Color,
    borderFormDefault: Color,
    borderAccent: Color,
    borderDisabled: Color,
    borderCritical: Color,
    borderToolbarDivider: Color,
    ripple: Color,
    tabActive: Color,
    tabInactive: Color,
    badgeActive: Color,
    surfaceDimVariant: Color,
) {
    // Layers

    // Default Screen background, Frontlayer background, App Bar Top, App Bar Bottom, Frontlayer header
    var layer1 by mutableStateOf(layer1)
        private set

    // Card background, Menu background, Dialog, Banner
    var layer2 by mutableStateOf(layer2)
        private set

    // Search
    var layer3 by mutableStateOf(layer3)
        private set

    // Homescreen background, Toolbar
    var layer4Start by mutableStateOf(layer4Start)
        private set

    // Homescreen background, Toolbar
    var layer4Center by mutableStateOf(layer4Center)
        private set

    // Homescreen background, Toolbar
    var layer4End by mutableStateOf(layer4End)
        private set

    // App Bar Top (edit), Text Cursor, Selected Tab Check
    var layerAccent by mutableStateOf(layerAccent)
        private set

    // Selected tab
    var layerAccentNonOpaque by mutableStateOf(layerAccentNonOpaque)
        private set

    // Selected tab
    var layerAccentOpaque by mutableStateOf(layerAccentOpaque)
        private set

    var layerScrim by mutableStateOf(layerScrim)
        private set

    // Tooltip
    var layerGradientStart by mutableStateOf(layerGradientStart)
        private set

    // Tooltip
    var layerGradientEnd by mutableStateOf(layerGradientEnd)
        private set

    // Warning background
    var layerWarning by mutableStateOf(layerWarning)
        private set

    // Confirmation background
    var layerSuccess by mutableStateOf(layerSuccess)
        private set

    // Error Background
    var layerCritical by mutableStateOf(layerCritical)
        private set

    // Info background
    var layerInformation by mutableStateOf(layerInformation)
        private set

    // Search
    var layerSearch by mutableStateOf(layerSearch)
        private set

    // Autofill text background
    // Use for the background of autofill text in the address bar.
    var layerAutofillText by mutableStateOf(layerAutofillText)
        private set

    // Actions

    // Primary button, Snackbar, Floating action button, Chip selected
    var actionPrimary by mutableStateOf(actionPrimary)
        private set

    // Primary disabled button background
    var actionPrimaryDisabled by mutableStateOf(actionPrimaryDisabled)
        private set

    // Secondary button
    var actionSecondary by mutableStateOf(actionSecondary)
        private set

    // Filter
    var actionTertiary by mutableStateOf(actionTertiary)
        private set

    // Chip
    var actionQuarternary by mutableStateOf(actionQuarternary)
        private set

    // Warning button
    var actionWarning by mutableStateOf(actionWarning)
        private set

    // Confirmation button
    var actionSuccess by mutableStateOf(actionSuccess)
        private set

    // Error button
    var actionCritical by mutableStateOf(actionCritical)
        private set

    // Info button
    var actionInformation by mutableStateOf(actionInformation)
        private set

    // Checkbox default, Radio button default
    var formDefault by mutableStateOf(formDefault)
        private set

    // Checkbox selected, Radio button selected
    var formSelected by mutableStateOf(formSelected)
        private set

    // Switch background OFF, Switch background ON
    var formSurface by mutableStateOf(formSurface)
        private set

    // Checkbox disabled, Radio disabled
    var formDisabled by mutableStateOf(formDisabled)
        private set

    // Switch thumb ON
    var formOn by mutableStateOf(formOn)
        private set

    // Switch thumb OFF
    var formOff by mutableStateOf(formOff)
        private set

    // Scroll indicator active
    var indicatorActive by mutableStateOf(indicatorActive)
        private set

    // Scroll indicator inactive
    var indicatorInactive by mutableStateOf(indicatorInactive)
        private set

    // Selected chip
    var actionChipSelected by mutableStateOf(actionChipSelected)
        private set

    // Text

    // Primary text
    var textPrimary by mutableStateOf(textPrimary)
        private set

    // Secondary text
    var textSecondary by mutableStateOf(textSecondary)
        private set

    // Disabled text
    var textDisabled by mutableStateOf(textDisabled)
        private set

    // Warning text
    var textCritical by mutableStateOf(textCritical)
        private set

    // Warning text on Secondary button
    var textCriticalButton by mutableStateOf(textCriticalButton)
        private set

    // Small heading, Text link
    var textAccent by mutableStateOf(textAccent)
        private set

    // Small heading, Text link
    var textAccentDisabled by mutableStateOf(textAccentDisabled)
        private set

    // Text Inverted
    var textInverted by mutableStateOf(textInverted)
        private set

    // Text Inverted/On Color
    var textOnColorPrimary by mutableStateOf(textOnColorPrimary)
        private set

    // Text Inverted/On Color
    var textOnColorSecondary by mutableStateOf(textOnColorSecondary)
        private set

    // Action Primary text
    var textActionPrimary by mutableStateOf(textActionPrimary)
        private set

    // Disabled Action Primary text
    var textActionPrimaryDisabled by mutableStateOf(textActionPrimaryDisabled)
        private set

    // Action Secondary text
    var textActionSecondary by mutableStateOf(textActionSecondary)
        private set

    // Action Tertiary text
    var textActionTertiary by mutableStateOf(textActionTertiary)
        private set

    // Action Tertiary Active text
    var textActionTertiaryActive by mutableStateOf(textActionTertiaryActive)
        private set

    // Icon

    // Primary icon
    var iconPrimary by mutableStateOf(iconPrimary)
        private set

    // Inactive tab
    var iconPrimaryInactive by mutableStateOf(iconPrimaryInactive)
        private set

    // Secondary icon
    var iconSecondary by mutableStateOf(iconSecondary)
        private set

    // Active tab
    var iconActive by mutableStateOf(iconActive)
        private set

    // Disabled icon
    var iconDisabled by mutableStateOf(iconDisabled)
        private set

    // Icon inverted (on color)
    var iconOnColor by mutableStateOf(iconOnColor)
        private set

    // Disabled icon inverted (on color)
    var iconOnColorDisabled by mutableStateOf(iconOnColorDisabled)
        private set

    // New
    var iconNotice by mutableStateOf(iconNotice)
        private set

    // Icon button
    var iconButton by mutableStateOf(iconButton)
        private set
    var iconCritical by mutableStateOf(iconCritical)
        private set

    // Warning icon on Secondary button
    var iconCriticalButton by mutableStateOf(iconCriticalButton)
        private set
    var iconAccentViolet by mutableStateOf(iconAccentViolet)
        private set
    var iconAccentBlue by mutableStateOf(iconAccentBlue)
        private set
    var iconAccentPink by mutableStateOf(iconAccentPink)
        private set
    var iconAccentGreen by mutableStateOf(iconAccentGreen)
        private set
    var iconAccentYellow by mutableStateOf(iconAccentYellow)
        private set

    // Action primary icon
    var iconActionPrimary by mutableStateOf(iconActionPrimary)
        private set

    // Action secondary icon
    var iconActionSecondary by mutableStateOf(iconActionSecondary)
        private set

    // Action tertiary icon
    var iconActionTertiary by mutableStateOf(iconActionTertiary)
        private set

    // Reader, ETP Shield
    var iconGradientStart by mutableStateOf(iconGradientStart)
        private set

    // Reader, ETP Shield
    var iconGradientEnd by mutableStateOf(iconGradientEnd)
        private set

    // Border

    // Default, Divider, Dotted
    var borderPrimary by mutableStateOf(borderPrimary)
        private set

    var borderSecondary by mutableStateOf(borderSecondary)
        private set

    // Onboarding
    var borderInverted by mutableStateOf(borderInverted)
        private set

    // Form parts
    var borderFormDefault by mutableStateOf(borderFormDefault)
        private set

    // Active tab (Nav), Selected tab, Active form
    var borderAccent by mutableStateOf(borderAccent)
        private set

    // Form parts
    var borderDisabled by mutableStateOf(borderDisabled)
        private set

    // Form parts
    var borderCritical by mutableStateOf(borderCritical)
        private set

    // Toolbar divider
    var borderToolbarDivider by mutableStateOf(borderToolbarDivider)
        private set

    var ripple by mutableStateOf(ripple)
        private set

    // Tab Active
    var tabActive by mutableStateOf(tabActive)
        private set

    // Tab Inactive
    var tabInactive by mutableStateOf(tabInactive)
        private set

    // Badge Active
    var badgeActive by mutableStateOf(badgeActive)
        private set

    /*
     * M3 color scheme extensions that do not have a mapped value from Acorn
     */

    /**
     * Surface Dim Variant
     *
     * Slightly dimmer surface color in light theme.
     */
    var surfaceDimVariant by mutableStateOf(surfaceDimVariant)
        private set

    /**
     * Updates the existing colors with the provided [AcornColors].
     */
    @Suppress("LongMethod")
    fun update(other: AcornColors) {
        layer1 = other.layer1
        layer2 = other.layer2
        layer3 = other.layer3
        layer4Start = other.layer4Start
        layer4Center = other.layer4Center
        layer4End = other.layer4End
        layerAccent = other.layerAccent
        layerAccentNonOpaque = other.layerAccentNonOpaque
        layerAccentOpaque = other.layerAccentOpaque
        layerScrim = other.layerScrim
        layerGradientStart = other.layerGradientStart
        layerGradientEnd = other.layerGradientEnd
        layerWarning = other.layerWarning
        layerSuccess = other.layerSuccess
        layerCritical = other.layerCritical
        layerInformation = other.layerInformation
        layerSearch = other.layerSearch
        actionPrimary = other.actionPrimary
        actionPrimaryDisabled = other.actionPrimaryDisabled
        actionSecondary = other.actionSecondary
        actionTertiary = other.actionTertiary
        actionQuarternary = other.actionQuarternary
        actionWarning = other.actionWarning
        actionSuccess = other.actionSuccess
        actionCritical = other.actionCritical
        actionInformation = other.actionInformation
        formDefault = other.formDefault
        formSelected = other.formSelected
        formSurface = other.formSurface
        formDisabled = other.formDisabled
        formOn = other.formOn
        formOff = other.formOff
        indicatorActive = other.indicatorActive
        indicatorInactive = other.indicatorInactive
        textPrimary = other.textPrimary
        textSecondary = other.textSecondary
        textDisabled = other.textDisabled
        textCritical = other.textCritical
        textCriticalButton = other.textCriticalButton
        textAccent = other.textAccent
        textAccentDisabled = other.textAccentDisabled
        textOnColorPrimary = other.textOnColorPrimary
        textOnColorSecondary = other.textOnColorSecondary
        textActionPrimary = other.textActionPrimary
        textActionPrimaryDisabled = other.textActionPrimaryDisabled
        textActionSecondary = other.textActionSecondary
        textActionTertiary = other.textActionTertiary
        textActionTertiaryActive = other.textActionTertiaryActive
        iconPrimary = other.iconPrimary
        iconPrimaryInactive = other.iconPrimaryInactive
        iconSecondary = other.iconSecondary
        iconActive = other.iconActive
        iconDisabled = other.iconDisabled
        iconOnColor = other.iconOnColor
        iconOnColorDisabled = other.iconOnColorDisabled
        iconNotice = other.iconNotice
        iconButton = other.iconButton
        iconCritical = other.iconCritical
        iconCriticalButton = other.iconCriticalButton
        iconAccentViolet = other.iconAccentViolet
        iconAccentBlue = other.iconAccentBlue
        iconAccentPink = other.iconAccentPink
        iconAccentGreen = other.iconAccentGreen
        iconAccentYellow = other.iconAccentYellow
        iconActionPrimary = other.iconActionPrimary
        iconActionSecondary = other.iconActionSecondary
        iconActionTertiary = other.iconActionTertiary
        iconGradientStart = other.iconGradientStart
        iconGradientEnd = other.iconGradientEnd
        borderPrimary = other.borderPrimary
        borderSecondary = other.borderSecondary
        borderInverted = other.borderInverted
        borderFormDefault = other.borderFormDefault
        borderAccent = other.borderAccent
        borderDisabled = other.borderDisabled
        borderCritical = other.borderCritical
        borderToolbarDivider = other.borderToolbarDivider
        ripple = other.ripple
        tabActive = other.tabActive
        tabInactive = other.tabInactive
        badgeActive = other.badgeActive
        surfaceDimVariant = other.surfaceDimVariant
    }

    /**
     * Return a copy of this [AcornColors] and optionally overriding any of the provided values.
     */
    @Suppress("LongMethod")
    fun copy(
        layer1: Color = this.layer1,
        layer2: Color = this.layer2,
        layer3: Color = this.layer3,
        layer4Start: Color = this.layer4Start,
        layer4Center: Color = this.layer4Center,
        layer4End: Color = this.layer4End,
        layerAccent: Color = this.layerAccent,
        layerAccentNonOpaque: Color = this.layerAccentNonOpaque,
        layerAccentOpaque: Color = this.layerAccentOpaque,
        layerScrim: Color = this.layerScrim,
        layerGradientStart: Color = this.layerGradientStart,
        layerGradientEnd: Color = this.layerGradientEnd,
        layerWarning: Color = this.layerWarning,
        layerSuccess: Color = this.layerSuccess,
        layerCritical: Color = this.layerCritical,
        layerInformation: Color = this.layerInformation,
        layerSearch: Color = this.layerSearch,
        layerAutofillText: Color = this.layerAutofillText,
        actionPrimary: Color = this.actionPrimary,
        actionPrimaryDisabled: Color = this.actionPrimaryDisabled,
        actionSecondary: Color = this.actionSecondary,
        actionTertiary: Color = this.actionTertiary,
        actionQuarternary: Color = this.actionQuarternary,
        actionWarning: Color = this.actionWarning,
        actionSuccess: Color = this.actionSuccess,
        actionCritical: Color = this.actionCritical,
        actionInformation: Color = this.actionInformation,
        formDefault: Color = this.formDefault,
        formSelected: Color = this.formSelected,
        formSurface: Color = this.formSurface,
        formDisabled: Color = this.formDisabled,
        formOn: Color = this.formOn,
        formOff: Color = this.formOff,
        indicatorActive: Color = this.indicatorActive,
        indicatorInactive: Color = this.indicatorInactive,
        textPrimary: Color = this.textPrimary,
        textSecondary: Color = this.textSecondary,
        textDisabled: Color = this.textDisabled,
        textCritical: Color = this.textCritical,
        textCriticalButton: Color = this.textCriticalButton,
        textAccent: Color = this.textAccent,
        textAccentDisabled: Color = this.textAccentDisabled,
        textInverted: Color = this.textInverted,
        textOnColorPrimary: Color = this.textOnColorPrimary,
        textOnColorSecondary: Color = this.textOnColorSecondary,
        textActionPrimary: Color = this.textActionPrimary,
        textActionPrimaryDisabled: Color = this.textActionPrimaryDisabled,
        textActionSecondary: Color = this.textActionSecondary,
        textActionTertiary: Color = this.textActionTertiary,
        textActionTertiaryActive: Color = this.textActionTertiaryActive,
        iconPrimary: Color = this.iconPrimary,
        iconPrimaryInactive: Color = this.iconPrimaryInactive,
        iconSecondary: Color = this.iconSecondary,
        iconActive: Color = this.iconActive,
        iconDisabled: Color = this.iconDisabled,
        iconOnColor: Color = this.iconOnColor,
        iconOnColorDisabled: Color = this.iconOnColorDisabled,
        iconNotice: Color = this.iconNotice,
        iconButton: Color = this.iconButton,
        iconCritical: Color = this.iconCritical,
        iconCriticalButton: Color = this.iconCriticalButton,
        iconAccentViolet: Color = this.iconAccentViolet,
        iconAccentBlue: Color = this.iconAccentBlue,
        iconAccentPink: Color = this.iconAccentPink,
        iconAccentGreen: Color = this.iconAccentGreen,
        iconAccentYellow: Color = this.iconAccentYellow,
        iconActionPrimary: Color = this.iconActionPrimary,
        iconActionSecondary: Color = this.iconActionSecondary,
        iconActionTertiary: Color = this.iconActionTertiary,
        actionChipSelected: Color = this.actionChipSelected,
        iconGradientStart: Color = this.iconGradientStart,
        iconGradientEnd: Color = this.iconGradientEnd,
        borderPrimary: Color = this.borderPrimary,
        borderSecondary: Color = this.borderSecondary,
        borderInverted: Color = this.borderInverted,
        borderFormDefault: Color = this.borderFormDefault,
        borderAccent: Color = this.borderAccent,
        borderDisabled: Color = this.borderDisabled,
        borderWarning: Color = this.borderCritical,
        borderToolbarDivider: Color = this.borderToolbarDivider,
        ripple: Color = this.ripple,
        tabActive: Color = this.tabActive,
        tabInactive: Color = this.tabInactive,
        badgeActive: Color = this.badgeActive,
        surfaceDimVariant: Color = this.surfaceDimVariant,
    ): AcornColors = AcornColors(
        layer1 = layer1,
        layer2 = layer2,
        layer3 = layer3,
        layer4Start = layer4Start,
        layer4Center = layer4Center,
        layer4End = layer4End,
        layerAccent = layerAccent,
        layerAccentNonOpaque = layerAccentNonOpaque,
        layerAccentOpaque = layerAccentOpaque,
        layerScrim = layerScrim,
        layerGradientStart = layerGradientStart,
        layerGradientEnd = layerGradientEnd,
        layerWarning = layerWarning,
        layerSuccess = layerSuccess,
        layerCritical = layerCritical,
        layerInformation = layerInformation,
        layerSearch = layerSearch,
        layerAutofillText = layerAutofillText,
        actionPrimary = actionPrimary,
        actionPrimaryDisabled = actionPrimaryDisabled,
        actionSecondary = actionSecondary,
        actionTertiary = actionTertiary,
        actionQuarternary = actionQuarternary,
        actionWarning = actionWarning,
        actionSuccess = actionSuccess,
        actionCritical = actionCritical,
        actionInformation = actionInformation,
        formDefault = formDefault,
        formSelected = formSelected,
        formSurface = formSurface,
        formDisabled = formDisabled,
        formOn = formOn,
        formOff = formOff,
        indicatorActive = indicatorActive,
        indicatorInactive = indicatorInactive,
        textPrimary = textPrimary,
        textSecondary = textSecondary,
        textDisabled = textDisabled,
        textCritical = textCritical,
        textCriticalButton = textCriticalButton,
        textAccent = textAccent,
        textAccentDisabled = textAccentDisabled,
        textInverted = textInverted,
        textOnColorPrimary = textOnColorPrimary,
        textOnColorSecondary = textOnColorSecondary,
        textActionPrimary = textActionPrimary,
        textActionPrimaryDisabled = textActionPrimaryDisabled,
        textActionSecondary = textActionSecondary,
        textActionTertiary = textActionTertiary,
        textActionTertiaryActive = textActionTertiaryActive,
        iconPrimary = iconPrimary,
        iconPrimaryInactive = iconPrimaryInactive,
        iconSecondary = iconSecondary,
        iconActive = iconActive,
        iconDisabled = iconDisabled,
        iconOnColor = iconOnColor,
        iconOnColorDisabled = iconOnColorDisabled,
        iconNotice = iconNotice,
        iconButton = iconButton,
        iconCritical = iconCritical,
        iconCriticalButton = iconCriticalButton,
        iconAccentViolet = iconAccentViolet,
        iconAccentBlue = iconAccentBlue,
        iconAccentPink = iconAccentPink,
        iconAccentGreen = iconAccentGreen,
        iconAccentYellow = iconAccentYellow,
        iconActionPrimary = iconActionPrimary,
        iconActionSecondary = iconActionSecondary,
        iconActionTertiary = iconActionTertiary,
        actionChipSelected = actionChipSelected,
        iconGradientStart = iconGradientStart,
        iconGradientEnd = iconGradientEnd,
        borderPrimary = borderPrimary,
        borderSecondary = borderSecondary,
        borderInverted = borderInverted,
        borderFormDefault = borderFormDefault,
        borderAccent = borderAccent,
        borderDisabled = borderDisabled,
        borderCritical = borderWarning,
        borderToolbarDivider = borderToolbarDivider,
        ripple = ripple,
        tabActive = tabActive,
        tabInactive = tabInactive,
        badgeActive = badgeActive,
        surfaceDimVariant = surfaceDimVariant,
    )
}

val darkColorPalette = AcornColors(
    layer1 = PhotonColors.DarkGrey60,
    layer2 = PhotonColors.DarkGrey30,
    layer3 = PhotonColors.DarkGrey80,
    layer4Start = PhotonColors.Purple70,
    layer4Center = PhotonColors.Violet80,
    layer4End = PhotonColors.Ink05,
    layerAccent = PhotonColors.Violet40,
    layerAccentNonOpaque = PhotonColors.Violet50A32,
    layerAccentOpaque = Color(0xFF423262),
    layerScrim = PhotonColors.DarkGrey90A95,
    layerGradientStart = PhotonColors.Violet70,
    layerGradientEnd = PhotonColors.Violet60,
    layerWarning = PhotonColors.Yellow70A77,
    layerSuccess = PhotonColors.Green80,
    layerCritical = PhotonColors.Pink80,
    layerInformation = PhotonColors.Blue50,
    layerSearch = PhotonColors.DarkGrey80,
    layerAutofillText = PhotonColors.LightGrey05A34,
    actionPrimary = PhotonColors.Violet60,
    actionPrimaryDisabled = PhotonColors.Violet60A50,
    actionSecondary = PhotonColors.DarkGrey05,
    actionTertiary = PhotonColors.DarkGrey10,
    actionQuarternary = PhotonColors.DarkGrey80,
    actionWarning = PhotonColors.Yellow40A41,
    actionSuccess = PhotonColors.Green70,
    actionCritical = PhotonColors.Pink70A69,
    actionInformation = PhotonColors.Blue60,
    formDefault = PhotonColors.LightGrey05,
    formSelected = PhotonColors.Violet40,
    formSurface = PhotonColors.DarkGrey05,
    formDisabled = PhotonColors.DarkGrey05,
    formOn = PhotonColors.Violet40,
    formOff = PhotonColors.LightGrey05,
    indicatorActive = PhotonColors.LightGrey90,
    indicatorInactive = PhotonColors.DarkGrey05,
    textPrimary = PhotonColors.LightGrey05,
    textSecondary = PhotonColors.LightGrey40,
    textDisabled = PhotonColors.LightGrey05A40,
    textCritical = PhotonColors.Red20,
    textCriticalButton = PhotonColors.Red20,
    textAccent = PhotonColors.Violet20,
    textAccentDisabled = PhotonColors.Violet20A60,
    textInverted = PhotonColors.DarkGrey90,
    textOnColorPrimary = PhotonColors.LightGrey05,
    textOnColorSecondary = PhotonColors.LightGrey40,
    textActionPrimary = PhotonColors.LightGrey05,
    textActionPrimaryDisabled = PhotonColors.LightGrey05A50,
    textActionSecondary = PhotonColors.LightGrey05,
    textActionTertiary = PhotonColors.LightGrey05,
    textActionTertiaryActive = PhotonColors.LightGrey05,
    iconPrimary = PhotonColors.LightGrey05,
    iconPrimaryInactive = PhotonColors.LightGrey05A60,
    iconSecondary = PhotonColors.LightGrey40,
    iconActive = PhotonColors.Violet40,
    iconDisabled = PhotonColors.LightGrey05A40,
    iconOnColor = PhotonColors.LightGrey05,
    iconOnColorDisabled = PhotonColors.LightGrey05A40,
    iconNotice = PhotonColors.Blue30,
    iconButton = PhotonColors.LightGrey05,
    iconCritical = PhotonColors.Red20,
    iconCriticalButton = PhotonColors.Red20,
    iconAccentViolet = PhotonColors.Violet20,
    iconAccentBlue = PhotonColors.Blue20,
    iconAccentPink = PhotonColors.Pink20,
    iconAccentGreen = PhotonColors.Green20,
    iconAccentYellow = PhotonColors.Yellow20,
    iconActionPrimary = PhotonColors.LightGrey05,
    iconActionSecondary = PhotonColors.LightGrey05,
    iconActionTertiary = PhotonColors.LightGrey05,
    actionChipSelected = PhotonColors.Violet50A32,
    iconGradientStart = PhotonColors.Violet20,
    iconGradientEnd = PhotonColors.Blue20,
    borderPrimary = PhotonColors.DarkGrey05,
    borderSecondary = PhotonColors.DarkGrey10,
    borderInverted = PhotonColors.LightGrey30,
    borderFormDefault = PhotonColors.LightGrey05,
    borderAccent = PhotonColors.Violet40,
    borderDisabled = PhotonColors.LightGrey05A40,
    borderCritical = PhotonColors.Red20,
    borderToolbarDivider = PhotonColors.DarkGrey60,
    ripple = PhotonColors.White,
    tabActive = PhotonColors.DarkGrey30,
    tabInactive = PhotonColors.DarkGrey80,
    badgeActive = PhotonColors.Ink60,
    surfaceDimVariant = PhotonColors.DarkGrey80,
)

val lightColorPalette = AcornColors(
    layer1 = PhotonColors.LightGrey10,
    layer2 = PhotonColors.White,
    layer3 = PhotonColors.LightGrey20,
    layer4Start = PhotonColors.Purple70,
    layer4Center = PhotonColors.Violet80,
    layer4End = PhotonColors.Ink05,
    layerAccent = PhotonColors.Ink20,
    layerAccentNonOpaque = PhotonColors.Violet70A12,
    layerAccentOpaque = Color(0xFFEAE4F9),
    layerScrim = PhotonColors.DarkGrey30A95,
    layerGradientStart = PhotonColors.Violet70,
    layerGradientEnd = PhotonColors.Violet60,
    layerWarning = PhotonColors.Yellow20,
    layerSuccess = PhotonColors.Green20,
    layerCritical = PhotonColors.Red10,
    layerInformation = PhotonColors.Blue50A44,
    layerSearch = PhotonColors.LightGrey30,
    layerAutofillText = PhotonColors.DarkGrey05A43,
    actionPrimary = PhotonColors.Ink20,
    actionPrimaryDisabled = PhotonColors.Ink20A20,
    actionSecondary = PhotonColors.LightGrey30,
    actionTertiary = PhotonColors.LightGrey40,
    actionQuarternary = PhotonColors.LightGrey10,
    actionWarning = PhotonColors.Yellow60A40,
    actionSuccess = PhotonColors.Green60,
    actionCritical = PhotonColors.Red30,
    actionInformation = PhotonColors.Blue50,
    formDefault = PhotonColors.DarkGrey90,
    formSelected = PhotonColors.Ink20,
    formSurface = PhotonColors.LightGrey50,
    formDisabled = PhotonColors.LightGrey50,
    formOn = PhotonColors.Ink20,
    formOff = PhotonColors.LightGrey05,
    indicatorActive = PhotonColors.LightGrey50,
    indicatorInactive = PhotonColors.LightGrey30,
    textPrimary = PhotonColors.DarkGrey90,
    textSecondary = PhotonColors.DarkGrey05,
    textDisabled = PhotonColors.DarkGrey90A40,
    textCritical = PhotonColors.Red70,
    textCriticalButton = PhotonColors.Red70,
    textAccent = PhotonColors.Violet70,
    textAccentDisabled = PhotonColors.Violet70A80,
    textInverted = PhotonColors.LightGrey05,
    textOnColorPrimary = PhotonColors.LightGrey05,
    textOnColorSecondary = PhotonColors.LightGrey40,
    textActionPrimary = PhotonColors.LightGrey05,
    textActionPrimaryDisabled = PhotonColors.DarkGrey90A50,
    textActionSecondary = PhotonColors.DarkGrey90,
    textActionTertiary = PhotonColors.DarkGrey90,
    textActionTertiaryActive = PhotonColors.LightGrey05,
    iconPrimary = PhotonColors.DarkGrey90,
    iconPrimaryInactive = PhotonColors.DarkGrey90A60,
    iconSecondary = PhotonColors.DarkGrey05,
    iconActive = PhotonColors.Ink20,
    iconDisabled = PhotonColors.DarkGrey90A40,
    iconOnColor = PhotonColors.LightGrey05,
    iconOnColorDisabled = PhotonColors.LightGrey05A40,
    iconNotice = PhotonColors.Blue30,
    iconButton = PhotonColors.Ink20,
    iconCritical = PhotonColors.Red70,
    iconCriticalButton = PhotonColors.Red70,
    iconAccentViolet = PhotonColors.Violet70,
    iconAccentBlue = PhotonColors.Blue60,
    iconAccentPink = PhotonColors.Pink60,
    iconAccentGreen = PhotonColors.Green60,
    iconAccentYellow = PhotonColors.Yellow60,
    iconActionPrimary = PhotonColors.LightGrey05,
    iconActionSecondary = PhotonColors.DarkGrey90,
    iconActionTertiary = PhotonColors.DarkGrey90,
    actionChipSelected = PhotonColors.Violet50A32,
    iconGradientStart = PhotonColors.Violet50,
    iconGradientEnd = PhotonColors.Blue60,
    borderPrimary = PhotonColors.LightGrey30,
    borderSecondary = PhotonColors.LightGrey20,
    borderInverted = PhotonColors.DarkGrey05,
    borderFormDefault = PhotonColors.DarkGrey90,
    borderAccent = PhotonColors.Ink20,
    borderDisabled = PhotonColors.DarkGrey90A40,
    borderCritical = PhotonColors.Red70,
    borderToolbarDivider = PhotonColors.LightGrey10,
    ripple = PhotonColors.Black,
    tabActive = PhotonColors.LightGrey10,
    tabInactive = PhotonColors.LightGrey20,
    badgeActive = PhotonColors.Violet05,
    surfaceDimVariant = PhotonColors.LightGrey20,
)

val privateColorPalette = darkColorPalette.copy(
    layer1 = PhotonColors.Violet90,
    layer2 = PhotonColors.Violet90,
    layer3 = PhotonColors.Ink90,
    layerSearch = PhotonColors.Ink90,
    layerAutofillText = PhotonColors.Violet60,
    borderPrimary = PhotonColors.Ink05,
    borderSecondary = PhotonColors.Ink10,
    borderToolbarDivider = PhotonColors.Violet80,
    tabActive = PhotonColors.Purple60,
    tabInactive = PhotonColors.Ink90,
    surfaceDimVariant = PhotonColors.Ink90,
)

@Suppress("LongParameterList")
private fun AcornColors.toM3ColorScheme(
    primary: Color,
    primaryContainer: Color,
    inversePrimary: Color,
    secondaryContainer: Color,
    tertiaryContainer: Color,
    surface: Color,
    inverseSurface: Color,
    errorContainer: Color,
    outline: Color,
    outlineVariant: Color,
    surfaceBright: Color,
    surfaceContainer: Color,
    surfaceContainerHigh: Color,
    surfaceContainerHighest: Color,
    surfaceContainerLow: Color,
    surfaceContainerLowest: Color,
): ColorScheme = ColorScheme(
    primary = primary,
    onPrimary = textInverted,
    primaryContainer = primaryContainer,
    onPrimaryContainer = textPrimary,
    inversePrimary = inversePrimary,
    secondary = textSecondary,
    onSecondary = textInverted,
    secondaryContainer = secondaryContainer,
    onSecondaryContainer = textPrimary,
    tertiary = textAccent,
    onTertiary = textInverted,
    tertiaryContainer = tertiaryContainer,
    onTertiaryContainer = textPrimary,
    background = surface,
    onBackground = textPrimary, // onSurface
    surface = surface,
    onSurface = textPrimary,
    surfaceVariant = surfaceContainerHighest,
    onSurfaceVariant = textSecondary,
    surfaceTint = layerAutofillText,
    inverseSurface = inverseSurface,
    inverseOnSurface = textInverted,
    error = textCritical,
    onError = textInverted,
    errorContainer = errorContainer,
    onErrorContainer = textPrimary,
    outline = outline,
    outlineVariant = outlineVariant,
    scrim = layerScrim,
    surfaceBright = surfaceBright,
    surfaceDim = layerSearch,
    surfaceContainer = surfaceContainer,
    surfaceContainerHigh = surfaceContainerHigh,
    surfaceContainerHighest = surfaceContainerHighest,
    surfaceContainerLow = surfaceContainerLow,
    surfaceContainerLowest = surfaceContainerLowest,
)

/**
 * Returns a dark Material color scheme mapped from Acorn.
 */
fun acornDarkColorScheme(): ColorScheme = darkColorPalette.toM3ColorScheme(
    primary = PhotonColors.Violet10,
    primaryContainer = PhotonColors.Violet80,
    inversePrimary = PhotonColors.Violet70,
    secondaryContainer = Color(0xFF4B3974),
    tertiaryContainer = PhotonColors.Pink80,
    surface = PhotonColors.DarkGrey60,
    inverseSurface = PhotonColors.LightGrey10,
    errorContainer = PhotonColors.Red80,
    outline = PhotonColors.LightGrey80,
    outlineVariant = PhotonColors.DarkGrey05,
    surfaceBright = PhotonColors.DarkGrey40,
    surfaceContainer = PhotonColors.DarkGrey60,
    surfaceContainerHigh = PhotonColors.DarkGrey50,
    surfaceContainerHighest = PhotonColors.DarkGrey40,
    surfaceContainerLow = PhotonColors.DarkGrey70,
    surfaceContainerLowest = PhotonColors.DarkGrey80,
)

/**
 * Returns a light Material color scheme mapped from Acorn.
 */
fun acornLightColorScheme(): ColorScheme = lightColorPalette.toM3ColorScheme(
    primary = PhotonColors.Ink20,
    primaryContainer = PhotonColors.Violet05,
    inversePrimary = PhotonColors.Violet20,
    secondaryContainer = Color(0xFFE6E0F5),
    tertiaryContainer = PhotonColors.Pink05,
    surface = PhotonColors.LightGrey10,
    inverseSurface = PhotonColors.DarkGrey60,
    errorContainer = PhotonColors.Red05,
    outline = PhotonColors.LightGrey90,
    outlineVariant = PhotonColors.LightGrey30,
    surfaceBright = PhotonColors.White,
    surfaceContainer = PhotonColors.LightGrey10,
    surfaceContainerHigh = PhotonColors.LightGrey20,
    surfaceContainerHighest = PhotonColors.LightGrey30,
    surfaceContainerLow = PhotonColors.LightGrey05,
    surfaceContainerLowest = PhotonColors.White,
)

/**
 * Returns a private Material color scheme mapped from Acorn.
 */
fun acornPrivateColorScheme(): ColorScheme = privateColorPalette.toM3ColorScheme(
    primary = PhotonColors.Violet10,
    primaryContainer = PhotonColors.Violet80,
    inversePrimary = PhotonColors.Violet70,
    secondaryContainer = Color(0xFF4B3974),
    tertiaryContainer = PhotonColors.Pink80,
    surface = Color(0xFF342B4A),
    inverseSurface = PhotonColors.LightGrey10,
    errorContainer = PhotonColors.Red80,
    outline = PhotonColors.LightGrey80,
    outlineVariant = PhotonColors.DarkGrey05,
    surfaceBright = Color(0xFF413857),
    surfaceContainer = Color(0xFF342B4A),
    surfaceContainerHigh = Color(0xFF3B3251),
    surfaceContainerHighest = Color(0xFF413857),
    surfaceContainerLow = Color(0xFF281C3D),
    surfaceContainerLowest = PhotonColors.Ink90,
)

/**
 * M3 color scheme extensions
 */

/**
 * Information
 *
 * Attention-grabbing color against surface for fills, icons, and text, indicating neutral information.
 */
val ColorScheme.information: Color
    @Composable
    @ReadOnlyComposable
    get() = AcornTheme.colors.iconAccentBlue

/**
 * @see AcornColors.surfaceDimVariant
 */
val ColorScheme.surfaceDimVariant: Color
    @Composable
    @ReadOnlyComposable
    get() = AcornTheme.colors.surfaceDimVariant

/**
 * Primary Fixed
 *
 * A fixed accent color. This maintains the same tone across all themes.
 */
val ColorScheme.primaryFixed: Color
    get() = Color(0xFFE7DFFF)

/**
 * On Primary Fixed
 *
 * Used for text and icons against the [ColorScheme.primaryFixed] color.
 */
val ColorScheme.onPrimaryFixed: Color
    get() = Color(0xFF15141A)
