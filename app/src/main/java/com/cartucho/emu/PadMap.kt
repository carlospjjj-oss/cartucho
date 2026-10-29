package com.cartucho.emu

import android.content.Context
import android.view.KeyEvent
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.json.JSONObject

/**
 * The player's own button mapping for physical controllers: Android key code
 * -> a console button (Retro.*) or one of the app's actions (negative).
 * Whatever is not remapped keeps the default mapping in [GamepadInput].
 */
object PadMap {
    const val FAST_FORWARD = -1
    const val REWIND = -2
    const val QUICK_SAVE = -3
    const val QUICK_LOAD = -4
    const val MENU = -5

    /** What can be assigned, in the order the screen lists them. */
    val targets: List<Pair<Int, String>> = listOf(
        Retro.A to "A", Retro.B to "B", Retro.X to "X", Retro.Y to "Y",
        Retro.L to "L", Retro.R to "R", Retro.L2 to "ZL / L2", Retro.R2 to "ZR / R2",
        Retro.START to "START", Retro.SELECT to "SELECT",
        Retro.UP to "Cima", Retro.DOWN to "Baixo", Retro.LEFT to "Esquerda", Retro.RIGHT to "Direita",
        Retro.L3 to "L3 (MIC no DS)", Retro.R3 to "R3",
        FAST_FORWARD to "⏩ Acelerar", REWIND to "⏪ Voltar no tempo (segurar)",
        QUICK_SAVE to "Salvar rápido", QUICK_LOAD to "Carregar rápido", MENU to "Abrir o menu",
    )

    private const val KEY = "pad_map"

    fun load(context: Context): Map<Int, Int> = runCatching {
        val o = JSONObject(Prefs.raw(context).getString(KEY, "{}")!!)
        o.keys().asSequence().associate { it.toInt() to o.getInt(it) }
    }.getOrDefault(emptyMap())

    private fun save(context: Context, map: Map<Int, Int>) {
        val o = JSONObject()
        map.forEach { (k, v) -> o.put(k.toString(), v) }
        Prefs.raw(context).edit().putString(KEY, o.toString()).apply()
    }

    private fun keyName(code: Int) = KeyEvent.keyCodeToString(code).removePrefix("KEYCODE_")
        .removePrefix("BUTTON_").replace('_', ' ')

    /** The mapping screen: every target with its key; tap one, press a button. */
    fun showDialog(context: Context) {
        val map = load(context)
        val labels = targets.map { (target, name) ->
            val keys = map.filterValues { it == target }.keys
            "$name   →   " + if (keys.isEmpty()) "padrão" else keys.joinToString(", ") { keyName(it) }
        }.toTypedArray()
        MaterialAlertDialogBuilder(context)
            .setTitle("Controle físico")
            .setItems(labels) { _, which -> capture(context, targets[which]) }
            .setPositiveButton("Fechar", null)
            .setNeutralButton("Restaurar padrão") { _, _ ->
                save(context, emptyMap())
                showDialog(context)
            }
            .show()
    }

    private fun capture(context: Context, target: Pair<Int, String>) {
        var dialog: AlertDialog? = null
        dialog = MaterialAlertDialogBuilder(context)
            .setTitle(target.second)
            .setMessage("Aperte o botão do controle que vai fazer \"${target.second}\".")
            .setNegativeButton(android.R.string.cancel) { _, _ -> showDialog(context) }
            .setOnKeyListener { _, code, event ->
                if (code == KeyEvent.KEYCODE_BACK && !GamepadInput.isGamepad(event.source)) return@setOnKeyListener false
                if (event.action != KeyEvent.ACTION_UP) return@setOnKeyListener true
                val map = load(context).toMutableMap()
                map[code] = target.first
                save(context, map)
                dialog?.dismiss()
                showDialog(context)
                true
            }
            .show()
    }
}
