package com.cartucho.emu

import android.content.Context
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * Ready-made cheats from the libretro database (github.com/libretro/
 * libretro-database, folder cht/). The list of names ships in the APK
 * (assets/cheats/<folder>.txt, written by CI); the chosen file is downloaded
 * when asked for and kept.
 */
class CheatDb(private val context: Context) {

    /** One .cht file: [folder] in cht/, [name] without the extension. */
    data class Source(val folder: String, val name: String) {
        /** What tells files of the same game apart: "GameShark", "Game Genie"... */
        val label: String
            get() {
                val tags = Regex("\\(([^)]*)\\)").findAll(name).map { it.groupValues[1] }.toList()
                val device = tags.filter { t ->
                    listOf("gameshark", "game genie", "action replay", "code breaker", "codebreaker", "xploder",
                        "pro action", "cheat", "diff", "gs", "ar").any { t.lowercase().contains(it) }
                }
                return device.joinToString(" · ").ifEmpty { "Padrão" } +
                    if (folder.endsWith("Color")) "" else "  (${folder.substringAfter(" - ")})"
            }
    }

    data class Entry(val name: String, val code: String)

    private val cacheDir = File(context.filesDir, "cht").apply { mkdirs() }

    private fun folders(system: GameSystem): List<String> = when (system) {
        GameSystem.GB -> listOf("Nintendo - Game Boy Color", "Nintendo - Game Boy")
        GameSystem.GBA -> listOf("Nintendo - Game Boy Advance")
        GameSystem.NDS -> listOf("Nintendo - Nintendo DS")
        GameSystem.NES -> listOf("Nintendo - Nintendo Entertainment System")
        GameSystem.SNES -> listOf("Nintendo - Super Nintendo Entertainment System")
        GameSystem.MD -> listOf("Sega - Mega Drive - Genesis")
        GameSystem.SMS -> listOf("Sega - Master System - Mark III")
        GameSystem.GG -> listOf("Sega - Game Gear")
        GameSystem.PS1 -> listOf("Sony - PlayStation")
        else -> emptyList()
    }

    fun supported(system: GameSystem) = folders(system).isNotEmpty()

    private fun names(folder: String): List<String> = try {
        context.assets.open("cheats/$folder.txt").bufferedReader().use { r -> r.readLines().filter { it.isNotEmpty() } }
    } catch (e: Exception) {
        emptyList()
    }

    /** Title without tags: "Pokemon - Yellow Version - Special Pikachu Edition". */
    private fun base(name: String) = name.substringBefore(" (").trim()

    /** Every cheat file for this game, the closest match's versions first. */
    fun find(game: Game): List<Source> {
        val out = mutableListOf<Source>()
        for (folder in folders(game.system)) {
            val all = names(folder)
            val best = Covers.bestMatch(game.fileName, all) ?: bestByTitle(game, all) ?: continue
            val title = base(best)
            all.filter { base(it) == title }
                .sortedBy { if (it == best) 0 else 1 }
                .forEach { out += Source(folder, it) }
        }
        return out
    }

    /** Matching on the title alone ("Pokémon – Yellow" vs "Pokemon - Yellow Version - Special..."). */
    private fun bestByTitle(game: Game, all: List<String>): String? {
        val want = Covers.tokens(game.fileName).toSet()
        if (want.isEmpty()) return null
        return all.filter { Covers.tokens(it).toSet().containsAll(want) }
            .minByOrNull { Covers.tokens(it).size * 100 + it.length }
    }

    /** Downloads (or reads the kept copy of) one file. Off the main thread. */
    fun load(source: Source): List<Entry> {
        val file = File(cacheDir, (source.folder + "__" + source.name).replace(Regex("[\\\\/:*?\"<>|]"), "_") + ".cht")
        if (!file.exists() || file.length() == 0L) {
            fun enc(s: String) = URLEncoder.encode(s, "UTF-8").replace("+", "%20")
            val url = URL("https://raw.githubusercontent.com/libretro/libretro-database/master/cht/" +
                enc(source.folder) + "/" + enc(source.name) + ".cht")
            val conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = 15000
            conn.readTimeout = 20000
            try {
                if (conn.responseCode != 200) throw IllegalStateException("HTTP ${conn.responseCode}")
                val tmp = File(file.path + ".tmp")
                conn.inputStream.use { input -> tmp.outputStream().use { input.copyTo(it) } }
                tmp.renameTo(file)
            } finally {
                conn.disconnect()
            }
        }
        return parse(file.readText())
    }

    companion object {
        /** RetroArch .cht: cheatN_desc = "...", cheatN_code = "A+B+C". */
        fun parse(text: String): List<Entry> {
            val desc = HashMap<Int, String>()
            val code = HashMap<Int, String>()
            val re = Regex("^\\s*cheat(\\d+)_(desc|code)\\s*=\\s*\"(.*)\"\\s*$")
            for (line in text.lineSequence()) {
                val m = re.find(line) ?: continue
                val i = m.groupValues[1].toInt()
                val v = m.groupValues[3]
                if (m.groupValues[2] == "desc") desc[i] = unescape(v) else code[i] = v
            }
            return code.keys.sorted().mapNotNull { i ->
                val c = code[i]!!.trim()
                if (c.isEmpty()) null
                else Entry(desc[i]?.ifBlank { null } ?: "Trapaça ${i + 1}", c.split('+').joinToString("\n") { it.trim() })
            }
        }

        private fun unescape(s: String) = s.replace("&amp;", "&").replace("&quot;", "\"")
            .replace("&lt;", "<").replace("&gt;", ">").replace("&#39;", "'").replace("\\\"", "\"")
    }
}
