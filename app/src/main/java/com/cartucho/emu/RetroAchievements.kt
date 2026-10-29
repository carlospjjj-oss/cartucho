package com.cartucho.emu

import android.content.Context
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * RetroAchievements (retroachievements.org): the account and the HTTP side.
 * The password is used once to get a token and is never stored; the native
 * rcheevos client (achievements.cpp) does everything else and hands its
 * requests to [pump].
 */
object RetroAchievements {
    private const val SERVER = "https://retroachievements.org/dorequest.php"

    val userAgent: String
        get() = "Cartucho/${BuildConfig.VERSION_NAME} (Linux; Android ${android.os.Build.VERSION.RELEASE}) rcheevos"

    fun user(context: Context): String = Prefs.raw(context).getString("ra_user", "")!!
    fun token(context: Context): String = Prefs.raw(context).getString("ra_token", "")!!
    fun signedIn(context: Context) = user(context).isNotEmpty() && token(context).isNotEmpty()
    fun enabled(context: Context) = signedIn(context) && Prefs.raw(context).getBoolean("ra_enabled", true)

    fun signOut(context: Context) {
        Prefs.raw(context).edit().remove("ra_user").remove("ra_token").remove("ra_score").apply()
    }

    /** Signs in; returns null on success or the message to show. Off the main thread. */
    fun signIn(context: Context, user: String, password: String): String? {
        val body = "r=login2&u=" + enc(user) + "&p=" + enc(password)
        val (code, text) = try {
            post(SERVER, body, "application/x-www-form-urlencoded")
        } catch (e: Exception) {
            return "Sem conexão com o RetroAchievements (${e.message ?: e.javaClass.simpleName})."
        }
        val json = runCatching { JSONObject(String(text)) }.getOrNull()
            ?: return "Resposta inesperada do servidor (HTTP $code)."
        if (!json.optBoolean("Success")) return json.optString("Error").ifEmpty { "Usuário ou senha incorretos." }
        Prefs.raw(context).edit()
            .putString("ra_user", json.optString("User", user))
            .putString("ra_token", json.getString("Token"))
            .putInt("ra_score", json.optInt("SoftcoreScore", json.optInt("Score")))
            .apply()
        return null
    }

    fun score(context: Context) = Prefs.raw(context).getInt("ra_score", 0)

    /** The RC_CONSOLE_* id rcheevos uses for this game, or 0 if not supported. */
    fun consoleId(game: Game): Int = when (game.system) {
        GameSystem.GB -> if (game.fileName.endsWith(".gbc", true) || game.fileName.endsWith(".cgb", true)) 6 else 4
        GameSystem.GBA -> 5
        GameSystem.NDS -> 18
        GameSystem.NES -> 7
        GameSystem.SNES -> 3
        GameSystem.MD -> 1
        GameSystem.SMS -> 11
        GameSystem.GG -> 15
        GameSystem.PS1 -> 12
        GameSystem.N3DS -> 0
    }

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8")

    private fun post(url: String, body: String?, contentType: String?): Pair<Int, ByteArray> {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 15000
        conn.readTimeout = 30000
        conn.setRequestProperty("User-Agent", userAgent)
        try {
            if (!body.isNullOrEmpty()) {
                conn.requestMethod = "POST"
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", contentType?.ifEmpty { null } ?: "application/x-www-form-urlencoded")
                conn.outputStream.use { it.write(body.toByteArray()) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val bytes = stream?.use { it.readBytes() } ?: ByteArray(0)
            return code to bytes
        } finally {
            conn.disconnect()
        }
    }

    /**
     * The game's HTTP thread: carries the native client's requests to the
     * server and its answers back, until [running] says stop.
     */
    fun pump(running: () -> Boolean) {
        Thread({
            while (running()) {
                val r = NativeBridge.raTakeRequest()
                if (r == null) {
                    Thread.sleep(50)
                    continue
                }
                val id = r[0].toInt()
                val (code, body) = try {
                    post(r[1], r[2], r[3])
                } catch (e: Exception) {
                    // No connection: rcheevos tries again later (RETRYABLE_CLIENT_ERROR).
                    -2 to ByteArray(0)
                }
                NativeBridge.raRespond(id, code, body)
            }
        }, "achievements").apply { isDaemon = true }.start()
    }
}
