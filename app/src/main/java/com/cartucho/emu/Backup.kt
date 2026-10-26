package com.cartucho.emu

import android.content.Context
import android.net.Uri
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Saves backup: one .zip with battery saves, save states, cheats, chosen
 * covers and imported BIOS files. Written and read through the system file
 * picker, so it can go to the phone, an SD card, Google Drive or any other
 * storage app.
 */
object Backup {
    /** Folders under files/ that belong in a backup. */
    private val FOLDERS = listOf("saves", "states", "cheats", "covers_custom", "system")
    private const val MARKER = "cartucho-backup.txt"

    data class Result(val files: Int, val bytes: Long)

    fun export(context: Context, target: Uri): Result {
        var files = 0
        var bytes = 0L
        val root = context.filesDir
        context.contentResolver.openOutputStream(target, "wt")!!.use { raw ->
            ZipOutputStream(raw.buffered()).use { zip ->
                zip.putNextEntry(ZipEntry(MARKER))
                zip.write("Cartucho ${BuildConfig.VERSION_NAME}\n".toByteArray())
                zip.closeEntry()
                for (folder in FOLDERS) {
                    val dir = File(root, folder)
                    if (!dir.exists()) continue
                    dir.walkTopDown().filter { it.isFile && !it.name.endsWith(".tmp") }.forEach { f ->
                        zip.putNextEntry(ZipEntry(f.relativeTo(root).path))
                        f.inputStream().use { it.copyTo(zip) }
                        zip.closeEntry()
                        files++
                        bytes += f.length()
                    }
                }
            }
        }
        return Result(files, bytes)
    }

    /** Restores a backup over the current files. Throws if it is not one. */
    fun restore(context: Context, source: Uri): Result {
        val root = context.filesDir.canonicalFile
        // First pass: make sure this is a Cartucho backup before touching anything.
        var marked = false
        context.contentResolver.openInputStream(source)!!.use { raw ->
            ZipInputStream(raw.buffered()).use { zip ->
                var e = zip.nextEntry
                while (e != null) {
                    if (e.name == MARKER) marked = true
                    e = zip.nextEntry
                }
            }
        }
        require(marked) { "Esse arquivo não é um backup do Cartucho." }

        var files = 0
        var bytes = 0L
        context.contentResolver.openInputStream(source)!!.use { raw ->
            ZipInputStream(raw.buffered()).use { zip ->
                var e = zip.nextEntry
                while (e != null) {
                    val name = e.name
                    val top = name.substringBefore('/')
                    if (!e.isDirectory && top in FOLDERS) {
                        val out = File(root, name).canonicalFile
                        // Never write outside the app's folders (malformed zips).
                        if (out.path.startsWith(root.path + File.separator)) {
                            out.parentFile?.mkdirs()
                            val tmp = File(out.path + ".tmp")
                            tmp.outputStream().use { zip.copyTo(it) }
                            tmp.renameTo(out)
                            files++
                            bytes += out.length()
                        }
                    }
                    e = zip.nextEntry
                }
            }
        }
        return Result(files, bytes)
    }

    fun defaultName(): String =
        "Cartucho-backup-" + java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US).format(java.util.Date()) + ".zip"
}
