package com.cartucho.emu

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

/**
 * Play time and "continue where you left off" for each game.
 *
 * The game runs in its own process, so this lives in small files rather than
 * SharedPreferences (which do not see each other's changes across processes).
 */
object PlayStats {
    private fun dir(context: Context) = File(context.filesDir, "play").apply { mkdirs() }

    private fun id(game: Game): String {
        val d = MessageDigest.getInstance("SHA-1").digest(game.uri.toByteArray())
        return d.take(10).joinToString("") { "%02x".format(it) }
    }

    private fun statsFile(context: Context, game: Game) = File(dir(context), id(game) + ".json")

    private fun read(context: Context, game: Game): JSONObject =
        runCatching { JSONObject(statsFile(context, game).readText()) }.getOrElse { JSONObject() }

    /** Total time played, in milliseconds. */
    fun playTime(context: Context, game: Game): Long = read(context, game).optLong("ms")

    /** When the game was last left (ms since epoch), 0 if never. */
    fun lastPlayed(context: Context, game: Game): Long = read(context, game).optLong("last")

    @Synchronized
    fun addPlayTime(context: Context, game: Game, ms: Long) {
        if (ms <= 0) return
        val o = read(context, game)
        o.put("ms", o.optLong("ms") + ms).put("last", System.currentTimeMillis())
        val f = statsFile(context, game)
        val tmp = File(f.path + ".tmp")
        tmp.writeText(o.toString())
        tmp.renameTo(f)
    }

    // --- Continue where you left off --------------------------------------------

    /** Systems whose states are quick enough to save every time you leave. */
    fun canResume(system: GameSystem) = system != GameSystem.N3DS

    fun resumeState(context: Context, game: Game): File =
        File(context.filesDir, "states/${game.system.name}/${game.saveName}.stateAuto")

    fun resumeThumb(context: Context, game: Game): File = File(dir(context), id(game) + ".png")

    fun hasResume(context: Context, game: Game) =
        canResume(game.system) && resumeState(context, game).let { it.exists() && it.length() > 0 }

    fun clearResume(context: Context, game: Game) {
        resumeState(context, game).delete()
        resumeThumb(context, game).delete()
    }

    /** Saves the picture shown when leaving: ARGB pixels as from NativeBridge.screenshot(). */
    fun saveThumb(context: Context, game: Game, data: IntArray?) {
        if (data == null || data.size < 2) return
        val w = data[0]
        val h = data[1]
        if (w <= 0 || h <= 0 || data.size < 2 + w * h) return
        runCatching {
            val bmp = Bitmap.createBitmap(data, 2, w, w, h, Bitmap.Config.ARGB_8888)
            // Small pictures stay crisp when enlarged with nearest-neighbour;
            // big ones (DS, upscaled) are shrunk to keep the file light.
            val scale = minOf(1f, 480f / maxOf(w, h))
            val out = if (scale < 1f) Bitmap.createScaledBitmap(bmp, (w * scale).toInt(), (h * scale).toInt(), true) else bmp
            val f = resumeThumb(context, game)
            val tmp = File(f.path + ".tmp")
            tmp.outputStream().use { out.compress(Bitmap.CompressFormat.PNG, 100, it) }
            tmp.renameTo(f)
        }
    }

    fun thumb(context: Context, game: Game): Bitmap? =
        resumeThumb(context, game).takeIf { it.exists() }?.let { BitmapFactory.decodeFile(it.path) }

    /** "3 h 20 min", "45 min", "menos de 1 min". */
    fun formatTime(ms: Long): String {
        val minutes = ms / 60000
        return when {
            minutes < 1 -> "menos de 1 min"
            minutes < 60 -> "$minutes min"
            minutes % 60 == 0L -> "${minutes / 60} h"
            else -> "${minutes / 60} h ${minutes % 60} min"
        }
    }

    /** "agora há pouco", "há 3 h", "ontem", "há 5 dias". */
    fun formatAgo(time: Long): String {
        val d = System.currentTimeMillis() - time
        val h = d / 3_600_000
        return when {
            d < 5 * 60_000 -> "agora há pouco"
            h < 1 -> "há ${d / 60_000} min"
            h < 24 -> "há $h h"
            h < 48 -> "ontem"
            else -> "há ${h / 24} dias"
        }
    }
}
