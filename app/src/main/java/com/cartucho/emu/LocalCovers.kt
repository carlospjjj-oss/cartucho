package com.cartucho.emu

import android.content.Context
import android.net.Uri
import android.util.Xml
import org.xmlpull.v1.XmlPullParser

/**
 * Cover images that already sit next to the games: what Skraper,
 * EmulationStation / ES-DE or a player's own folders put there.
 *
 *  - gamelist.xml (EmulationStation): <game><path>./Game.gba</path>
 *    <thumbnail>./media/box2dfront/Game.png</thumbnail> ... — box art fields
 *    are preferred over screenshots.
 *  - Otherwise an image named like the game anywhere in the library folder
 *    (Game.png, Game-image.png, Game-thumb.png...), preferring folders whose
 *    name says box art (box2dfront, covers, boxart, thumbnails, images) over
 *    screenshots, marquees and logos.
 *
 * Paths are relative to the library folder; `images` maps lowercase relative
 * paths to their documents.
 */
class LocalCovers(
    private val context: Context,
    private val images: Map<String, Uri>,
    gamelists: List<Pair<String, Uri>>,
) {
    // Lowercase rom path -> image document, from every gamelist.xml.
    private val fromGamelists = HashMap<String, Uri>()

    // Lowercase file base name (without extension) -> images with that name.
    private val byBase = HashMap<String, MutableList<String>>()

    init {
        for ((dir, uri) in gamelists) parseGamelist(dir, uri)
        for (path in images.keys) {
            val base = path.substringAfterLast('/').substringBeforeLast('.')
            byBase.getOrPut(base) { mutableListOf() } += path
        }
    }

    /** The cover for the game file at `romPath`, as a content URI string. */
    fun find(romPath: String): String? {
        fromGamelists[romPath.lowercase()]?.let { return it.toString() }
        val base = romPath.substringAfterLast('/').substringBeforeLast('.').lowercase()
        val candidates = SUFFIXES.flatMap { suffix -> byBase[base + suffix].orEmpty() }
        return candidates.maxByOrNull { score(it) }?.takeIf { score(it) > -50 }
            ?.let { images[it]?.toString() }
    }

    private fun score(path: String): Int {
        val folders = path.substringBeforeLast('/', "")
        var s = 0
        for ((word, points) in FOLDER_POINTS) if (word in folders) s += points
        if (path.substringAfterLast('/').contains("-thumb") || path.contains("-boxart")) s += 5
        return s
    }

    private fun parseGamelist(dir: String, uri: Uri) {
        try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                val parser = Xml.newPullParser()
                parser.setInput(input, null)
                var inGame = false
                var field: String? = null
                val values = HashMap<String, String>()
                while (true) {
                    when (parser.next()) {
                        XmlPullParser.END_DOCUMENT -> return
                        XmlPullParser.START_TAG -> {
                            if (parser.name == "game") {
                                inGame = true
                                values.clear()
                            } else if (inGame) field = parser.name
                        }
                        XmlPullParser.TEXT -> if (inGame && field != null) values[field!!] = parser.text.trim()
                        XmlPullParser.END_TAG -> {
                            if (parser.name == "game") {
                                inGame = false
                                addEntry(dir, values)
                            }
                            field = null
                        }
                    }
                }
            }
        } catch (e: Exception) {
            // A broken gamelist only costs its covers.
        }
    }

    private fun addEntry(dir: String, values: Map<String, String>) {
        val rom = values["path"]?.takeIf { it.isNotEmpty() } ?: return
        for (key in GAMELIST_FIELDS) {
            val img = values[key]?.takeIf { it.isNotEmpty() } ?: continue
            val uri = images[resolve(dir, img).lowercase()] ?: continue
            fromGamelists[resolve(dir, rom).lowercase()] = uri
            return
        }
    }

    companion object {
        private val SUFFIXES = listOf("", "-thumb", "-boxart", "-box", "-cover", "-image", "-box2d")

        // Box art first, then generic images; screenshots and decorations last.
        private val GAMELIST_FIELDS = listOf("cover", "boxart", "box2dfront", "thumbnail", "image")

        private val FOLDER_POINTS = listOf(
            "box2dfront" to 40, "boxart" to 35, "covers" to 35, "cover" to 30, "box" to 20,
            "thumbnail" to 15, "named_boxarts" to 40, "images" to 10, "media" to 2,
            "screenshot" to -40, "snap" to -40, "titlescreen" to -40, "marquee" to -80,
            "wheel" to -80, "logo" to -80, "fanart" to -60, "video" to -100, "manual" to -100,
        )

        /** "roms/gba" + "./media/x.png" -> "roms/gba/media/x.png" (handles ./ and ../). */
        fun resolve(dir: String, relative: String): String {
            val parts = if (dir.isEmpty()) mutableListOf() else dir.split('/').toMutableList()
            for (seg in relative.replace('\\', '/').split('/')) {
                when (seg) {
                    "", "." -> {}
                    ".." -> if (parts.isNotEmpty()) parts.removeAt(parts.size - 1)
                    else -> parts += seg
                }
            }
            return parts.joinToString("/")
        }
    }
}
