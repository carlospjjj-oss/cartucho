package com.cartucho.emu

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import android.widget.ImageView
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Executors

/**
 * Box art for the library.
 *
 * - A cover the player picked for a game always wins.
 * - Then an image already next to the game (Skraper / EmulationStation media,
 *   gamelist.xml), found when the library is scanned (LocalCovers).
 * - Then the player's own cover repository, if set (Prefs.coversRepo): a
 *   GitHub repository ("user/repo") or any web folder holding
 *   <console>/<game name>.png, e.g. GBA/Eclat Pourpre.png.
 * - Then ScreenScraper, when the player entered an account (by CRC for
 *   GB/GBA, by name and size otherwise).
 * - Then the libretro thumbnail collection.
 * - Last TheGamesDB, with the player's API key (it has a monthly allowance). Its names follow No-Intro
 *   ("Pokemon - FireRed Version (USA, Europe)"); files named differently
 *   ("Pokemon Fire Red (PT-BR).gba") are matched by similarity against the
 *   collection's list of names, which ships inside the APK (built by CI) so
 *   no rate-limited API call is needed.
 * - Downloads are kept on disk. A game with no cover anywhere is retried
 *   after a few days or after "Atualizar capas" (refresh()); a network error
 *   is never recorded as "no cover".
 */
class Covers(private val context: Context) {
    private val dir = File(context.cacheDir, "covers2").apply { mkdirs() }
    private val customDir = File(context.filesDir, "covers_custom").apply { mkdirs() }
    private val memory = object : LruCache<String, Bitmap>(24 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }
    private val pool = Executors.newFixedThreadPool(3)
    private val main = Handler(Looper.getMainLooper())
    private val prefs = Prefs(context)
    private val indexes = HashMap<String, List<String>>()

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
            val bmp = try { fetch(game, key) } catch (e: Exception) { null }
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

    /** Uses an image the player picked as this game's cover. */
    fun setCustom(game: Game, image: Uri): Boolean = try {
        context.contentResolver.openInputStream(image)?.use { input ->
            customFile(game).outputStream().use { input.copyTo(it) }
        }
        memory.remove(key(game))
        decode(customFile(game)) != null
    } catch (e: Exception) {
        false
    }

    fun hasCustom(game: Game) = customFile(game).exists()

    fun removeCustom(game: Game) {
        customFile(game).delete()
        memory.remove(key(game))
    }

    /** Forgets misses and the name lists so every game without art is tried again. */
    fun refresh() {
        dir.listFiles()?.forEach { if (it.name.endsWith(".none") || it.name.startsWith("index_")) it.delete() }
        synchronized(indexes) { indexes.clear() }
        memory.evictAll()
        ssPausedUntil = 0L
        tgdbPausedUntil = 0L
    }

    /** The cover, found or downloaded now (call off the main thread). */
    fun bitmapBlocking(game: Game): Bitmap? = runCatching { fetch(game, key(game)) }.getOrNull()

    private fun key(game: Game) = game.system.name + "/" + sanitize(game.fileName.substringBeforeLast('.'))
    private fun customFile(game: Game) = File(customDir, key(game).replace('/', '_') + ".img")

    private fun fetch(game: Game, key: String): Bitmap? {
        customFile(game).takeIf { it.exists() }?.let { return decode(it) }
        val file = File(dir, key.replace('/', '_') + ".png")
        val miss = File(file.path + ".none")
        // An image found next to the game (Skraper, EmulationStation, own
        // folders) beats anything downloaded.
        if (game.coverUri.isNotEmpty()) {
            val local = File(dir, key.replace('/', '_') + ".local")
            try {
                context.contentResolver.openInputStream(Uri.parse(game.coverUri))?.use { input ->
                    local.outputStream().use { input.copyTo(it) }
                }
                decode(local)?.let { return it }
            } catch (e: Exception) {
            }
        }
        if (file.exists()) return decode(file)
        if (!prefs.downloadCovers) return null
        if (miss.exists() && System.currentTimeMillis() - miss.lastModified() < 3L * 24 * 3600 * 1000) return null

        var offline = false
        // The player's own repository first.
        for (url in customUrls(game)) {
            when (download(url, file)) {
                Fetch.OK -> return decode(file)
                Fetch.NETWORK -> offline = true
                Fetch.MISSING -> {}
            }
        }
        // ScreenScraper, when the player configured an account.
        when (val url = screenScraperUrl(game)) {
            null -> {}
            "" -> offline = true  // not reachable now: try again later
            else -> when (download(url, file)) {
                Fetch.OK -> return decode(file)
                Fetch.NETWORK -> offline = true
                Fetch.MISSING -> {}
            }
        }
        for (repo in repos(game)) {
            val names = index(repo)
            if (names == null) {
                offline = true
                continue
            }
            val name = bestMatch(game.fileName, names) ?: continue
            for (url in libretroUrls(repo, name)) {
                when (download(url, file)) {
                    Fetch.OK -> return decode(file)
                    Fetch.NETWORK -> offline = true
                    Fetch.MISSING -> {}
                }
            }
        }
        // TheGamesDB last: its key has a monthly allowance.
        when (val url = theGamesDbUrl(game)) {
            null -> {}
            "" -> offline = true
            else -> when (download(url, file)) {
                Fetch.OK -> return decode(file)
                Fetch.NETWORK -> offline = true
                Fetch.MISSING -> {}
            }
        }
        if (!offline) miss.writeText("")
        return null
    }

    private enum class Fetch { OK, MISSING, NETWORK }

    // --- TheGamesDB ------------------------------------------------------------

    private val tgdbLock = Any()
    @Volatile private var tgdbPausedUntil = 0L

    /**
     * Front box art from TheGamesDB, searched by name within the console:
     * null when not configured or not found, "" when it could not be asked
     * now (allowance used up, bad key, network).
     */
    private fun theGamesDbUrl(game: Game): String? {
        val key = prefs.tgdbKey
        if (key.isEmpty()) return null
        if (System.currentTimeMillis() < tgdbPausedUntil) return ""
        val platform = when (game.system) {
            GameSystem.GBA -> 5
            GameSystem.GB -> if (game.fileName.endsWith(".gbc", true)) 41 else 4
            GameSystem.NDS -> 8
            GameSystem.N3DS -> 4912
            GameSystem.NES -> 7
            GameSystem.SNES -> 6
            GameSystem.MD -> 18
            GameSystem.SMS -> 35
            GameSystem.GG -> 20
            GameSystem.PS1 -> 10
        }
        // Search with the plain title words; the match below is ours.
        val query = tokens(game.fileName).joinToString(" ").ifEmpty { return null }
        synchronized(tgdbLock) {
            val url = "https://api.thegamesdb.net/v1.1/Games/ByGameName?apikey=${enc(key)}" +
                "&name=${enc(query)}&filter%5Bplatform%5D=$platform&include=boxart"
            val conn = try { URL(url).openConnection() as HttpURLConnection } catch (e: Exception) { return "" }
            conn.connectTimeout = 10000
            conn.readTimeout = 30000
            conn.setRequestProperty("User-Agent", "Cartucho")
            return try {
                val code = conn.responseCode
                val body = (if (code in 200..299) conn.inputStream else conn.errorStream)
                    ?.bufferedReader()?.use { it.readText() } ?: ""
                if (code != 200) {
                    tgdbPausedUntil = System.currentTimeMillis() + 30 * 60 * 1000
                    tgdbStatus(if (code == 401 || code == 403) "Chave recusada ou limite do mês atingido" else "Erro $code")
                    return ""
                }
                val json = JSONObject(body)
                json.optInt("remaining_monthly_allowance", -1).takeIf { it >= 0 }?.let {
                    tgdbStatus("Funcionando · $it consultas restantes no mês")
                }
                val games = json.optJSONObject("data")?.optJSONArray("games") ?: return null
                val titles = (0 until games.length()).mapNotNull { games.optJSONObject(it) }
                    .associate { it.optString("game_title") to it.optInt("id") }
                val title = bestMatch(game.fileName, titles.keys.toList()) ?: return null
                val id = titles[title] ?: return null
                val boxart = json.optJSONObject("include")?.optJSONObject("boxart") ?: return null
                val base = boxart.optJSONObject("base_url")?.optString("original")
                    ?: "https://cdn.thegamesdb.net/images/original/"
                val images = boxart.optJSONObject("data")?.optJSONArray(id.toString()) ?: return null
                for (i in 0 until images.length()) {
                    val img = images.optJSONObject(i) ?: continue
                    if (img.optString("side") == "front") return base + img.optString("filename")
                }
                null
            } catch (e: Exception) {
                ""
            } finally {
                conn.disconnect()
            }
        }
    }

    private fun tgdbStatus(text: String) = Prefs.raw(context).edit().putString("tgdb_status", text).apply()

    // --- ScreenScraper -------------------------------------------------------

    private val ssLock = Any()
    @Volatile private var ssPausedUntil = 0L

    /**
     * Box art URL from ScreenScraper: null when not configured or the game
     * is unknown there, "" when it could not be asked right now (limits,
     * login, network). One request at a time, as their API asks.
     */
    private fun screenScraperUrl(game: Game): String? {
        val user = prefs.ssUser
        val devId = prefs.ssDevId
        if (user.isEmpty() && devId.isEmpty()) return null
        if (System.currentTimeMillis() < ssPausedUntil) return ""
        val system = when (game.system) {
            GameSystem.GBA -> 12
            GameSystem.GB -> if (game.fileName.endsWith(".gbc", true)) 10 else 9
            GameSystem.NDS -> 15
            GameSystem.N3DS -> 17
            GameSystem.NES -> 3
            GameSystem.SNES -> 4
            GameSystem.MD -> 1
            GameSystem.SMS -> 2
            GameSystem.GG -> 21
            GameSystem.PS1 -> 57
        }
        synchronized(ssLock) {
            val params = linkedMapOf(
                "devid" to devId, "devpassword" to prefs.ssDevPassword,
                "softname" to "Cartucho", "output" to "json",
                "ssid" to user, "sspassword" to prefs.ssPassword,
                "systemeid" to system.toString(), "romtype" to "rom",
                "romnom" to (if (game.archive.isNotEmpty()) game.archive else game.fileName),
                "romtaille" to game.size.toString(),
            )
            crcOf(game)?.let { params["crc"] = String.format("%08X", it) }
            val url = "https://api.screenscraper.fr/api2/jeuInfos.php?" +
                params.entries.joinToString("&") { it.key + "=" + enc(it.value) }
            val conn = try { URL(url).openConnection() as HttpURLConnection } catch (e: Exception) { return "" }
            conn.connectTimeout = 10000
            conn.readTimeout = 30000
            conn.setRequestProperty("User-Agent", "Cartucho")
            return try {
                val code = conn.responseCode
                val body = (if (code in 200..299) conn.inputStream else conn.errorStream)
                    ?.bufferedReader()?.use { it.readText() } ?: ""
                when {
                    code == 404 || body.contains("non trouv") -> {
                        ssStatus("Funcionando")
                        null
                    }
                    code != 200 || body.contains("Erreur de login") || !body.trimStart().startsWith("{") -> {
                        // Login refused, quota, too many requests, closed API...:
                        // stop asking for a while and say why in the settings.
                        ssPausedUntil = System.currentTimeMillis() + 10 * 60 * 1000
                        ssStatus(when {
                            body.contains("Erreur de login") || code == 403 -> "Login recusado: confira usuário, senha e ID de desenvolvedor"
                            code == 429 -> "Muitas consultas por minuto; tentando de novo em 10 min"
                            code == 430 -> "Limite diário da sua conta atingido"
                            code == 423 -> "ScreenScraper fora do ar no momento"
                            else -> "Erro $code"
                        })
                        ""
                    }
                    else -> {
                        ssStatus("Funcionando")
                        pickBox(JSONObject(body))
                    }
                }
            } catch (e: Exception) {
                ""
            } finally {
                conn.disconnect()
            }
        }
    }

    /**
     * A description of the game in Portuguese (or English): ScreenScraper
     * first (has Portuguese texts), then TheGamesDB (English). Null when
     * neither is set up or knows the game. Off the main thread.
     */
    fun describe(game: Game): String? {
        if ((prefs.ssUser.isNotEmpty() || prefs.ssDevId.isNotEmpty()) && System.currentTimeMillis() >= ssPausedUntil) {
            synchronized(ssLock) {
                runCatching {
                    val params = linkedMapOf(
                        "devid" to prefs.ssDevId, "devpassword" to prefs.ssDevPassword,
                        "softname" to "Cartucho", "output" to "json",
                        "ssid" to prefs.ssUser, "sspassword" to prefs.ssPassword,
                        "systemeid" to ssSystem(game).toString(), "romtype" to "rom",
                        "romnom" to (if (game.archive.isNotEmpty()) game.archive else game.fileName),
                        "romtaille" to game.size.toString(),
                    )
                    crcOf(game)?.let { params["crc"] = String.format("%08X", it) }
                    val conn = URL("https://api.screenscraper.fr/api2/jeuInfos.php?" +
                        params.entries.joinToString("&") { it.key + "=" + enc(it.value) }).openConnection() as HttpURLConnection
                    conn.connectTimeout = 10000
                    conn.readTimeout = 30000
                    conn.setRequestProperty("User-Agent", "Cartucho")
                    val body = try { conn.inputStream.bufferedReader().use { it.readText() } } finally { conn.disconnect() }
                    val synopsis = JSONObject(body).optJSONObject("response")?.optJSONObject("jeu")?.optJSONArray("synopsis")
                    if (synopsis != null) {
                        val texts = (0 until synopsis.length()).mapNotNull { synopsis.optJSONObject(it) }
                            .associate { it.optString("langue") to it.optString("text") }
                        (texts["pt"] ?: texts["br"] ?: texts["en"] ?: texts.values.firstOrNull())
                            ?.takeIf { it.isNotBlank() }?.let { return it.trim() }
                    }
                }
            }
        }
        val key = prefs.tgdbKey
        if (key.isNotEmpty() && System.currentTimeMillis() >= tgdbPausedUntil) {
            synchronized(tgdbLock) {
                runCatching {
                    val query = tokens(game.fileName).joinToString(" ")
                    val conn = URL("https://api.thegamesdb.net/v1.1/Games/ByGameName?apikey=${enc(key)}" +
                        "&name=${enc(query)}&filter%5Bplatform%5D=${tgdbPlatform(game)}&fields=overview")
                        .openConnection() as HttpURLConnection
                    conn.connectTimeout = 10000
                    conn.readTimeout = 30000
                    conn.setRequestProperty("User-Agent", "Cartucho")
                    val body = try { conn.inputStream.bufferedReader().use { it.readText() } } finally { conn.disconnect() }
                    val games = JSONObject(body).optJSONObject("data")?.optJSONArray("games") ?: return null
                    val byTitle = (0 until games.length()).mapNotNull { games.optJSONObject(it) }
                        .associateBy { it.optString("game_title") }
                    val title = bestMatch(game.fileName, byTitle.keys.toList()) ?: return null
                    return byTitle[title]?.optString("overview")?.takeIf { it.isNotBlank() }?.trim()
                }
            }
        }
        return null
    }

    private fun ssSystem(game: Game) = when (game.system) {
        GameSystem.GBA -> 12
        GameSystem.GB -> if (game.fileName.endsWith(".gbc", true)) 10 else 9
        GameSystem.NDS -> 15
        GameSystem.N3DS -> 17
        GameSystem.NES -> 3
        GameSystem.SNES -> 4
        GameSystem.MD -> 1
        GameSystem.SMS -> 2
        GameSystem.GG -> 21
        GameSystem.PS1 -> 57
    }

    private fun tgdbPlatform(game: Game) = when (game.system) {
        GameSystem.GBA -> 5
        GameSystem.GB -> if (game.fileName.endsWith(".gbc", true)) 41 else 4
        GameSystem.NDS -> 8
        GameSystem.N3DS -> 4912
        GameSystem.NES -> 7
        GameSystem.SNES -> 6
        GameSystem.MD -> 18
        GameSystem.SMS -> 35
        GameSystem.GG -> 20
        GameSystem.PS1 -> 10
    }

    /** The front box art, in the most fitting region available. */
    private fun pickBox(json: JSONObject): String? {
        val medias = json.optJSONObject("response")?.optJSONObject("jeu")?.optJSONArray("medias") ?: return null
        val regions = listOf("br", "us", "wor", "eu", "ss", "jp")
        var best: String? = null
        var bestRank = Int.MAX_VALUE
        for (i in 0 until medias.length()) {
            val m = medias.optJSONObject(i) ?: continue
            if (m.optString("type") != "box-2D") continue
            val rank = regions.indexOf(m.optString("region")).let { if (it < 0) regions.size else it }
            if (rank < bestRank) {
                bestRank = rank
                best = m.optString("url").ifEmpty { null }
            }
        }
        return best
    }

    /** CRC32 of small plain ROMs (GB/GBA), the most reliable way to identify them. */
    private fun crcOf(game: Game): Long? {
        if (game.entry != null || game.size !in 1..64L * 1024 * 1024) return null
        if (game.system.needsPath || game.system == GameSystem.NDS) return null
        return try {
            context.contentResolver.openInputStream(Uri.parse(game.uri))?.use { input ->
                val crc = java.util.zip.CRC32()
                val buf = ByteArray(1 shl 16)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    crc.update(buf, 0, n)
                }
                crc.value
            }
        } catch (e: Exception) {
            null
        }
    }

    private fun ssStatus(text: String) = Prefs.raw(context).edit().putString("ss_status", text).apply()

    /** Candidate URLs in the player's cover repository, best first. */
    private fun customUrls(game: Game): List<String> {
        val base = customBase(prefs.coversRepo) ?: return emptyList()
        val names = listOf(game.fileName.substringBeforeLast('.'), game.title).distinct()
        val folders = listOf(game.system.shortName + "/", "")
        return folders.flatMap { folder ->
            names.flatMap { n -> listOf("$base$folder${enc(n)}.png", "$base$folder${enc(n)}.jpg") }
        }
    }

    /** Names (without .png) in the collection's Named_Boxarts, cached for a month. */
    private fun index(repo: String): List<String>? {
        synchronized(indexes) {
            indexes[repo]?.let { return it }
            // Shipped in the APK (CI writes assets/covers/<repo>.txt).
            try {
                val names = context.assets.open("covers/$repo.txt").bufferedReader().use { r ->
                    r.readLines().filter { it.isNotEmpty() }
                }
                if (names.isNotEmpty()) {
                    indexes[repo] = names
                    return names
                }
            } catch (e: Exception) {
            }
            val cache = File(dir, "index_$repo.txt")
            if (cache.exists() && System.currentTimeMillis() - cache.lastModified() < 30L * 24 * 3600 * 1000) {
                val names = cache.readLines().filter { it.isNotEmpty() }
                indexes[repo] = names
                return names
            }
            return try {
                // Root tree -> the Named_Boxarts subtree (one level, a few
                // thousand entries) through GitHub's public API.
                val root = JSONObject(get("https://api.github.com/repos/libretro-thumbnails/$repo/git/trees/master"))
                val tree = root.getJSONArray("tree")
                var sha: String? = null
                for (i in 0 until tree.length()) {
                    val e = tree.getJSONObject(i)
                    if (e.getString("path") == "Named_Boxarts") sha = e.getString("sha")
                }
                val names = mutableListOf<String>()
                if (sha != null) {
                    val sub = JSONObject(get("https://api.github.com/repos/libretro-thumbnails/$repo/git/trees/$sha"))
                        .getJSONArray("tree")
                    for (i in 0 until sub.length()) {
                        val p = sub.getJSONObject(i).getString("path")
                        if (p.endsWith(".png")) names += p.removeSuffix(".png")
                    }
                }
                cache.writeText(names.joinToString("\n"))
                indexes[repo] = names
                names
            } catch (e: Exception) {
                null
            }
        }
    }

    private fun libretroUrls(repo: String, name: String) = listOf(
        "https://raw.githubusercontent.com/libretro-thumbnails/$repo/master/Named_Boxarts/${enc(name)}.png",
        "https://thumbnails.libretro.com/${enc(repo.replace('_', ' '))}/Named_Boxarts/${enc(name)}.png",
    )

    /** One attempt, retried once on a network error (flaky mobile data). */
    private fun download(url: String, file: File): Fetch {
        var result = Fetch.NETWORK
        for (attempt in 0..1) {
            result = try {
                val conn = URL(url).openConnection() as HttpURLConnection
                conn.connectTimeout = 8000
                conn.readTimeout = 20000
                conn.instanceFollowRedirects = true
                conn.setRequestProperty("User-Agent", "Cartucho")
                try {
                    when (conn.responseCode) {
                        200 -> {
                            val tmp = File(file.path + ".tmp")
                            conn.inputStream.use { input -> tmp.outputStream().use { input.copyTo(it) } }
                            if (tmp.length() > 0 && decodeOk(tmp) && tmp.renameTo(file)) Fetch.OK
                            else Fetch.MISSING
                        }
                        404, 410 -> Fetch.MISSING
                        else -> Fetch.NETWORK  // 403 rate limit, 5xx...
                    }
                } finally {
                    conn.disconnect()
                }
            } catch (e: Exception) {
                Fetch.NETWORK
            }
            if (result != Fetch.NETWORK) return result
        }
        return result
    }

    private fun decodeOk(f: File): Boolean {
        val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(f.path, o)
        return o.outWidth > 0
    }

    private fun get(url: String): String {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 8000
        conn.readTimeout = 20000
        conn.setRequestProperty("Accept", "application/vnd.github+json")
        conn.setRequestProperty("User-Agent", "Cartucho")
        try {
            if (conn.responseCode != 200) throw IllegalStateException("HTTP ${conn.responseCode}")
            return conn.inputStream.bufferedReader().use { it.readText() }
        } finally {
            conn.disconnect()
        }
    }

    private fun repos(game: Game): List<String> = when (game.system) {
        GameSystem.GBA -> listOf("Nintendo_-_Game_Boy_Advance")
        GameSystem.GB -> if (game.fileName.endsWith(".gbc", true))
            listOf("Nintendo_-_Game_Boy_Color", "Nintendo_-_Game_Boy")
        else listOf("Nintendo_-_Game_Boy", "Nintendo_-_Game_Boy_Color")
        GameSystem.NDS -> listOf("Nintendo_-_Nintendo_DS")
        GameSystem.N3DS -> listOf("Nintendo_-_Nintendo_3DS")
        GameSystem.NES -> listOf("Nintendo_-_Nintendo_Entertainment_System")
        GameSystem.SNES -> listOf("Nintendo_-_Super_Nintendo_Entertainment_System")
        GameSystem.MD -> listOf("Sega_-_Mega_Drive_-_Genesis")
        GameSystem.SMS -> listOf("Sega_-_Master_System_-_Mark_III")
        GameSystem.GG -> listOf("Sega_-_Game_Gear")
        GameSystem.PS1 -> listOf("Sony_-_PlayStation")
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
        /**
         * Base URL (ending in '/') of the player's cover repository:
         * "user/repo" or "github.com/user/repo[/tree/branch[/folder]]" for
         * GitHub, or any http(s) URL of a folder.
         */
        fun customBase(setting: String): String? {
            var s = setting.trim().removeSuffix("/")
            if (s.isEmpty()) return null
            if (s.startsWith("http://") || s.startsWith("https://")) {
                val gh = Regex("^https?://(www\\.)?github\\.com/([^/]+)/([^/]+)(/tree/([^/]+)(/(.*))?)?$").find(s)
                    ?: return "$s/"
                s = "github.com/" + gh.groupValues[2] + "/" + gh.groupValues[3] + gh.groupValues[4]
            }
            s = s.removePrefix("github.com/")
            val parts = s.split('/')
            if (parts.size < 2 || parts[0].isEmpty() || parts[1].isEmpty()) return null
            // user/repo[/tree/branch[/sub/folder]]
            var branch = "HEAD"
            var sub = ""
            if (parts.size >= 4 && parts[2] == "tree") {
                branch = parts[3]
                sub = parts.drop(4).joinToString("/").let { if (it.isEmpty()) "" else "$it/" }
            }
            return "https://raw.githubusercontent.com/${parts[0]}/${parts[1].removeSuffix(".git")}/$branch/$sub"
        }

        /** The thumbnail collection replaces these characters with '_'. */
        fun sanitize(name: String) = name.replace(Regex("[&*/:`<>?\\\\|\"]"), "_")

        private val STOP = setOf("the", "version", "a", "of", "and", "ver")

        /** Words of a title without tags, punctuation and filler words. */
        fun tokens(name: String): List<String> {
            var s = name.replace(Regex("\\.[A-Za-z0-9]{2,4}$"), "")
            // "Pokémon" == "Pokemon": drop accents before comparing.
            s = java.text.Normalizer.normalize(s, java.text.Normalizer.Form.NFD)
                .replace(Regex("\\p{Mn}+"), "")
            s = s.replace(Regex("[\\(\\[][^\\)\\]]*[\\)\\]]"), " ").lowercase()
                .replace("&", " and ").replace("'", "")
            return s.split(Regex("[^a-z0-9]+")).filter { it.isNotEmpty() && it !in STOP }
        }

        private fun rank(name: String): Triple<Int, Int, Int> {
            val n = name.lowercase()
            val bad = listOf("(beta", "(proto", "(kiosk", "(demo", "(sample", "(alternate", "hack]", "(hack")
                .any { it in n }
            val region = when {
                "(usa" in n -> 0
                "europe" in n -> 1
                "brazil" in n -> 2
                else -> 3
            }
            return Triple(if (bad) 1 else 0, region, name.length)
        }

        /**
         * The collection name for a game file: the same name when present,
         * else the most similar one (same words in any spacing, or every word
         * of the file found in the name), preferring regular USA/Europe
         * releases. Null when nothing is close enough.
         */
        fun bestMatch(fileName: String, names: List<String>): String? {
            val exact = sanitize(fileName.substringBeforeLast('.'))
            if (exact in names) return exact
            val gt = tokens(fileName)
            if (gt.isEmpty()) return null
            val gc = gt.joinToString("")
            val gset = gt.toSet()
            var best: String? = null
            var bestScore = 0
            for (n in names) {
                val ct = tokens(n)
                if (ct.isEmpty()) continue
                val cc = ct.joinToString("")
                val cset = ct.toSet()
                val score = when {
                    cc == gc -> 100
                    gt.size >= 2 && cset.containsAll(gset) -> 90 - (cset.size - gset.size) * 2
                    gt.size >= 2 && gc in cc && cc.startsWith(gt[0]) -> 85 - (cc.length - gc.length) / 4
                    else -> (100 * (gset intersect cset).size) / (gset union cset).size
                }
                if (score < 80) continue
                val better = best == null || score > bestScore ||
                    (score == bestScore && compareValuesBy(rank(n), rank(best!!), { it.first }, { it.second }, { it.third }) < 0)
                if (better) {
                    best = n
                    bestScore = score
                }
            }
            return best
        }
    }
}
