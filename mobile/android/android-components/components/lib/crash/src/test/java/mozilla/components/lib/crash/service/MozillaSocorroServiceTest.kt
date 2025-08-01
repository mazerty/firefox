/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.lib.crash.service

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.google.common.io.Resources.getResource
import mozilla.components.concept.base.crash.Breadcrumb
import mozilla.components.lib.crash.Crash
import mozilla.components.lib.crash.CrashReporter
import mozilla.components.support.ktx.kotlin.toDate
import mozilla.components.support.test.any
import mozilla.components.support.test.argumentCaptor
import mozilla.components.support.test.robolectric.testContext
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Test
import org.junit.runner.RunWith
import org.mockito.ArgumentMatchers.anyBoolean
import org.mockito.Mockito.doCallRealMethod
import org.mockito.Mockito.doNothing
import org.mockito.Mockito.doReturn
import org.mockito.Mockito.mock
import org.mockito.Mockito.never
import org.mockito.Mockito.spy
import org.mockito.Mockito.times
import org.mockito.Mockito.verify
import java.io.BufferedReader
import java.io.ByteArrayInputStream
import java.io.File
import java.io.InputStreamReader
import java.util.zip.GZIPInputStream

@RunWith(AndroidJUnit4::class)
class MozillaSocorroServiceTest {
    private fun mockFormDataWriter(service: MozillaSocorroService): MozillaSocorroService.FormDataWriter {
        val mockFormDataWriter = mock(MozillaSocorroService.FormDataWriter::class.java)
        doCallRealMethod().`when`(mockFormDataWriter).sendAndDeleteFileAtPath(any(), any())
        doReturn(mockFormDataWriter).`when`(service).createFormDataWriter(any(), any(), any())
        return mockFormDataWriter
    }

    @Test
    fun `MozillaSocorroService sends native code crashes to GeckoView crash reporter`() {
        val service = spy(
            MozillaSocorroService(
                testContext,
                "Test App",
            ),
        )
        doReturn("").`when`(service).sendReport(any(), any(), any(), any(), anyBoolean(), anyBoolean())

        val crash = Crash.NativeCodeCrash(
            123,
            "",
            "",
            Crash.NativeCodeCrash.PROCESS_VISIBILITY_FOREGROUND_CHILD,
            processType = "content",
            breadcrumbs = arrayListOf(),
            remoteType = null,
        )
        service.report(crash)

        verify(service).report(crash)
        verify(service).sendReport(crash, null, crash.minidumpPath, crash.extrasPath, true, false)
    }

    @Test
    fun `MozillaSocorroService generated server URL have no spaces`() {
        val versionName = "test version name"
        val service = MozillaSocorroService(
            testContext,
            "Test App",
            versionName = versionName,
        )

        assertFalse(service.buildServerUrl(versionName).contains(" "))
        assertFalse(service.buildServerUrl(versionName).contains("}"))
        assertFalse(service.buildServerUrl(versionName).contains("{"))
    }

    @Test
    fun `MozillaSocorroService send uncaught exception crashes`() {
        val service = spy(
            MozillaSocorroService(
                testContext,
                "Test App",
            ),
        )
        doReturn("").`when`(service).sendReport(any(), any(), any(), any(), anyBoolean(), anyBoolean())

        val crash = Crash.UncaughtExceptionCrash(123, RuntimeException("Test"), arrayListOf())
        service.report(crash)

        verify(service).report(crash)
        verify(service).sendReport(crash, crash.throwable, null, null, false, true)
    }

    @Test
    fun `MozillaSocorroService do not send caught exception`() {
        val service = spy(
            MozillaSocorroService(
                testContext,
                "Test App",
            ),
        )
        doReturn("").`when`(service).sendReport(any(), any(), any(), any(), anyBoolean(), anyBoolean())
        val throwable = RuntimeException("Test")
        val breadcrumbs: ArrayList<Breadcrumb> = arrayListOf()
        val id = service.report(throwable, breadcrumbs)

        verify(service).report(throwable, breadcrumbs)
        verify(service, never()).sendReport(any(), any(), any(), any(), anyBoolean(), anyBoolean())
        assertNull(id)
    }

    @Test
    fun `MozillaSocorroService native fatal crash request is correct`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.NativeCodeCrash(
                123456,
                "dump.path",
                "extras.path",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
                processType = "main",
                breadcrumbs = arrayListOf(Breadcrumb(message = "Hello World", date = "2018-06-12T19:30+00:00".toDate("yyyy-MM-dd'T'HH:mmXXX"))),
                remoteType = null,
            )
            service.report(crash)

            val fileInputStream =
                ByteArrayInputStream(mockWebServer.takeRequest().body.inputStream().readBytes())
            val inputStream = GZIPInputStream(fileInputStream)
            val reader = InputStreamReader(inputStream)
            val bufferedReader = BufferedReader(reader)
            val request = bufferedReader.readText()

            assert(request.contains("name=Android_ProcessName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=ProductID\r\n\r\n{aa3c5121-dab2-40e2-81ca-7ea25febc110}"))
            assert(request.contains("name=Vendor\r\n\r\nN/A"))
            assert(request.contains("name=ReleaseChannel\r\n\r\nN/A"))
            assert(request.contains("name=Android_PackageName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=Android_Device\r\n\r\nrobolectric"))
            assert(request.contains("name=CrashType\r\n\r\n$FATAL_NATIVE_CRASH_TYPE"))
            assert(request.contains("name=CrashTime\r\n\r\n123"))
            assert(request.contains("name=useragent_locale\r\n\r\nen-US"))
            assert(request.contains("name=Breadcrumbs\r\n\r\n[{\"timestamp\":\"2018-06-12T19:30:00\",\"message\":\"Hello World\",\"category\":\"\",\"level\":\"Debug\",\"type\":\"Default\",\"data\":{}}]"))

            verify(service).report(crash)
            verify(service).sendReport(crash, null, "dump.path", "extras.path", true, true)
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `incorrect file extension is ignored in native fatal crash requests`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.NativeCodeCrash(
                123456,
                "test/minidumps/3fa772dc-dc89-c08d-c03e-7f441c50821e.ini",
                "test/file/66dd8af2-643c-ca11-5178-e61c6819f827",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
                processType = "main",
                breadcrumbs = arrayListOf(),
                remoteType = null,
            )

            doReturn(HashMap<String, String>()).`when`(service).readExtrasFromFile(any())
            val formDataWriter = mockFormDataWriter(service)
            service.report(crash)

            verify(service).report(crash)
            verify(service, times(0)).readExtrasFromFile(any())
            verify(formDataWriter, times(0)).sendFile(any(), any())
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `incorrect file format is ignored in native fatal crash requests`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.NativeCodeCrash(
                123456,
                "test/minidumps/test.dmp",
                "test/file/test.extra",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
                processType = "main",
                breadcrumbs = arrayListOf(),
                remoteType = null,
            )

            doReturn(HashMap<String, String>()).`when`(service).readExtrasFromFile(any())
            val formDataWriter = mockFormDataWriter(service)
            service.report(crash)

            verify(service).report(crash)
            verify(service, times(0)).readExtrasFromFile(any())
            verify(formDataWriter, times(0)).sendFile(any(), any())
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `correct file format is used in native fatal crash requests`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.NativeCodeCrash(
                123456,
                "test/minidumps/3fa772dc-dc89-c08d-c03e-7f441c50821e.dmp",
                "test/file/66dd8af2-643c-ca11-5178-e61c6819f827.extra",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
                processType = "main",
                breadcrumbs = arrayListOf(),
                remoteType = null,
            )

            doReturn(HashMap<String, String>()).`when`(service).readExtrasFromFile(any())
            val formDataWriter = mockFormDataWriter(service)
            service.report(crash)

            verify(service).report(crash)
            verify(service).readExtrasFromFile(any())
            verify(formDataWriter).sendFile(any(), any())
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `additional minidumps are sent in requests`() {
        val mockWebServer = MockWebServer()
        val nameCaptor = argumentCaptor<String>()
        val fileCaptor = argumentCaptor<File>()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.NativeCodeCrash(
                123456,
                "test/minidumps/3fa772dc-dc89-c08d-c03e-7f441c50821e.dmp",
                "test/file/66dd8af2-643c-ca11-5178-e61c6819f827.extra",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
                processType = "main",
                breadcrumbs = arrayListOf(),
                remoteType = null,
            )

            doReturn(hashMapOf("additional_minidumps" to "browser,content")).`when`(service).readExtrasFromFile(any())
            val formDataWriter = mockFormDataWriter(service)
            service.report(crash)

            verify(service).report(crash)
            verify(service).readExtrasFromFile(any())
            verify(formDataWriter, times(3)).sendFile(
                nameCaptor.capture(),
                fileCaptor.capture(),
            )

            assertEquals(listOf("upload_file_minidump", "upload_file_minidump_browser", "upload_file_minidump_content"), nameCaptor.allValues)
            val files = fileCaptor.allValues
            assertEquals("test/minidumps/3fa772dc-dc89-c08d-c03e-7f441c50821e.dmp", files.get(0).path)
            assertEquals("test/minidumps/3fa772dc-dc89-c08d-c03e-7f441c50821e-browser.dmp", files.get(1).path)
            assertEquals("test/minidumps/3fa772dc-dc89-c08d-c03e-7f441c50821e-content.dmp", files.get(2).path)
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `MozillaSocorroService parameters is reported correctly`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    version = "test version",
                    buildId = "test build id",
                    vendor = "test vendor",
                    serverUrl = serverUrl.toString(),
                    versionName = "1.0.1",
                    versionCode = "1000",
                    releaseChannel = "test channel",
                    distributionId = "test distribution id",
                ),
            )

            val crash = Crash.NativeCodeCrash(
                123456,
                "dump.path",
                "extras.path",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
                processType = "main",
                breadcrumbs = arrayListOf(),
                remoteType = null,
            )
            service.report(crash)

            val fileInputStream =
                ByteArrayInputStream(mockWebServer.takeRequest().body.inputStream().readBytes())
            val inputStream = GZIPInputStream(fileInputStream)
            val reader = InputStreamReader(inputStream)
            val bufferedReader = BufferedReader(reader)
            val request = bufferedReader.readText()

            assert(request.contains("name=Android_ProcessName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=ProductID\r\n\r\n{aa3c5121-dab2-40e2-81ca-7ea25febc110}"))
            assert(request.contains("name=Vendor\r\n\r\ntest vendor"))
            assert(request.contains("name=ReleaseChannel\r\n\r\ntest channel"))
            assert(request.contains("name=Android_PackageName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=Android_Device\r\n\r\nrobolectric"))
            assert(request.contains("name=CrashType\r\n\r\n$FATAL_NATIVE_CRASH_TYPE"))
            assert(request.contains("name=CrashTime\r\n\r\n123"))
            assert(request.contains("name=GeckoViewVersion\r\n\r\ntest version"))
            assert(request.contains("name=BuildID\r\n\r\ntest build id"))
            assert(request.contains("name=Version\r\n\r\n1.0.1"))
            assert(request.contains("name=ApplicationBuildID\r\n\r\n1000"))
            assert(request.contains("name=useragent_locale\r\n\r\nen-US"))
            assert(request.contains("name=DistributionID\r\n\r\ntest distribution id"))

            verify(service).report(crash)
            verify(service).sendReport(crash, null, "dump.path", "extras.path", true, true)
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `MozillaSocorroService native non-fatal crash request is correct`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    vendor = "Mozilla",
                    releaseChannel = "nightly",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.NativeCodeCrash(
                123456,
                "dump.path",
                "extras.path",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_FOREGROUND_CHILD,
                processType = "content",
                breadcrumbs = arrayListOf(),
                remoteType = null,
            )
            service.report(crash)

            val fileInputStream =
                ByteArrayInputStream(mockWebServer.takeRequest().body.inputStream().readBytes())
            val inputStream = GZIPInputStream(fileInputStream)
            val reader = InputStreamReader(inputStream)
            val bufferedReader = BufferedReader(reader)
            val request = bufferedReader.readText()

            assert(request.contains("name=Android_ProcessName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=ProductID\r\n\r\n{aa3c5121-dab2-40e2-81ca-7ea25febc110}"))
            assert(request.contains("name=Vendor\r\n\r\nMozilla"))
            assert(request.contains("name=ReleaseChannel\r\n\r\nnightly"))
            assert(request.contains("name=Android_PackageName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=Android_Device\r\n\r\nrobolectric"))
            assert(request.contains("name=CrashType\r\n\r\n$NON_FATAL_NATIVE_CRASH_TYPE"))
            assert(request.contains("name=CrashTime\r\n\r\n123"))
            assert(request.contains("name=useragent_locale\r\n\r\nen-US"))

            verify(service).report(crash)
            verify(service).sendReport(crash, null, "dump.path", "extras.path", true, false)
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `MozillaSocorroService uncaught exception request is correct`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    vendor = "Mozilla",
                    releaseChannel = "nightly",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.UncaughtExceptionCrash(123456, RuntimeException("Test"), arrayListOf())
            service.report(crash)

            val fileInputStream =
                ByteArrayInputStream(mockWebServer.takeRequest().body.inputStream().readBytes())
            val inputStream = GZIPInputStream(fileInputStream)
            val reader = InputStreamReader(inputStream)
            val bufferedReader = BufferedReader(reader)
            val request = bufferedReader.readText()

            assert(request.contains("name=JavaStackTrace\r\n\r\njava.lang.RuntimeException: Test"))
            assert(request.contains("name=JavaException\r\n\r\n{\"exception\":{\"values\":[{\"stacktrace\":{\"frames\":[{\"module\":\"mozilla.components.lib.crash.service.MozillaSocorroServiceTest\",\"function\":\"MozillaSocorroService uncaught exception request is correct\",\"in_app\":true"))
            assert(request.contains("name=Android_ProcessName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=ProductID\r\n\r\n{aa3c5121-dab2-40e2-81ca-7ea25febc110}"))
            assert(request.contains("name=Vendor\r\n\r\nMozilla"))
            assert(request.contains("name=ReleaseChannel\r\n\r\nnightly"))
            assert(request.contains("name=Android_PackageName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=Android_Device\r\n\r\nrobolectric"))
            assert(request.contains("name=CrashType\r\n\r\n$UNCAUGHT_EXCEPTION_TYPE"))
            assert(request.contains("name=CrashTime\r\n\r\n123"))
            assert(request.contains("name=useragent_locale\r\n\r\nen-US"))

            verify(service).report(crash)
            verify(service).sendReport(crash, crash.throwable, null, null, false, true)
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `MozillaSocorroService handles 200 response correctly`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.UncaughtExceptionCrash(123, RuntimeException("Test"), arrayListOf())
            service.report(crash)

            mockWebServer.shutdown()
            verify(service).report(crash)
            verify(service).sendReport(crash, crash.throwable, null, null, false, true)
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `MozillaSocorroService handles 404 response correctly`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(MockResponse().setResponseCode(404).setBody("error"))
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    serverUrl = serverUrl.toString(),
                ),
            )

            val crash = Crash.NativeCodeCrash(
                123,
                null,
                null,
                Crash.NativeCodeCrash.PROCESS_VISIBILITY_FOREGROUND_CHILD,
                processType = "content",
                breadcrumbs = arrayListOf(),
                remoteType = null,
            )
            service.report(crash)
            mockWebServer.shutdown()

            verify(service).report(crash)
            verify(service).sendReport(crash, null, crash.minidumpPath, crash.extrasPath, true, false)
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `MozillaSocorroService parses extrasFile correctly`() {
        val service = spy(
            MozillaSocorroService(
                testContext,
                "Test App",
            ),
        )
        val file = File(getResource("TestExtrasFile").file)
        val extrasMap = service.readExtrasFromFile(file)

        assertEquals(extrasMap.size, 25)
        assertEquals(extrasMap["ContentSandboxLevel"], "2")
        assertEquals(extrasMap["TelemetryEnvironment"], "{\"EscapedField\":\"EscapedData\n\nfoo\"}")
        assertEquals(extrasMap["EMCheckCompatibility"], "true")
        assertEquals(extrasMap["ProductName"], "Firefox")
        assertEquals(extrasMap["ContentSandboxCapabilities"], "119")
        assertEquals(extrasMap["TelemetryClientId"], "")
        assertEquals(extrasMap["Vendor"], "Mozilla")
        assertEquals(extrasMap["InstallTime"], "1000000000")
        assertEquals(extrasMap["Theme"], "classic/1.0")
        assertEquals(extrasMap["ReleaseChannel"], "default")
        assertEquals(extrasMap["SafeMode"], "0")
        assertEquals(extrasMap["ContentSandboxCapable"], "1")
        assertEquals(extrasMap["useragent_locale"], "en-US")
        assertEquals(extrasMap["Version"], "55.0a1")
        assertEquals(extrasMap["BuildID"], "20170512114708")
        assertEquals(extrasMap["ProductID"], "{ec8030f7-c20a-464f-9b0e-13a3a9e97384}")
        assertEquals(extrasMap["TelemetryServerURL"], "")
        assertEquals(extrasMap["DOMIPCEnabled"], "1")
        assertEquals(extrasMap["Add-ons"], "")
        assertEquals(extrasMap["CrashTime"], "1494582646")
        assertEquals(extrasMap["UptimeTS"], "14.9179586")
        assertEquals(extrasMap["ThreadIdNameMapping"], "")
        assertEquals(extrasMap["ContentSandboxEnabled"], "1")
        assertEquals(extrasMap["StartupTime"], "1000000000")
        assertFalse(extrasMap.contains("URL"))
        assertFalse(extrasMap.contains("ServerURL"))
        assertFalse(extrasMap.contains("StackTraces"))
    }

    @Test
    fun `MozillaSocorroService parses legacyExtraFile correctly`() {
        val service = spy(
            MozillaSocorroService(
                testContext,
                "Test App",
            ),
        )
        val file = File(getResource("TestLegacyExtrasFile").file)
        val extrasMap = service.readExtrasFromFile(file)

        assertEquals(extrasMap.size, 25)
        assertEquals(extrasMap["ContentSandboxLevel"], "2")
        assertEquals(extrasMap["TelemetryEnvironment"], "{\"EscapedField\":\"EscapedData\n\nfoo\"}")
        assertEquals(extrasMap["EMCheckCompatibility"], "true")
        assertEquals(extrasMap["ProductName"], "Firefox")
        assertEquals(extrasMap["ContentSandboxCapabilities"], "119")
        assertEquals(extrasMap["TelemetryClientId"], "")
        assertEquals(extrasMap["Vendor"], "Mozilla")
        assertEquals(extrasMap["InstallTime"], "1000000000")
        assertEquals(extrasMap["Theme"], "classic/1.0")
        assertEquals(extrasMap["ReleaseChannel"], "default")
        assertEquals(extrasMap["SafeMode"], "0")
        assertEquals(extrasMap["ContentSandboxCapable"], "1")
        assertEquals(extrasMap["useragent_locale"], "en-US")
        assertEquals(extrasMap["Version"], "55.0a1")
        assertEquals(extrasMap["BuildID"], "20170512114708")
        assertEquals(extrasMap["ProductID"], "{ec8030f7-c20a-464f-9b0e-13a3a9e97384}")
        assertEquals(extrasMap["TelemetryServerURL"], "")
        assertEquals(extrasMap["DOMIPCEnabled"], "1")
        assertEquals(extrasMap["Add-ons"], "")
        assertEquals(extrasMap["CrashTime"], "1494582646")
        assertEquals(extrasMap["UptimeTS"], "14.9179586")
        assertEquals(extrasMap["ThreadIdNameMapping"], "")
        assertEquals(extrasMap["ContentSandboxEnabled"], "1")
        assertEquals(extrasMap["StartupTime"], "1000000000")
        assertFalse(extrasMap.contains("URL"))
        assertFalse(extrasMap.contains("ServerURL"))
        assertFalse(extrasMap.contains("StackTraces"))
    }

    @Test
    fun `MozillaSocorroService handles bad extrasFile correctly`() {
        val service = spy(
            MozillaSocorroService(
                testContext,
                "Test App",
            ),
        )
        val file = File(getResource("BadTestExtrasFile").file)
        val extrasMap = service.readExtrasFromFile(file)

        assertEquals(extrasMap.size, 0)
    }

    @Test
    fun `MozillaSocorroService unescape strings correctly`() {
        val service = spy(
            MozillaSocorroService(
                testContext,
                "Test App",
            ),
        )
        val test1 = "\\\\\\\\"
        val expected1 = "\\"
        assert(service.unescape(test1) == expected1)

        val test2 = "\\\\n"
        val expected2 = "\n"
        assert(service.unescape(test2) == expected2)

        val test3 = "\\\\t"
        val expected3 = "\t"
        assert(service.unescape(test3) == expected3)

        val test4 = "\\\\\\\\\\\\t\\\\t\\\\n\\\\\\\\"
        val expected4 = "\\\t\t\n\\"
        assert(service.unescape(test4) == expected4)
    }

    @Test
    fun `MozillaSocorroService returns crash id from Socorro`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()

            val service = MozillaSocorroService(
                testContext,
                "Test App",
                "{1234-1234-1234}",
                "0.1",
                "1.0",
                "Mozilla Test",
                mockWebServer.url("/").toString(),
                "0.0.1",
                "123",
                "test channel",
                "test distribution id",
            )

            val crash = Crash.NativeCodeCrash(
                0,
                "dump.path",
                "extras.path",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
                processType = "main",
                breadcrumbs = arrayListOf(),
                remoteType = null,
            )
            val id = service.report(crash)

            assertEquals("bp-924121d3-4de3-4b32-ab12-026fc0190928", id)
        } finally {
            mockWebServer.shutdown()
        }
    }

    @Test
    fun `GIVEN crash with release runtime tag WHEN reporting THEN version is updated correctly`() {
        val mockWebServer = MockWebServer()

        try {
            mockWebServer.enqueue(
                MockResponse().setResponseCode(200)
                    .setBody("CrashID=bp-924121d3-4de3-4b32-ab12-026fc0190928"),
            )
            mockWebServer.start()
            val serverUrl = mockWebServer.url("/")
            val service = spy(
                MozillaSocorroService(
                    testContext,
                    "Test App",
                    appId = "{aa3c5121-dab2-40e2-81ca-7ea25febc110}",
                    version = "test version",
                    buildId = "test build id",
                    vendor = "test vendor",
                    serverUrl = serverUrl.toString(),
                    versionName = "1.0.1",
                    versionCode = "1000",
                    releaseChannel = "test channel",
                    distributionId = "test distribution id",
                ),
            )

            val version = "136.0.1"
            val crash = Crash.NativeCodeCrash(
                123456,
                "dump.path",
                "extras.path",
                processVisibility = Crash.NativeCodeCrash.PROCESS_VISIBILITY_MAIN,
                processType = "main",
                breadcrumbs = arrayListOf(),
                remoteType = null,
                runtimeTags = mapOf(CrashReporter.RELEASE_RUNTIME_TAG to version),
            )
            service.report(crash)

            val fileInputStream =
                ByteArrayInputStream(mockWebServer.takeRequest().body.inputStream().readBytes())
            val inputStream = GZIPInputStream(fileInputStream)
            val reader = InputStreamReader(inputStream)
            val bufferedReader = BufferedReader(reader)
            val request = bufferedReader.readText()

            assert(request.contains("name=Android_ProcessName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=ProductID\r\n\r\n{aa3c5121-dab2-40e2-81ca-7ea25febc110}"))
            assert(request.contains("name=Vendor\r\n\r\ntest vendor"))
            assert(request.contains("name=ReleaseChannel\r\n\r\ntest channel"))
            assert(request.contains("name=Android_PackageName\r\n\r\nmozilla.components.lib.crash.test"))
            assert(request.contains("name=Android_Device\r\n\r\nrobolectric"))
            assert(request.contains("name=CrashType\r\n\r\n$FATAL_NATIVE_CRASH_TYPE"))
            assert(request.contains("name=CrashTime\r\n\r\n123"))
            assert(request.contains("name=GeckoViewVersion\r\n\r\ntest version"))
            assert(request.contains("name=BuildID\r\n\r\ntest build id"))
            assert(request.contains("name=Version\r\n\r\n136.0.1"))
            assert(request.contains("name=ApplicationBuildID\r\n\r\n1000"))
            assert(request.contains("name=useragent_locale\r\n\r\nen-US"))
            assert(request.contains("name=DistributionID\r\n\r\ntest distribution id"))

            verify(service).report(crash)
            verify(service).sendReport(crash, null, "dump.path", "extras.path", true, true)
        } finally {
            mockWebServer.shutdown()
        }
    }
}
