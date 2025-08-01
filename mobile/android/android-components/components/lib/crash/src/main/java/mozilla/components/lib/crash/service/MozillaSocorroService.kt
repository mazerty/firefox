/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.lib.crash.service

import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.annotation.VisibleForTesting
import androidx.core.content.pm.PackageInfoCompat
import androidx.core.net.toUri
import mozilla.components.concept.base.crash.Breadcrumb
import mozilla.components.lib.crash.Crash
import mozilla.components.lib.crash.CrashReporter
import mozilla.components.lib.crash.service.CrashReport.Annotation
import mozilla.components.support.base.ext.getStacktraceAsJsonString
import mozilla.components.support.base.ext.getStacktraceAsString
import mozilla.components.support.base.log.logger.Logger
import mozilla.components.support.utils.ext.getPackageInfoCompat
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.io.BufferedReader
import java.io.File
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.io.FileReader
import java.io.IOException
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.channels.Channels
import java.util.Locale
import java.util.concurrent.TimeUnit
import java.util.zip.GZIPOutputStream
import kotlin.random.Random
import mozilla.components.Build as AcBuild

// This ID is used for all Mozilla products.  Setting as default if no ID is passed in
private const val MOZILLA_PRODUCT_ID = "{eeb82917-e434-4870-8148-5c03d4caa81b}"

@VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
internal const val CAUGHT_EXCEPTION_TYPE = "caught exception"
internal const val UNCAUGHT_EXCEPTION_TYPE = "uncaught exception"
internal const val FATAL_NATIVE_CRASH_TYPE = "fatal native crash"
internal const val NON_FATAL_NATIVE_CRASH_TYPE = "non-fatal native crash"

internal const val DEFAULT_VERSION_NAME = "N/A"
internal const val DEFAULT_VERSION_CODE = "N/A"
internal const val DEFAULT_VERSION = "N/A"
internal const val DEFAULT_BUILD_ID = "N/A"
internal const val DEFAULT_VENDOR = "N/A"
internal const val DEFAULT_RELEASE_CHANNEL = "N/A"
internal const val DEFAULT_DISTRIBUTION_ID = "N/A"

private const val KEY_CRASH_ID = "CrashID"

private const val MINI_DUMP_FILE_EXT = "dmp"
private const val EXTRAS_FILE_EXT = "extra"
private const val FILE_REGEX = "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\\."

/**
 * A [CrashReporterService] implementation uploading crash reports to crash-stats.mozilla.com.
 *
 * @param applicationContext The application [Context].
 * @param appName A human-readable app name. This name is used on crash-stats.mozilla.com to filter crashes by app.
 *                The name needs to be safelisted for the server to accept the crash.
 *                [File a bug](https://bugzilla.mozilla.org/enter_bug.cgi?product=Socorro) if you would like to get your
 *                app added to the safelist.
 * @param appId The application ID assigned by Socorro server.
 * @param version The engine version.
 * @param buildId The engine build ID.
 * @param vendor The application vendor name.
 * @param serverUrl The URL of the server.
 * @param versionName The version of the application.
 * @param versionCode The version code of the application.
 * @param releaseChannel The release channel of the application.
 * @param distributionId The distribution id of the application.
 */
@Suppress("LargeClass")
class MozillaSocorroService(
    private val applicationContext: Context,
    private val appName: String,
    private val appId: String = MOZILLA_PRODUCT_ID,
    private val version: String = DEFAULT_VERSION,
    private val buildId: String = DEFAULT_BUILD_ID,
    private val vendor: String = DEFAULT_VENDOR,
    @get:VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
    internal val serverUrl: String? = null,
    private var versionName: String = DEFAULT_VERSION_NAME,
    private var versionCode: String = DEFAULT_VERSION_CODE,
    private val releaseChannel: String = DEFAULT_RELEASE_CHANNEL,
    private val distributionId: String = DEFAULT_DISTRIBUTION_ID,
) : CrashReporterService {
    private val logger = Logger("mozac/MozillaSocorroCrashHelperService")
    private val startTime = System.currentTimeMillis()
    private val ignoreKeys = hashSetOf("URL", "ServerURL", "StackTraces")

    override val id: String = "socorro"

    override val name: String = "Socorro"

    override fun createCrashReportUrl(identifier: String): String? {
        return "https://crash-stats.mozilla.org/report/index/$identifier"
    }

    init {
        val packageInfo = try {
            applicationContext.packageManager.getPackageInfoCompat(applicationContext.packageName, 0)
        } catch (e: PackageManager.NameNotFoundException) {
            logger.error("package name not found, failed to get application version")
            null
        }

        packageInfo?.let {
            if (versionName == DEFAULT_VERSION_NAME) {
                try {
                    versionName = packageInfo.versionName ?: DEFAULT_VERSION_NAME
                } catch (e: IllegalStateException) {
                    logger.error("failed to get application version")
                }
            }

            if (versionCode == DEFAULT_VERSION_CODE) {
                try {
                    versionCode = PackageInfoCompat.getLongVersionCode(packageInfo).toString()
                } catch (e: IllegalStateException) {
                    logger.error("failed to get application version code")
                }
            }
        }
    }

    override fun report(crash: Crash.UncaughtExceptionCrash): String? {
        return sendReport(
            crash = crash,
            crash.throwable,
            miniDumpFilePath = null,
            extrasFilePath = null,
            isNativeCodeCrash = false,
            isFatalCrash = true,
        )
    }

    override fun report(crash: Crash.NativeCodeCrash): String? {
        return sendReport(
            crash = crash,
            throwable = null,
            miniDumpFilePath = crash.minidumpPath,
            extrasFilePath = crash.extrasPath,
            isNativeCodeCrash = true,
            isFatalCrash = crash.isFatal,
        )
    }

    override fun report(throwable: Throwable, breadcrumbs: ArrayList<Breadcrumb>): String? {
        // Not sending caught exceptions to Socorro
        return null
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
    internal fun sendReport(
        crash: Crash,
        throwable: Throwable?,
        miniDumpFilePath: String?,
        extrasFilePath: String?,
        isNativeCodeCrash: Boolean,
        isFatalCrash: Boolean,
    ): String? {
        val crashVersionName = crash.runtimeTags[CrashReporter.RELEASE_RUNTIME_TAG] ?: versionName
        val url = URL(serverUrl ?: buildServerUrl(crashVersionName))
        val boundary = generateBoundary()
        var conn: HttpURLConnection? = null

        val breadcrumbsJson = JSONArray()
        for (breadcrumb in crash.breadcrumbs) {
            breadcrumbsJson.put(breadcrumb.toJson())
        }

        try {
            conn = url.openConnection() as HttpURLConnection
            conn.requestMethod = "POST"
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "multipart/form-data; boundary=$boundary")
            conn.setRequestProperty("Content-Encoding", "gzip")

            sendCrashData(
                conn.outputStream, boundary, crash.timestamp, throwable, miniDumpFilePath, extrasFilePath,
                isNativeCodeCrash, isFatalCrash, breadcrumbsJson.toString(), crashVersionName,
            )

            BufferedReader(InputStreamReader(conn.inputStream)).use { reader ->
                val map = parseResponse(reader)

                val id = map?.get(KEY_CRASH_ID)

                if (id != null) {
                    logger.info("Crash reported to Socorro: $id")
                } else {
                    logger.info("Server rejected crash report")
                }

                return id
            }
        } catch (e: IOException) {
            try {
                logger.error("failed to send report to Socorro with " + conn?.responseCode, e)
            } catch (e: IOException) {
                logger.error("failed to send report to Socorro", e)
            }

            return null
        } finally {
            conn?.disconnect()
        }
    }

    private fun parseResponse(reader: BufferedReader): Map<String, String>? {
        val map = mutableMapOf<String, String>()

        reader.readLines().forEach { line ->
            val position = line.indexOf("=")
            if (position != -1) {
                val key = line.substring(0, position)
                val value = unescape(line.substring(position + 1))
                map[key] = value
            }
        }

        return map
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
    internal fun createFormDataWriter(os: OutputStream, boundary: String, logger: Logger) =
        FormDataWriter(os, boundary, logger)

    @Suppress("LongParameterList", "LongMethod", "ComplexMethod")
    private fun sendCrashData(
        os: OutputStream,
        boundary: String,
        timestamp: Long,
        throwable: Throwable?,
        miniDumpFilePath: String?,
        extrasFilePath: String?,
        isNativeCodeCrash: Boolean,
        isFatalCrash: Boolean,
        breadcrumbs: String,
        versionName: String,
    ) {
        val formDataWriter = createFormDataWriter(GZIPOutputStream(os), boundary, logger)
        formDataWriter.sendAnnotation(Annotation.ProductName, appName)
        formDataWriter.sendAnnotation(Annotation.ProductID, appId)
        formDataWriter.sendAnnotation(Annotation.Version, versionName)
        formDataWriter.sendAnnotation(Annotation.ApplicationBuildID, versionCode)
        formDataWriter.sendAnnotation(Annotation.AndroidComponentVersion, AcBuild.VERSION)
        formDataWriter.sendAnnotation(Annotation.GleanVersion, AcBuild.GLEAN_SDK_VERSION)
        formDataWriter.sendAnnotation(Annotation.ApplicationServicesVersion, AcBuild.APPLICATION_SERVICES_VERSION)
        formDataWriter.sendAnnotation(Annotation.GeckoViewVersion, version)
        formDataWriter.sendAnnotation(Annotation.BuildID, buildId)
        formDataWriter.sendAnnotation(Annotation.Vendor, vendor)
        formDataWriter.sendAnnotation(Annotation.Breadcrumbs, breadcrumbs)
        formDataWriter.sendAnnotation(Annotation.useragent_locale, Locale.getDefault().toLanguageTag())
        formDataWriter.sendAnnotation(Annotation.DistributionID, distributionId)

        var additionalDumps: FormDataWriter.AdditionalMinidumps? = null

        extrasFilePath?.let {
            val regex = "$FILE_REGEX$EXTRAS_FILE_EXT".toRegex()
            if (regex.matchEntire(it.substringAfterLast("/")) != null) {
                val extrasFile = File(it)
                val extrasMap = readExtrasFromFile(extrasFile)
                for (key in extrasMap.keys) {
                    formDataWriter.sendPart(key, extrasMap[key])
                }
                additionalDumps = formDataWriter.AdditionalMinidumps(extrasMap)
                extrasFile.delete()
            }
        }

        if (throwable?.stackTrace?.isEmpty() == false) {
            formDataWriter.sendAnnotation(
                Annotation.JavaStackTrace,
                getExceptionStackTrace(
                    throwable,
                    !isNativeCodeCrash && !isFatalCrash,
                ),
            )

            formDataWriter.sendAnnotation(Annotation.JavaException, throwable.getStacktraceAsJsonString())
        }

        miniDumpFilePath?.let {
            val regex = "$FILE_REGEX$MINI_DUMP_FILE_EXT".toRegex()
            if (regex.matchEntire(it.substringAfterLast("/")) != null) {
                formDataWriter.sendAndDeleteFileAtPath("upload_file_minidump", it)
                additionalDumps?.send(it)
            }
        }

        formDataWriter.sendAnnotation(
            Annotation.CrashType,
            if (isNativeCodeCrash) {
                if (isFatalCrash) FATAL_NATIVE_CRASH_TYPE else NON_FATAL_NATIVE_CRASH_TYPE
            } else {
                if (isFatalCrash) UNCAUGHT_EXCEPTION_TYPE else CAUGHT_EXCEPTION_TYPE
            },
        )

        formDataWriter.sendPackageInstallTime(applicationContext)
        formDataWriter.sendProcessName(applicationContext)
        formDataWriter.sendAnnotation(Annotation.ReleaseChannel, releaseChannel)
        formDataWriter.sendAnnotation(
            Annotation.StartupTime,
            TimeUnit.MILLISECONDS.toSeconds(startTime).toString(),
        )
        formDataWriter.sendAnnotation(
            Annotation.CrashTime,
            TimeUnit.MILLISECONDS.toSeconds(timestamp).toString(),
        )
        formDataWriter.sendAnnotation(Annotation.Android_PackageName, applicationContext.packageName)
        formDataWriter.sendAnnotation(Annotation.Android_Manufacturer, Build.MANUFACTURER)
        formDataWriter.sendAnnotation(Annotation.Android_Model, Build.MODEL)
        formDataWriter.sendAnnotation(Annotation.Android_Board, Build.BOARD)
        formDataWriter.sendAnnotation(Annotation.Android_Brand, Build.BRAND)
        formDataWriter.sendAnnotation(Annotation.Android_Device, Build.DEVICE)
        formDataWriter.sendAnnotation(Annotation.Android_Display, Build.DISPLAY)
        formDataWriter.sendAnnotation(Annotation.Android_Fingerprint, Build.FINGERPRINT)
        formDataWriter.sendAnnotation(Annotation.Android_Hardware, Build.HARDWARE)
        formDataWriter.sendAnnotation(
            Annotation.Android_Version,
            "${Build.VERSION.SDK_INT} (${Build.VERSION.CODENAME})",
        )

        if (Build.SUPPORTED_ABIS.isNotEmpty()) {
            formDataWriter.sendAnnotation(Annotation.Android_CPU_ABI, Build.SUPPORTED_ABIS[0])
            if (Build.SUPPORTED_ABIS.size >= 2) {
                formDataWriter.sendAnnotation(Annotation.Android_CPU_ABI2, Build.SUPPORTED_ABIS[1])
            }
        }

        formDataWriter.finish()
    }

    private fun generateBoundary(): String {
        val r0 = Random.nextInt(0, Int.MAX_VALUE)
        val r1 = Random.nextInt(0, Int.MAX_VALUE)
        return String.format("---------------------------%08X%08X", r0, r1)
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
    internal class FormDataWriter(
        private val os: OutputStream,
        private val boundary: String,
        private val logger: Logger,
    ) {
        private val nameSet: MutableSet<String> = mutableSetOf()

        internal inner class AdditionalMinidumps(
            extrasMap: HashMap<String, String>,
        ) {
            private val names = extrasMap[Annotation.additional_minidumps.toString()]?.split(',') ?: listOf()

            fun send(baseMinidumpPath: String) {
                for (suffix in names) {
                    val path = "${baseMinidumpPath.removeSuffix(".$MINI_DUMP_FILE_EXT")}-$suffix.$MINI_DUMP_FILE_EXT"
                    sendAndDeleteFileAtPath("upload_file_minidump_$suffix", path)
                }
            }
        }

        fun sendAnnotation(
            annotation: Annotation,
            data: String?,
        ) {
            sendPart(annotation.toString(), data)
        }

        fun sendPart(name: String, data: String?) {
            if (data == null) {
                return
            }

            if (nameSet.contains(name)) {
                return
            } else {
                nameSet.add(name)
            }

            try {
                os.write(
                    (
                        "--$boundary\r\nContent-Disposition: form-data; " +
                            "name=$name\r\n\r\n$data\r\n"
                        ).toByteArray(),
                )
            } catch (e: IOException) {
                logger.error("Exception when sending $name", e)
            }
        }

        fun sendAndDeleteFileAtPath(name: String, path: String) {
            val file = File(path)
            sendFile(name, file)
            file.delete()
        }

        fun sendFile(
            name: String,
            file: File,
        ) {
            if (nameSet.contains(name)) {
                return
            } else {
                nameSet.add(name)
            }

            if (!file.exists()) {
                logger.error("failed to send file for $name as ${file.path} doesn't exist")
                return
            }

            try {
                os.write(
                    (
                        "--${boundary}\r\n" +
                            "Content-Disposition: form-data; name=\"$name\"; " +
                            "filename=\"${file.getName()}\"\r\n" +
                            "Content-Type: application/octet-stream\r\n\r\n"
                        ).toByteArray(),
                )
            } catch (e: IOException) {
                logger.error("failed to write boundary", e)
                return
            }

            try {
                val fileInputStream = FileInputStream(file).channel
                fileInputStream.transferTo(0, fileInputStream.size(), Channels.newChannel(os))
                fileInputStream.close()
            } catch (e: IOException) {
                logger.error("failed to send file", e)
            }

            try {
                // Add EOL to separate from the next part
                os.write("\r\n".toByteArray())
            } catch (e: IOException) {
                logger.error("failed to write EOL", e)
            }
        }

        fun sendProcessName(applicationContext: Context) {
            val pid = android.os.Process.myPid()
            val manager = applicationContext.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            manager.runningAppProcesses.filter { it.pid == pid }.forEach {
                sendAnnotation(Annotation.Android_ProcessName, it.processName)
                return
            }
        }

        fun sendPackageInstallTime(applicationContext: Context) {
            val packageManager = applicationContext.packageManager
            try {
                val packageInfo = packageManager.getPackageInfoCompat(applicationContext.packageName, 0)
                sendAnnotation(
                    Annotation.InstallTime,
                    TimeUnit.MILLISECONDS.toSeconds(
                        packageInfo.lastUpdateTime,
                    ).toString(),
                )
            } catch (e: PackageManager.NameNotFoundException) {
                logger.error("Error getting package info", e)
            }
        }

        fun finish() {
            os.write(("\r\n--$boundary--\r\n").toByteArray())
            os.flush()
            os.close()
        }
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
    internal fun unescape(string: String): String {
        return string.replace("\\\\\\\\", "\\").replace("\\\\n", "\n").replace("\\\\t", "\t")
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
    internal fun jsonUnescape(string: String): String {
        return string.replace("""\\\\""", "\\").replace("""\n""", "\n").replace("""\t""", "\t")
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
    @Suppress("NestedBlockDepth")
    internal fun readExtrasFromLegacyFile(file: File): HashMap<String, String> {
        var fileReader: FileReader? = null
        var bufReader: BufferedReader? = null
        var line: String?
        val map = HashMap<String, String>()

        try {
            fileReader = FileReader(file)
            bufReader = BufferedReader(fileReader)
            line = bufReader.readLine()
            while (line != null) {
                val equalsPos = line.indexOf('=')
                if ((equalsPos) != -1) {
                    val key = line.substring(0, equalsPos)
                    val value = unescape(line.substring(equalsPos + 1))
                    if (!ignoreKeys.contains(key)) {
                        map[key] = value
                    }
                }
                line = bufReader.readLine()
            }
        } catch (e: IOException) {
            logger.error("failed to convert extras to map", e)
        } finally {
            try {
                fileReader?.close()
                bufReader?.close()
            } catch (e: IOException) {
                // do nothing
            }
        }

        return map
    }

    @VisibleForTesting(otherwise = VisibleForTesting.PRIVATE)
    @Suppress("NestedBlockDepth")
    internal fun readExtrasFromFile(file: File): HashMap<String, String> {
        var resultMap = HashMap<String, String>()
        var notJson = false

        try {
            FileReader(file).use { fileReader ->
                val input = fileReader.readLines().firstOrNull()
                    ?: throw JSONException("failed to read json file")

                val jsonObject = JSONObject(input)
                for (key in jsonObject.keys()) {
                    if (!key.isNullOrEmpty() && !ignoreKeys.contains(key)) {
                        resultMap[key] = jsonUnescape(jsonObject.getString(key))
                    }
                }
            }
        } catch (e: FileNotFoundException) {
            logger.error("failed to find extra file", e)
        } catch (e: IOException) {
            logger.error("failed read the extra file", e)
        } catch (e: JSONException) {
            logger.info("extras file JSON syntax error, trying legacy format")
            notJson = true
        }

        if (notJson) {
            resultMap = readExtrasFromLegacyFile(file)
        }

        return resultMap
    }

    @Suppress("TooGenericExceptionCaught")
    // printStackTrace() can throw a NullPointerException exception even if throwable is not null
    private fun getExceptionStackTrace(throwable: Throwable, isCaughtException: Boolean): String? {
        return try {
            when (isCaughtException) {
                true -> "$LIB_CRASH_INFO_PREFIX ${throwable.getStacktraceAsString()}"
                false -> throwable.getStacktraceAsString()
            }
        } catch (e: NullPointerException) {
            null
        }
    }

    internal fun buildServerUrl(versionName: String): String =
        "https://crash-reports.mozilla.com/submit".toUri()
            .buildUpon()
            .appendQueryParameter("id", appId)
            .appendQueryParameter("version", versionName)
            .appendQueryParameter("android_component_version", AcBuild.VERSION)
            .build().toString()
}
