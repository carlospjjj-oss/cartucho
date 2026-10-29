package com.cartucho.emu

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * Facts about a game: year, genre, developer, publisher and players from the
 * libretro database (assets/meta/<console>.tsv, packed by CI), and a
 * description from ScreenScraper or TheGamesDB when the player set them up.
 */
class GameInfo(private val context: Context) {
    data class Facts(
        val name: String,
        val year: String,
        val genre: String,
        val developer: String,
        val publisher: String,
        val players: String,
    )

    private val tables = HashMap<String, Map<String, Facts>>()

    private fun files(system: GameSystem, fileName: String): List<String> = when (system) {
        GameSystem.GB -> if (fileName.endsWith(".gbc", true)) listOf("Nintendo - Game Boy Color", "Nintendo - Game Boy")
                         else listOf("Nintendo - Game Boy", "Nintendo - Game Boy Color")
        GameSystem.GBA -> listOf("Nintendo - Game Boy Advance")
        GameSystem.NDS -> listOf("Nintendo - Nintendo DS")
        GameSystem.N3DS -> listOf("Nintendo - Nintendo 3DS")
        GameSystem.NES -> listOf("Nintendo - Nintendo Entertainment System")
        GameSystem.SNES -> listOf("Nintendo - Super Nintendo Entertainment System")
        GameSystem.MD -> listOf("Sega - Mega Drive - Genesis")
        GameSystem.SMS -> listOf("Sega - Master System - Mark III")
        GameSystem.GG -> listOf("Sega - Game Gear")
        GameSystem.PS1 -> listOf("Sony - PlayStation")
    }

    private fun table(name: String): Map<String, Facts> = synchronized(tables) {
        tables.getOrPut(name) {
            try {
                context.assets.open("meta/$name.tsv").bufferedReader().useLines { lines ->
                    lines.mapNotNull { line ->
                        val p = line.split('\t')
                        if (p.size < 6) null
                        else Facts(p[0], p[1], p[2], p[3], p[4], p[5])
                    }.associateBy { it.name }
                }
            } catch (e: Exception) {
                emptyMap()
            }
        }
    }

    private val indexes = HashMap<String, Map<String, Facts>>()

    /** Entries by title words, for quick lookups of the whole library. */
    private fun index(name: String): Map<String, Facts> = synchronized(indexes) {
        indexes.getOrPut(name) {
            val out = HashMap<String, Facts>()
            // USA / Europe releases first, so they win for a shared title.
            val ordered = table(name).values.sortedBy { f ->
                when {
                    "(USA" in f.name -> 0
                    "Europe" in f.name -> 1
                    else -> 2
                }
            }
            for (f in ordered) out.putIfAbsent(Covers.tokens(f.name).joinToString(""), f)
            out
        }
    }

    /** Fast lookup by the title's words (for the library list). */
    fun quickFacts(game: Game): Facts? {
        val key = Covers.tokens(game.fileName).joinToString("")
        if (key.isEmpty()) return null
        for (file in files(game.system, game.fileName)) index(file)[key]?.let { return it }
        return null
    }

    /** The database entry closest to this game's file name, or null. Off the main thread. */
    fun facts(game: Game): Facts? {
        quickFacts(game)?.let { return it }
        for (file in files(game.system, game.fileName)) {
            val t = table(file)
            if (t.isEmpty()) continue
            val name = Covers.bestMatch(game.fileName, t.keys.toList()) ?: continue
            return t[name]
        }
        return null
    }

    private fun descriptionFile(game: Game) =
        File(context.filesDir, "info/${game.system.name}_${game.saveName}.json")

    /** The description kept from an earlier lookup (null: never looked; "" none found). */
    fun keptDescription(game: Game): String? =
        runCatching { JSONObject(descriptionFile(game).readText()).optString("text") }.getOrNull()

    /** Looks the description up (and keeps it). Off the main thread. */
    fun fetchDescription(game: Game, covers: Covers): String? {
        val text = covers.describe(game)
        if (text != null) {
            descriptionFile(game).apply { parentFile?.mkdirs() }.writeText(JSONObject().put("text", text).toString())
        }
        return text
    }
}
