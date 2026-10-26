package com.cartucho.emu

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import android.widget.ImageView
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Executors

/**
 * Box art for the library, looked up by file name in the libretro thumbnail
 * collection (which uses No-Intro names, the usual naming of game dumps).
 * Downloaded once and kept on disk; misses are remembered for a week so a
 * game without art does not hit the network on every launch.
 */
class Covers(context: Context) {
    private val dir = File(context.cacheDir, "covers").apply { mkdirs() }
    private val memory = object : LruCache<String, Bitmap>(24 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }
    private val pool = Executors.newFixedThreadPool(3)
    private val main = Handler(Looper.getMainLooper())
    private val prefs = Prefs(context)

    fun load(game: Game, into: ImageView, onResult: (Boolean) -> Unit) {
        val key = key(game)
        into.tag = key
        memory.get(key)?.let {
            into.setImageBitmap(it)
            onResult(true)
            return
        }
        into.setImageDrawable(null)
        onResult(false)
        pool.execute {
            val bmp = fetch(game, key)
            main.post {
                if (into.tag != key) return@post  // view recycled for another game
                if (bmp != null) {
                    memory.put(key, bmp)
                    into.setImageBitmap(bmp)
                }
                onResult(bmp != null)
            }
        }
    }

    private fun key(game: Game) = game.system.name + "/" + sanitize(game.fileName.substringBeforeLast('.'))

    private fun fetch(game: Game, key: String): Bitmap? {
        val file = File(dir, key.replace('/', '_') + ".png")
        val miss = File(file.path + ".none")
        if (file.exists()) return decode(file)
        if (!prefs.downloadCovers) return null
        if (miss.exists() && System.currentTimeMillis() - miss.lastModified() < 7L * 24 * 3600 * 1000) return null
        for (folder in remoteFolders(game)) {
            val name = sanitize(game.fileName.substringBeforeLast('.'))
            val url = "https://thumbnails.libretro.com/" + enc(folder) + "/Named_Boxarts/" + enc(name) + ".png"
            try {
                val conn = URL(url).openConnection() as HttpURLConnection
                conn.connectTimeout = 8000
                conn.readTimeout = 15000
                if (conn.responseCode == 200) {
                    val tmp = File(file.path + ".tmp")
                    conn.inputStream.use { input -> tmp.outputStream().use { input.copyTo(it) } }
                    tmp.renameTo(file)
                    return decode(file)
                }
                conn.disconnect()
            } catch (e: Exception) {
                return null  // offline: try again next time, don't record a miss
            }
        }
        miss.writeText("")
        return null
    }

    private fun remoteFolders(game: Game): List<String> = when (game.system) {
        GameSystem.GBA -> listOf("Nintendo - Game Boy Advance")
        GameSystem.GB -> if (game.fileName.endsWith(".gbc", true))
            listOf("Nintendo - Game Boy Color", "Nintendo - Game Boy")
        else listOf("Nintendo - Game Boy", "Nintendo - Game Boy Color")
        GameSystem.N3DS -> listOf("Nintendo - Nintendo 3DS")
    }

    private fun decode(file: File): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.path, bounds)
        var sample = 1
        while (bounds.outWidth / (sample * 2) >= 360) sample *= 2
        return BitmapFactory.decodeFile(file.path, BitmapFactory.Options().apply { inSampleSize = sample })
    }

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8").replace("+", "%20")

    companion object {
        /** The thumbnail collection replaces these characters with '_'. */
        fun sanitize(name: String) = name.replace(Regex("[&*/:`<>?\\\\|\"]"), "_")
    }
}
