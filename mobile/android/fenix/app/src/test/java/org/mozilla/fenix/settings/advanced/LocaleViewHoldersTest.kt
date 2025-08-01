/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.settings.advanced

import android.view.LayoutInflater
import android.view.View
import androidx.core.view.isVisible
import io.mockk.Runs
import io.mockk.every
import io.mockk.just
import io.mockk.mockk
import io.mockk.verify
import mozilla.components.support.test.robolectric.testContext
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.mozilla.fenix.R
import org.mozilla.fenix.databinding.LocaleSettingsItemBinding
import org.robolectric.RobolectricTestRunner
import java.util.Locale

@RunWith(RobolectricTestRunner::class)
class LocaleViewHoldersTest {

    private val selectedLocale = mockk<Locale>(relaxed = true) {
        every { displayName } returns "Test Locale"
        every { getDisplayName(any()) } returns "Test Locale display name"
    }

    private lateinit var view: View
    private lateinit var interactor: LocaleSettingsViewInteractor
    private lateinit var localeViewHolder: LocaleViewHolder
    private lateinit var systemLocaleViewHolder: SystemLocaleViewHolder
    private lateinit var localeSettingsItemBinding: LocaleSettingsItemBinding
    private lateinit var localeSelectionChecker: LocaleSelectionChecker

    @Before
    fun setup() {
        view = LayoutInflater.from(testContext)
            .inflate(R.layout.locale_settings_item, null)

        localeSettingsItemBinding = LocaleSettingsItemBinding.bind(view)
        interactor = mockk()
        localeSelectionChecker = mockk(relaxed = true)
        localeViewHolder = LocaleViewHolder(view, selectedLocale, interactor, localeSelectionChecker)
        systemLocaleViewHolder = SystemLocaleViewHolder(view, selectedLocale, interactor, localeSelectionChecker)
    }

    @Test
    fun `bind LocaleViewHolder`() {
        localeViewHolder.bind(selectedLocale)

        assertEquals("Test Locale display name", localeSettingsItemBinding.localeTitleText.text)
        assertEquals("Test Locale", localeSettingsItemBinding.localeSubtitleText.text)
        // The selected icon should be visible for the selected locale.
        assertTrue(localeSettingsItemBinding.localeSelectedIcon.isVisible)
    }

    @Test
    fun `LocaleViewHolder calls interactor on click`() {
        localeViewHolder.bind(selectedLocale)

        every { interactor.onLocaleSelected(selectedLocale) } just Runs
        view.performClick()
        verify { interactor.onLocaleSelected(selectedLocale) }
    }

    // Note that after we can run tests on SDK 30 the result of the locale.getDisplayName(locale) could differ and this test will fail
    @Test
    fun `GIVEN a locale is not properly identified in Android WHEN we bind locale THEN the title and subtitle are set from locale maps`() {
        val otherLocale = Locale.forLanguageTag("vec")

        localeViewHolder.bind(otherLocale)

        assertEquals("Vèneto", localeSettingsItemBinding.localeTitleText.text)
        assertEquals("Venetian", localeSettingsItemBinding.localeSubtitleText.text)
    }

    @Test
    fun `GIVEN a locale is not properly identified in Android and it is not mapped  WHEN we bind locale THEN the text is the capitalised code`() {
        val otherLocale = Locale.forLanguageTag("yyy")

        localeViewHolder.bind(otherLocale)

        assertEquals("Yyy", localeSettingsItemBinding.localeTitleText.text)
        assertEquals("Yyy", localeSettingsItemBinding.localeSubtitleText.text)
    }

    @Test
    fun `bind SystemLocaleViewHolder`() {
        systemLocaleViewHolder.bind(selectedLocale)

        assertEquals("Follow device language", localeSettingsItemBinding.localeTitleText.text)
        assertEquals("Test Locale display name", localeSettingsItemBinding.localeSubtitleText.text)

        // The selected icon should not be visible for the system locale since selectedLocale is not the system default.
        assertFalse(localeSettingsItemBinding.localeSelectedIcon.isVisible)
    }

    @Test
    fun `SystemLocaleViewHolder calls interactor on click`() {
        systemLocaleViewHolder.bind(selectedLocale)

        every { interactor.onDefaultLocaleSelected() } just Runs
        view.performClick()
        verify { interactor.onDefaultLocaleSelected() }
    }
}
