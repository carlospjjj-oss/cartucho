package com.cartucho.emu

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** One game file the library knows about. */
data class Game(
    val uri: String,
    val fileName: String,
    val system: GameSystem,
    val size: Long,
) {
    /** "Pokemon - Fire Red Version (USA, Europe) (Rev 1).gba" -> "Pokemon - Fire Red Version". */
    val title: String
        get() = fileName.substringBeforeLast('.')
            .replace(Regex("\\s*[\\(\\[][^\\)\\]]*[\\)\\]]"), "")
            .replace('_', ' ')
            .trim()
            .ifEmpty { fileName }

    /** The bracketed tags, e.g. "USA, Europe · Rev 1". */
    val tags: String
        get() = Regex("[\\(\\[]([^\\)\\]]*)[\\)\\]]").findAll(fileName.substringBeforeLast('.'))
            .map { it.groupValues[1] }
            .joinToString(" · ")

    /** Name used for this game's saves and states (stable across folders). */
    val saveName: String
        get() = fileName.substringBeforeLast('.').replace(Regex("[\\\\/:*?\"<>|]"), "_")

    fun toJson(): JSONObject = JSONObject()
        .put("uri", uri).put("file", fileName).put("system", system.name).put("size", size)

    companion object {
        fun fromJson(o: JSONObject): Game? {
            val system = GameSystem.fromName(o.optString("system")) ?: return null
            return Game(o.getString("uri"), o.getString("file"), system, o.optLong("size"))
        }
    }
}

/**
 * The library: the folders the user picked, the games found in them, and what
 * was played when. Kept in small JSON files so the list shows instantly on
 * launch while a rescan runs in the background.
 */
class LibraryStore(private val context: Context) {
    private val file = File(context.filesDir, "library.json")
    private val raw = Prefs.raw(context)

    var folders: List<String>
        get() = raw.getString("folders", null)?.let { s ->
            val a = JSONArray(s); (0 until a.length()).map { a.getString(it) }
        } ?: emptyList()
        set(value) = raw.edit().putString("folders", JSONArray(value).toString()).apply()

    fun addFolder(uri: String) {
        if (uri !in folders) folders = folders + uri
    }

    fun removeFolder(uri: String) {
        folders = folders - uri
    }

    fun loadGames(): List<Game> = try {
        val a = JSONArray(file.readText())
        (0 until a.length()).mapNotNull { Game.fromJson(a.getJSONObject(it)) }
    } catch (e: Exception) {
        emptyList()
    }

    fun saveGames(games: List<Game>) {
        val a = JSONArray()
        games.forEach { a.put(it.toJson()) }
        val tmp = File(file.path + ".tmp")
        tmp.writeText(a.toString())
        tmp.renameTo(file)
    }

    fun lastPlayed(game: Game): Long = raw.getLong("played:" + game.uri, 0L)
    fun markPlayed(game: Game) = raw.edit().putLong("played:" + game.uri, System.currentTimeMillis()).apply()

    fun isFavorite(game: Game): Boolean = raw.getBoolean("fav:" + game.uri, false)
    fun setFavorite(game: Game, fav: Boolean) = raw.edit().putBoolean("fav:" + game.uri, fav).apply()

    /** Single files opened with "Open file" (not inside a library folder). */
    fun addLoose(game: Game) {
        val loose = raw.getStringSet("loose", emptySet())!!.toMutableSet()
        loose.add(game.toJson().toString())
        raw.edit().putStringSet("loose", loose).apply()
    }

    fun looseGames(): List<Game> = raw.getStringSet("loose", emptySet())!!
        .mapNotNull { runCatching { Game.fromJson(JSONObject(it)) }.getOrNull() }

    /**
     * Walks every library folder (a few levels deep) for game files. Slow on
     * big folders, so call it off the main thread.
     */
    fun scan(): List<Game> {
        val found = LinkedHashMap<String, Game>()
        for (folder in folders) {
            val root = DocumentFile.fromTreeUri(context, Uri.parse(folder)) ?: continue
            walk(root, 0, found)
        }
        looseGames().forEach { found.putIfAbsent(it.uri, it) }
        val games = found.values.sortedBy { it.title.lowercase() }
        saveGames(games)
        return games
    }

    private fun walk(dir: DocumentFile, depth: Int, out: MutableMap<String, Game>) {
        if (depth > 4) return
        val children = try { dir.listFiles() } catch (e: Exception) { return }
        for (f in children) {
            if (f.isDirectory) {
                walk(f, depth + 1, out)
                continue
            }
            val name = f.name ?: continue
            val system = GameSystem.forFileName(name) ?: continue
            // .bin/.elf etc. are ambiguous; skip tiny files that cannot be games.
            if (f.length() < 1024) continue
            out[f.uri.toString()] = Game(f.uri.toString(), name, system, f.length())
        }
    }
}
