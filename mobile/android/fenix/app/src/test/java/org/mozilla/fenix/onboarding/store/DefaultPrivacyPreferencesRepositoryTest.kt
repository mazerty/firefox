/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.onboarding.store

import androidx.test.ext.junit.runners.AndroidJUnit4
import junit.framework.TestCase.assertFalse
import junit.framework.TestCase.assertTrue
import mozilla.components.lib.crash.store.CrashReportOption
import mozilla.components.support.test.robolectric.testContext
import org.junit.Test
import org.junit.runner.RunWith
import org.mozilla.fenix.utils.Settings

@RunWith(AndroidJUnit4::class)
class DefaultPrivacyPreferencesRepositoryTest {

    @Test
    fun `GIVEN crash reporting flag is updated in settings WHEN getPreference is called THEN updated value is returned `() {
        val settings = Settings(testContext)
        val repository = DefaultPrivacyPreferencesRepository(settings)
        assertTrue(CrashReportOption.fromLabel(settings.crashReportChoice) == CrashReportOption.Ask)
        assertFalse(repository.getPreference(PreferenceType.CrashReporting))

        settings.crashReportChoice = CrashReportOption.Auto.label
        assertTrue(repository.getPreference(PreferenceType.CrashReporting))

        settings.crashReportChoice = CrashReportOption.Never.label
        assertFalse(repository.getPreference(PreferenceType.CrashReporting))
    }

    @Test
    fun `GIVEN telemetry flag is updated in settings WHEN getPreference is called THEN updated value is returned `() {
        val settings = Settings(testContext)
        val repository = DefaultPrivacyPreferencesRepository(settings)
        assertTrue(settings.isTelemetryEnabled)
        assertTrue(repository.getPreference(PreferenceType.UsageData))

        settings.isTelemetryEnabled = false
        assertFalse(repository.getPreference(PreferenceType.UsageData))

        settings.isTelemetryEnabled = true
        assertTrue(repository.getPreference(PreferenceType.UsageData))
    }

    @Test
    fun `WHEN setPreference is called THEN the preference value is updated`() {
        val settings = Settings(testContext)
        val repository = DefaultPrivacyPreferencesRepository(settings)
        assertTrue(settings.isTelemetryEnabled)
        assertTrue(settings.isExperimentationEnabled)

        repository.setPreference(PreferenceType.UsageData, false)

        assertFalse(settings.isTelemetryEnabled)
        assertFalse(settings.isExperimentationEnabled)

        repository.setPreference(PreferenceType.UsageData, true)
        assertTrue(settings.isTelemetryEnabled)
        assertTrue(settings.isExperimentationEnabled)
    }
}
