package com.cartucho.emu

import android.content.Context
import android.net.Uri
import org.apache.commons.compress.archivers.sevenz.SevenZFile
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.util.zip.ZipInputStream

/**
 * Games inside .zip and .7z files.
 *
 * The library peeks at an archive's file list (cheap: a zip's first local
 * header, a 7z's header) to find the game in it. Playing one extracts that
 * entry once into the app's storage; later launches reuse the extracted copy
 * as long as the archive did not change.
 */
object Archives {
    fun isArchive(name: String) = name.endsWith(".zip", true) || name.endsWith(".7z", true)

    data class Entry(val name: String, val size: Long)

    /** The first game in the archive, or null. */
    fun peek(context: Context, uri: Uri, archiveName: String): Entry? = try {
        if (archiveName.endsWith(".7z", true)) peek7z(context, uri) else peekZip(context, uri)
    } catch (e: Exception) {
        null
    } catch (e: OutOfMemoryError) {
        null
    }

    private fun isGame(name: String) =
        !name.endsWith("/") && GameSystem.forFileName(name.substringAfterLast('/')) != null

    private fun peekZip(context: Context, uri: Uri): Entry? =
        context.contentResolver.openInputStream(uri)?.use { raw ->
            ZipInputStream(raw.buffered()).use { zip ->
                // Most ROM zips hold a single file; look at the first few.
                repeat(8) {
                    val e = zip.nextEntry ?: return null
                    if (isGame(e.name)) return Entry(e.name, e.size)
                }
                null
            }
        }

    private fun peek7z(context: Context, uri: Uri): Entry? =
        context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
            FileInputStream(pfd.fileDescriptor).channel.use { channel ->
                SevenZFile.builder().setSeekableByteChannel(channel).get().use { sz ->
                    sz.entries.firstOrNull { !it.isDirectory && isGame(it.name) }
                        ?.let { Entry(it.name, it.size) }
                }
            }
        }

    /** Where an extracted game lives; reused while the archive is unchanged. */
    fun extractedFile(context: Context, game: Game): File {
        val dir = File(context.filesDir, "extracted").apply { mkdirs() }
        val tag = Integer.toHexString((game.uri + "|" + game.size).hashCode())
        return File(dir, "$tag-" + game.fileName.substringAfterLast('/').replace('/', '_'))
    }

    /**
     * Extracts the game, reporting progress 0..1. Keeps only the most recent
     * extraction to save space (3DS and DS games are large).
     */
    fun extract(context: Context, game: Game, onProgress: (Float) -> Unit): File {
        val target = extractedFile(context, game)
        if (target.exists() && target.length() > 0) return target
        target.parentFile?.listFiles()?.forEach { it.delete() }
        val tmp = File(target.path + ".part")
        val uri = Uri.parse(game.uri)
        val entry = game.entry ?: error("not an archive")
        if (game.archive.endsWith(".7z", true)) {
            context.contentResolver.openFileDescriptor(uri, "r")!!.use { pfd ->
                FileInputStream(pfd.fileDescriptor).channel.use { channel ->
                    SevenZFile.builder().setSeekableByteChannel(channel).get().use { sz ->
                        var e = sz.nextEntry
                        while (e != null && e.name != entry) e = sz.nextEntry
                        requireNotNull(e) { "game not found in archive" }
                        val total = e.size
                        tmp.outputStream().use { out ->
                            val buf = ByteArray(1 shl 16)
                            var done = 0L
                            while (true) {
                                val n = sz.read(buf)
                                if (n < 0) break
                                out.write(buf, 0, n)
                                done += n
                                if (total > 0) onProgress(done.toFloat() / total)
                            }
                        }
                    }
                }
            }
        } else {
            context.contentResolver.openInputStream(uri)!!.use { raw ->
                ZipInputStream(raw.buffered()).use { zip ->
                    var e = zip.nextEntry
                    while (e != null && e.name != entry) e = zip.nextEntry
                    requireNotNull(e) { "game not found in archive" }
                    val total = if (e.size > 0) e.size else game.size
                    tmp.outputStream().use { out -> copy(zip, out.buffered(), total, onProgress) }
                }
            }
        }
        if (!tmp.renameTo(target)) error("could not save the extracted game")
        return target
    }

    private fun copy(input: InputStream, out: java.io.OutputStream, total: Long, onProgress: (Float) -> Unit) {
        val buf = ByteArray(1 shl 16)
        var done = 0L
        while (true) {
            val n = input.read(buf)
            if (n < 0) break
            out.write(buf, 0, n)
            done += n
            if (total > 0) onProgress((done.toFloat() / total).coerceAtMost(1f))
        }
        out.flush()
    }
}
