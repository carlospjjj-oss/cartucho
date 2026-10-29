package com.cartucho.emu

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * Settings one game uses instead of the app's: filter, colours, run-ahead,
 * fast-forward speed, button look, its own button layout. Kept as one small
 * JSON file per game (read by the game's own process; no SharedPreferences
 * caching across processes).
 */
object PerGame {
    /** What can differ per game, with the app setting it falls back to. */
    data class Item(val key: String, val title: String, val names: Int, val values: Int)

    val items = listOf(
        Item("filter", "Filtro de imagem", R.array.filter_names, R.array.filter_values),
        Item("lcd_color", "Cores de tela real", R.array.lcd_color_names, R.array.lcd_color_values),
        Item("gb_palette", "Paleta do Game Boy clássico", R.array.gb_palette_names, R.array.gb_palette_values),
        Item("frame_blend", "Mistura de quadros", R.array.blend_names, R.array.blend_values),
        Item("run_ahead", "Run-ahead", R.array.run_ahead_names, R.array.run_ahead_values),
        Item("ff_speed", "Velocidade do acelerar", R.array.ff_names, R.array.ff_values),
        Item("button_style", "Visual dos botões", R.array.button_style_names, R.array.button_style_values),
    )

    private fun file(context: Context, game: Game) =
        File(context.filesDir, "pergame/${game.system.name}/${game.saveName}.json")

    fun load(context: Context, game: Game): JSONObject =
        runCatching { JSONObject(file(context, game).readText()) }.getOrElse { JSONObject() }

    /** Sets (or with null, removes) one setting of this game. */
    fun set(context: Context, game: Game, key: String, value: Any?) {
        val o = load(context, game)
        if (value == null) o.remove(key) else o.put(key, value)
        val f = file(context, game)
        f.parentFile?.mkdirs()
        if (o.length() == 0) f.delete() else f.writeText(o.toString())
    }
}
