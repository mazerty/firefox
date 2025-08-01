/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.support.utils

import android.webkit.MimeTypeMap
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class DownloadUtilsTest {

    @Rule @JvmField
    val folder = TemporaryFolder()

    private fun assertContentDisposition(expected: String, contentDisposition: String) {
        assertEquals(
            expected,
            DownloadUtils.guessFileName(
                contentDisposition = contentDisposition,
                destinationDirectory = folder.root.path,
                url = null,
                mimeType = null,
            ),
        )
    }

    @Test
    fun guessFileName_contentDisposition() {
        // Default file name
        assertContentDisposition("downloadfile.bin", "")

        CONTENT_DISPOSITION_TYPES.forEach { contentDisposition ->
            // continuing with default filenames
            assertContentDisposition("downloadfile.bin", contentDisposition)
            assertContentDisposition("downloadfile.bin", "$contentDisposition;")
            assertContentDisposition("downloadfile.bin", "$contentDisposition; filename")
            assertContentDisposition("downloadfile.bin", "$contentDisposition; filename=")
            assertContentDisposition("downloadfile.bin", "$contentDisposition; filename=\"\"")
            assertContentDisposition("downloadfile.bin", "$contentDisposition; filename=.bin")

            // Provided filename field
            assertContentDisposition("filename.jpg", "$contentDisposition; filename=\"filename.jpg\"")
            assertContentDisposition("file\"name.jpg", "$contentDisposition; filename=\"file\\\"name.jpg\"")
            assertContentDisposition("file\\name.jpg", "$contentDisposition; filename=\"file\\\\name.jpg\"")
            assertContentDisposition("file\\\"name.jpg", "$contentDisposition; filename=\"file\\\\\\\"name.jpg\"")
            assertContentDisposition("filename.jpg", "$contentDisposition; filename=filename.jpg")
            assertContentDisposition("filename.jpg", "$contentDisposition; filename=filename.jpg; foo")
            assertContentDisposition("filename.jpg", "$contentDisposition; filename=\"filename.jpg\"; foo")
            assertContentDisposition("file\nname.jpg", "$contentDisposition; filename=\"file%0Aname.jpg\"; foo")

            // UTF-8 encoded filename* field
            assertContentDisposition(
                "\uD83E\uDD8A + x.jpg",
                "$contentDisposition; filename=\"_.jpg\"; filename*=utf-8'en'%F0%9F%A6%8A%20+%20x.jpg",
            )
            assertContentDisposition(
                "filename 的副本.jpg",
                contentDisposition + ";filename=\"_.jpg\";" +
                    "filename*=UTF-8''filename%20%E7%9A%84%E5%89%AF%E6%9C%AC.jpg",
            )
            assertContentDisposition(
                "filename.jpg",
                "$contentDisposition; filename=_.jpg; filename*=utf-8'en'filename.jpg",
            )
            // Wrong order of the "filename*" segment
            assertContentDisposition(
                "filename.jpg",
                "$contentDisposition; filename*=utf-8'en'filename.jpg; filename=_.jpg",
            )
            // Semicolon at the end
            assertContentDisposition(
                "filename.jpg",
                "$contentDisposition; filename*=utf-8'en'filename.jpg; foo",
            )

            // ISO-8859-1 encoded filename* field
            assertContentDisposition(
                "file' 'name.jpg",
                "$contentDisposition; filename=\"_.jpg\"; filename*=iso-8859-1'en'file%27%20%27name.jpg",
            )

            assertContentDisposition("success.html", "$contentDisposition; filename*=utf-8''success.html; foo")
            assertContentDisposition("success.html", "$contentDisposition; filename*=utf-8''success.html")
        }
    }

    @Test
    fun uniqueFilenameNoExtension() {
        assertEquals("test", DownloadUtils.uniqueFileName(folder.root, "test"))

        folder.newFile("test")
        assertEquals("test(1)", DownloadUtils.uniqueFileName(folder.root, "test"))

        folder.newFile("test(1)")
        assertEquals("test(2)", DownloadUtils.uniqueFileName(folder.root, "test"))
    }

    @Test
    fun uniqueFilename() {
        assertEquals("test.zip", DownloadUtils.uniqueFileName(folder.root, "test.zip"))

        folder.newFile("test.zip")
        assertEquals("test(1).zip", DownloadUtils.uniqueFileName(folder.root, "test.zip"))

        folder.newFile("test(1).zip")
        assertEquals("test(2).zip", DownloadUtils.uniqueFileName(folder.root, "test.zip"))
    }

    @Test
    fun guessFilename() {
        assertEquals(
            "test.zip",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/test.zip",
                mimeType = "application/zip",
            ),
        )

        folder.newFile("test.zip")
        assertEquals(
            "test(1).zip",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/test.zip",
                mimeType = "application/zip",
            ),
        )

        folder.newFile("test(1).zip")
        assertEquals(
            "test(2).zip",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/test.zip",
                mimeType = "application/zip",
            ),
        )
    }

    @Test
    fun guessFileName_url() {
        assertUrl(
            destinationDirectory = folder.root.path,
            expected = "downloadfile.bin",
            url = "http://example.com/",
        )
        assertUrl(
            destinationDirectory = folder.root.path,
            expected = "downloadfile.bin",
            url = "http://example.com/filename/",
        )
        assertUrl(
            destinationDirectory = folder.root.path,
            expected = "filename.jpg",
            url = "http://example.com/filename.jpg",
        )
        assertUrl(
            destinationDirectory = folder.root.path,
            expected = "filename.jpg",
            url = "http://example.com/foo/bar/filename.jpg",
        )
    }

    @Test
    fun guessFileName_mimeType() {
        // Matches the first extension from official mapping: application/x-msdos-program -> com exe bat dll
        assertEquals(
            "com",
            MimeTypeMap.getSingleton().getExtensionFromMimeType("application/x-msdos-program"),
        )
        assertEquals(
            "application/x-msdos-program",
            MimeTypeMap.getSingleton().getMimeTypeFromExtension("exe"),
        )
        assertEquals(
            "application/x-msdos-program",
            MimeTypeMap.getSingleton().getMimeTypeFromExtension("dll"),
        )
        assertEquals(
            "application/x-msdos-program",
            MimeTypeMap.getSingleton().getMimeTypeFromExtension("bat"),
        )

        assertEquals(
            "file.jpg",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.jpg",
                mimeType = "image/jpeg",
            ),
        )

        // This is difference with URLUtil.guessFileName
        assertEquals(
            "file.bin.jpg",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.bin",
                mimeType = "image/jpeg",
            ),
        )

        assertEquals(
            "Caesium-wahoo-v3.6-b792615ced1b.zip",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "https://download.msfjarvis.website/caesium/wahoo/beta/Caesium-wahoo-v3.6-b792615ced1b.zip",
                mimeType = "application/zip",
            ),
        )
        assertEquals(
            "compressed.TAR.GZ",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/compressed.TAR.GZ",
                mimeType = "application/gzip",
            ),
        )
        assertEquals(
            "file.html",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file?abc",
                mimeType = "text/html",
            ),
        )
        assertEquals(
            "file.html",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file",
                mimeType = "text/html",
            ),
        )
        assertEquals(
            "file.html",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file",
                mimeType = "text/html; charset=utf-8",
            ),
        )
        assertEquals(
            "file.txt.html",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.txt",
                mimeType = "text/html",
            ),
        )
        assertEquals(
            "file.txt",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.txt",
                mimeType = "text/plain",
            ),
        )
        assertEquals(
            "file.data",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.data",
                mimeType = "application/octet-stream",
            ),
        )
        assertEquals(
            "file.data",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.data",
                mimeType = "binary/octet-stream",
            ),
        )
        assertEquals(
            "file.data",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.data",
                mimeType = "application/unknown",
            ),
        )

        assertEquals(
            "file.zip.jpg",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.zip",
                mimeType = "image/jpeg",
            ),
        )

        // extra information in content-type
        assertEquals(
            "file.jpg",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.jpg",
                mimeType = "application/octet-stream; Charset=utf-8",
            ),
        )

        // Should not change to file.dll
        assertEquals(
            "file.exe",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.exe",
                mimeType = "application/x-msdos-program",
            ),
        )
        assertEquals(
            "file.exe",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.exe",
                mimeType = "application/vnd.microsoft.portable-executable",
            ),
        )

        // application/x-pdf with .pdf
        assertEquals(
            "file.pdf",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.pdf",
                mimeType = "application/x-pdf",
            ),
        )

        // application/x-pdf without extension
        assertEquals(
            "downloadfile.pdf",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/downloadfile",
                mimeType = "application/x-pdf",
            ),
        )

        // application/x-pdf with non-pdf extension
        assertEquals(
            "file.bin.pdf",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.bin",
                mimeType = "application/x-pdf",
            ),
        )

        assertEquals(
            "file.bin.com",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                destinationDirectory = folder.root.path,
                url = "http://example.com/file.bin",
                mimeType = "application/x-msdos-program",
            ),
        )

        assertEquals(
            "file.apks.zip",
            DownloadUtils.guessFileName(
                contentDisposition = null,
                url = "http://example.com/file.apks",
                mimeType = "application/zip",
            ),
        )
    }

    @Test
    fun sanitizeMimeType() {
        assertEquals("application/pdf", DownloadUtils.sanitizeMimeType("application/pdf; qs=0.001"))
        assertEquals("application/pdf", DownloadUtils.sanitizeMimeType("application/pdf"))
        assertEquals(null, DownloadUtils.sanitizeMimeType(null))
    }

    @Test
    fun makePdfContentDisposition() {
        assertEquals("attachment; filename=foo.pdf;", DownloadUtils.makePdfContentDisposition("foo"))
        assertEquals("attachment; filename=foo.html.pdf;", DownloadUtils.makePdfContentDisposition("foo.html"))
        assertEquals("attachment; filename=foo.pdf;", DownloadUtils.makePdfContentDisposition("foo.pdf"))
        assertEquals("attachment; filename=${"a".repeat(251)}.pdf;", DownloadUtils.makePdfContentDisposition("a".repeat(260)))
        assertEquals("attachment; filename=${"a".repeat(251)}.pdf;", DownloadUtils.makePdfContentDisposition("a".repeat(260) + ".pdf"))
    }

    @Test
    fun truncateFileNameTest() {
        val path = "/storage/emulated/0/Download"
        val customPath = "/storage/emulated/0/MyCustomFolder"
        // Construct long file names with a specific number of characters
        val longBaseName = "a".repeat(300)

        // Standard cases
        assertEquals(Pair("foo", ".txt"), DownloadUtils.truncateFileName("foo", ".txt", path))
        assertEquals(Pair("foo", ""), DownloadUtils.truncateFileName("foo", "", path))
        assertEquals(Pair("foo.txt", "txt"), DownloadUtils.truncateFileName("foo.txt", "txt", path))
        assertEquals(
            Pair("a".repeat(30), "b".repeat(30)),
            DownloadUtils.truncateFileName("a".repeat(30), "b".repeat(30), customPath),
        )

        // When the base filename is too long
        assertEquals(
            Pair("a".repeat(218), "txt"),
            DownloadUtils.truncateFileName(longBaseName, "txt", path),
        )
        assertEquals(
            Pair("a".repeat(222), ""),
            DownloadUtils.truncateFileName(longBaseName, "", path),
        )
        assertEquals(
            Pair("a".repeat(212), "txt"),
            DownloadUtils.truncateFileName(longBaseName, "txt", customPath),
        )

        // Multi dot name cases
        // Keep the full filename if it is not too long
        assertEquals(
            Pair("multi.dot.filename", "superlongextension"),
            DownloadUtils.truncateFileName("multi.dot.filename", "superlongextension", path),
        )
        // When a full multi-dot file name length is too long and the extension is too long, only keep what is before the first dot
        assertEquals(
            Pair("a".repeat(100), ""),
            DownloadUtils.truncateFileName("${"a".repeat(100)}.${"b".repeat(100)}", "c".repeat(100), customPath),
        )

        // When the extension is too long and the base file name exceeds total characters allowed,
        // remove the extension and truncate the base file name
        assertEquals(
            Pair("a".repeat(222), ""),
            DownloadUtils.truncateFileName(longBaseName, "b".repeat(50), path),
        )
        assertEquals(
            Pair("a".repeat(216), ""),
            DownloadUtils.truncateFileName(longBaseName, "b".repeat(50), customPath),
        )
    }

    @Test
    fun `WHEN calling createFileName with any set of parameters THEN it constructs the fileName correctly`() {
        assertEquals("a", DownloadUtils.createFileName("a"))
        assertEquals("a.c", DownloadUtils.createFileName("a", null, "c"))
        assertEquals("a(1).c", DownloadUtils.createFileName("a", 1, "c"))
        assertEquals("a(1)", DownloadUtils.createFileName("a", 1))
        assertEquals("a(1)", DownloadUtils.createFileName("a", 1, ""))
    }

    companion object {
        private val CONTENT_DISPOSITION_TYPES = listOf("attachment", "inline")

        private fun assertUrl(destinationDirectory: String, expected: String, url: String) {
            assertEquals(
                expected,
                DownloadUtils.guessFileName(
                    contentDisposition = null,
                    destinationDirectory = destinationDirectory,
                    url = url,
                    mimeType = null,
                ),
            )
        }
    }
}
