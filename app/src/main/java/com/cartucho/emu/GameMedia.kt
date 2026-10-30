package com.cartucho.emu

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * Pictures and video of a game for the console-style list: a screenshot,
 * the title screen, the logo, a background picture and a gameplay video.
 *
 * - ScreenScraper, when the player entered an account, has all of them
 *   (videos only with it).
 * - The libretro thumbnail collection has screenshots and title screens
 *   for almost every game, without an account.
 *
 * Everything is kept on disk; a missing kind is remembered for a few days.
 */
class GameMedia(private val context: Context, private val covers: Covers) {
    data class Media(
        val snap: File? = null,
        val title: File? = null,
        val logo: File? = null,
        val fanart: File? = null,
        val video: File? = null,
    ) {
        /** Pictures for the slideshow, best first. */
        val pictures get() = listOfNotNull(snap, title, fanart)
        val empty get() = pictures.isEmpty() && logo == null && video == null
    }

    private val root = File(context.cacheDir, "media")
    private val prefs = Prefs(context)

    private enum class Kind(val ext: String, val ssTypes: List<String>, val libretro: String?) {
        SNAP("png", listOf("ss"), "Named_Snaps"),
        TITLE("png", listOf("sstitle"), "Named_Titles"),
        LOGO("png", listOf("wheel-hd", "wheel", "wheel-steel"), null),
        FANART("jpg", listOf("fanart"), null),
        VIDEO("mp4", listOf("video-normalized", "video"), null),
    }

    private fun file(game: Game, kind: Kind): File =
        File(root, "${game.system.name}/${Covers.sanitize(game.saveName)}.${kind.name.lowercase()}.${kind.ext}")

    /** What is on disk already (fast, any thread). */
    fun cached(game: Game): Media = Media(
        snap = file(game, Kind.SNAP).takeIf { it.exists() },
        title = file(game, Kind.TITLE).takeIf { it.exists() },
        logo = file(game, Kind.LOGO).takeIf { it.exists() },
        fanart = file(game, Kind.FANART).takeIf { it.exists() },
        video = file(game, Kind.VIDEO).takeIf { it.exists() },
    )

    /** Downloads what is missing (off the main thread) and returns everything. */
    fun fetch(game: Game): Media {
        if (!prefs.downloadCovers || !Prefs.raw(context).getBoolean("game_media", true)) return cached(game)
        val ss = covers.ssGame(game)
        val lr by lazy { covers.libretroName(game) }
        for (kind in Kind.entries) {
            val f = file(game, kind)
            if (f.exists()) continue
            val miss = File(f.path + ".none")
            if (miss.exists() && System.currentTimeMillis() - miss.lastModified() < 5L * 24 * 3600 * 1000) continue
            if (kind == Kind.VIDEO && !videoAllowed()) continue
            f.parentFile?.mkdirs()
            var found = false
            var offline = ss.offline
            ss.jeu?.let { jeu ->
                covers.pickMedia(jeu, kind.ssTypes)?.let { url ->
                    when (download(url, f, if (kind == Kind.VIDEO) 40L shl 20 else 8L shl 20)) {
                        true -> found = true
                        null -> offline = true
                        false -> {}
                    }
                }
            }
            if (!found && kind.libretro != null) {
                lr?.let { (repo, name) ->
                    val url = "https://raw.githubusercontent.com/libretro-thumbnails/$repo/master/${kind.libretro}/${enc(name)}.png"
                    when (download(url, f, 8L shl 20)) {
                        true -> found = true
                        null -> offline = true
                        false -> {}
                    }
                }
            }
            if (!found && !offline) runCatching { miss.writeText("") }
        }
        return cached(game)
    }

    /** Videos are big: by default only on Wi-Fi (or any unmetered network). */
    private fun videoAllowed(): Boolean {
        val mode = Prefs.raw(context).getString("media_video", "wifi") ?: "wifi"
        if (mode == "never") return false
        if (mode == "always") return true
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return false
        val caps = cm.getNetworkCapabilities(cm.activeNetwork) ?: return false
        return caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
    }

    /** true = saved, false = does not exist, null = could not ask now. */
    private fun download(url: String, f: File, maxBytes: Long): Boolean? = try {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 10_000
        conn.readTimeout = 30_000
        conn.setRequestProperty("User-Agent", "Cartucho")
        try {
            when (conn.responseCode) {
                200 -> {
                    val tmp = File(f.path + ".tmp")
                    var total = 0L
                    conn.inputStream.use { input ->
                        tmp.outputStream().use { out ->
                            val buf = ByteArray(64 * 1024)
                            while (true) {
                                val n = input.read(buf)
                                if (n < 0) break
                                total += n
                                if (total > maxBytes) break
                                out.write(buf, 0, n)
                            }
                        }
                    }
                    // ScreenScraper answers some misses with a short text.
                    if (total in 1024..maxBytes && tmp.renameTo(f)) true else { tmp.delete(); false }
                }
                404, 410 -> false
                else -> null
            }
        } finally {
            conn.disconnect()
        }
    } catch (e: Exception) {
        null
    }

    /** Frees the space (settings). */
    fun clear() = root.deleteRecursively()

    fun sizeBytes(): Long = root.walkBottomUp().filter { it.isFile }.sumOf { it.length() }

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8").replace("+", "%20")
}
