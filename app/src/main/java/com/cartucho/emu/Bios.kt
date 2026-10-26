package com.cartucho.emu

import android.content.Context
import android.net.Uri
import java.io.File

/**
 * Optional BIOS images the player dumped from their own console. None ship
 * with the app (they are Nintendo's copyrighted code); the cores fall back to
 * their built-in replacements when a file is missing. Imported files go to
 * the system folder under the names the VBA-M core looks for.
 */
enum class Bios(val key: String, val title: String, val fileName: String, val size: Long) {
    GBA("bios_gba", "BIOS do Game Boy Advance", "gba_bios.bin", 16384),
    GB("bios_gb", "BIOS do Game Boy", "gb_bios.bin", 256),
    GBC("bios_gbc", "BIOS do Game Boy Color", "gbc_bios.bin", 2304);

    enum class Result { OK, WRONG_SIZE, ERROR }

    fun file(context: Context) = File(systemDir(context), fileName)

    fun installed(context: Context) = file(context).length() == size

    fun import(context: Context, uri: Uri): Result = try {
        val data = context.contentResolver.openInputStream(uri)?.use { input ->
            // Read one byte more than expected so oversized files are caught.
            val buf = ByteArray((size + 1).toInt())
            var n = 0
            while (n < buf.size) {
                val r = input.read(buf, n, buf.size - n)
                if (r < 0) break
                n += r
            }
            buf.copyOf(n)
        } ?: throw IllegalStateException("unreadable")
        if (data.size.toLong() != size) Result.WRONG_SIZE
        else {
            file(context).writeBytes(data)
            Result.OK
        }
    } catch (e: Exception) {
        Result.ERROR
    }

    fun remove(context: Context) {
        file(context).delete()
    }

    companion object {
        fun systemDir(context: Context) = File(context.filesDir, "system").apply { mkdirs() }

        /** Whether the core should look for a real BIOS for this console. */
        fun anyFor(context: Context, system: GameSystem) = when (system) {
            GameSystem.GBA -> GBA.installed(context)
            GameSystem.GB -> GB.installed(context) || GBC.installed(context)
            GameSystem.NDS, GameSystem.N3DS -> false
        }
    }
}
