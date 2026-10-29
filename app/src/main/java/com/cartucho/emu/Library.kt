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
    /** The game's own file name (inside the archive, for zipped games). */
    val fileName: String,
    val system: GameSystem,
    val size: Long,
    /** Path of the game inside a .zip/.7z, or null for a plain file. */
    val entry: String? = null,
    /** File name of the archive holding it ("" for a plain file). */
    val archive: String = "",
    /** A cover image found next to the game (Skraper / EmulationStation media), or "". */
    val coverUri: String = "",
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
        .put("entry", entry ?: "").put("archive", archive).put("cover", coverUri)

    companion object {
        fun fromJson(o: JSONObject): Game? {
            val system = GameSystem.fromName(o.optString("system")) ?: return null
            return Game(
                o.getString("uri"), o.getString("file"), system, o.optLong("size"),
                o.optString("entry").ifEmpty { null }, o.optString("archive"), o.optString("cover"),
            )
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
            val tree = Tree()
            walk(root, "", 0, tree)
            val local = LocalCovers(context, tree.images, tree.gamelists)
            // Discs listed in an .m3u are played through it: list the game once.
            val inPlaylists = HashSet<String>()
            for ((path, game) in tree.games) {
                if (!game.fileName.endsWith(".m3u", true) || game.entry != null) continue
                val dir = path.substringBeforeLast('/', "")
                runCatching {
                    context.contentResolver.openInputStream(Uri.parse(game.uri))?.bufferedReader()?.useLines { lines ->
                        lines.map { it.trim() }.filter { it.isNotEmpty() && !it.startsWith("#") }.forEach { line ->
                            val rel = line.replace('\\', '/')
                            inPlaylists += (if (dir.isEmpty()) rel else "$dir/$rel").lowercase()
                        }
                    }
                }
            }
            for ((path, game) in tree.games) {
                if (path.lowercase() in inPlaylists) continue
                val cover = local.find(path)
                found[game.uri] = if (cover != null) game.copy(coverUri = cover) else game
            }
        }
        looseGames().forEach { found.putIfAbsent(it.uri, it) }
        val games = found.values.sortedBy { it.title.lowercase() }
        saveGames(games)
        return games
    }

    /** What one library folder holds, by path relative to the folder. */
    class Tree {
        val games = mutableListOf<Pair<String, Game>>()
        val images = HashMap<String, Uri>()        // lowercase relative path -> image
        val gamelists = mutableListOf<Pair<String, Uri>>()  // directory -> gamelist.xml
    }

    private fun walk(dir: DocumentFile, path: String, depth: Int, tree: Tree) {
        if (depth > 5) return
        val children = try { dir.listFiles() } catch (e: Exception) { return }
        for (f in children) {
            val name = f.name ?: continue
            val rel = if (path.isEmpty()) name else "$path/$name"
            if (f.isDirectory) {
                walk(f, rel, depth + 1, tree)
                continue
            }
            val lower = name.lowercase()
            if (lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".webp")) {
                tree.images[rel.lowercase()] = f.uri
                continue
            }
            if (lower == "gamelist.xml") {
                tree.gamelists += path to f.uri
                continue
            }
            if (Archives.isArchive(name)) {
                val inner = Archives.peek(context, f.uri, name) ?: continue
                val innerName = inner.name.substringAfterLast('/')
                val sys = GameSystem.forFileName(innerName) ?: continue
                tree.games += rel to Game(f.uri.toString(), innerName, sys, f.length(), inner.name, name)
                continue
            }
            val system = GameSystem.forFileName(name, f.length()) ?: continue
            // .bin/.elf etc. are ambiguous; skip tiny files that cannot be games.
            if (f.length() < 1024) continue
            tree.games += rel to Game(f.uri.toString(), name, system, f.length())
        }
    }

}
