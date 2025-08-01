/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components.metrics

import com.google.android.gms.ads.identifier.AdvertisingIdClient
import com.google.android.gms.common.GooglePlayServicesNotAvailableException
import com.google.android.gms.common.GooglePlayServicesRepairableException
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.IOException

class MetricsUtilsTest {

    @Test
    fun `getAdvertisingID() returns null if the API throws`() {
        val exceptions = listOf(
            GooglePlayServicesNotAvailableException(1),
            GooglePlayServicesRepairableException(0, "", mockk()),
            IllegalStateException(),
            IOException(),
        )

        exceptions.forEach {
            assertNull(MetricsUtils.getAdvertisingID { throw it })
        }
    }

    @Test
    fun `getAdvertisingID() returns null if the API returns null info`() {
        val mockInfo: AdvertisingIdClient.Info = mockk()
        every { mockInfo.id } returns null

        assertNull(MetricsUtils.getAdvertisingID { mockInfo.id })
    }

    @Test
    fun `getAdvertisingID() returns a valid string if the API returns a valid ID`() {
        val testId = "test-value-id"
        val mockInfo: AdvertisingIdClient.Info = mockk()
        every { mockInfo.id } returns testId

        assertEquals(testId, MetricsUtils.getAdvertisingID({ mockInfo.id }))
    }

    @Test
    fun `getHashedIdentifier() returns a hashed identifier`() = runTest {
        val testId = "test-value-id"
        val testPackageName = "org.mozilla-test.fenix"
        val expectedHashedIdentifier = "mocked-HEX"
        var shaDigest: ByteArray? = null

        assertEquals(
            expectedHashedIdentifier,
            MetricsUtils.getHashedIdentifier(
                retrieveAdvertisingIdInfo = { testId },
                encodeToString = { bytesToEncode: ByteArray, _: Int ->
                    shaDigest = bytesToEncode
                    expectedHashedIdentifier
                },
                customSalt = testPackageName,
            ),
        )

        // Check that the digest of the identifier matches with what we expect.
        // Please note that in the real world, Base64.encodeToString would encode
        // this to something much shorter, which we'd send with the ping.
        val expectedDigestBytes = byteArrayOf(
            52, -79, -84, 79, 101, 22, -82, -44, -44, -14, 21, 15, 48, 88, -94, -74,
            -8, 25, -72, -120, -37, 108, 47, 16, 2, -37, 126, 41, 102, -92, 103, 24,
        )

        assertArrayEquals(expectedDigestBytes, shaDigest)
    }

    companion object {
        const val ENGINE_SOURCE_IDENTIFIER = "google-2018"
    }
}
