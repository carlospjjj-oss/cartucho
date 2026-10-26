package com.cartucho.emu

import android.content.Context
import android.text.InputType
import android.widget.EditText
import android.widget.LinearLayout
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Cheat codes per game, in the console's usual formats (GameShark, Action
 * Replay, CodeBreaker...); the core does the parsing. Kept in
 * files/cheats/<console>/<game>.json.
 */
class Cheats(context: Context, game: Game) {
    data class Cheat(val name: String, val code: String, val enabled: Boolean)

    private val file = File(context.filesDir, "cheats/${game.system.name}/${game.saveName}.json")
        .apply { parentFile?.mkdirs() }

    var list: List<Cheat> = load()
        private set

    private fun load(): List<Cheat> = try {
        val a = JSONArray(file.readText())
        (0 until a.length()).map {
            val o = a.getJSONObject(it)
            Cheat(o.getString("name"), o.getString("code"), o.optBoolean("enabled", true))
        }
    } catch (e: Exception) {
        emptyList()
    }

    private fun save() {
        val a = JSONArray()
        list.forEach { a.put(JSONObject().put("name", it.name).put("code", it.code).put("enabled", it.enabled)) }
        file.writeText(a.toString())
    }

    fun add(c: Cheat) { list = list + c; save() }
    fun remove(index: Int) { list = list.filterIndexed { i, _ -> i != index }; save() }
    fun toggle(index: Int, on: Boolean) {
        list = list.mapIndexed { i, c -> if (i == index) c.copy(enabled = on) else c }
        save()
    }

    /** GL thread: hands the enabled codes to the core. */
    fun apply() {
        NativeBridge.cheatReset()
        list.forEachIndexed { i, c ->
            if (c.enabled) {
                // One code per line in the editor; cores take them joined by '+'.
                val code = c.code.lines().map { it.trim() }.filter { it.isNotEmpty() }.joinToString("+")
                NativeBridge.cheatSet(i, true, code)
            }
        }
    }

    /**
     * The cheats screen. `show` opens a dialog keeping the game paused;
     * `changed` runs after any change so the caller can re-apply.
     */
    fun showDialog(
        context: Context,
        show: (MaterialAlertDialogBuilder) -> AlertDialog,
        changed: () -> Unit,
    ) {
        val names = list.map { it.name }.toTypedArray()
        val checked = list.map { it.enabled }.toBooleanArray()
        val builder = MaterialAlertDialogBuilder(context)
            .setTitle("Trapaças")
            .setPositiveButton("Adicionar") { _, _ -> showAdd(context, show, changed) }
            .setNegativeButton("Fechar", null)
        if (list.isEmpty()) {
            builder.setMessage("Nenhuma trapaça ainda. Toque em Adicionar e cole o código " +
                "(GameShark, Action Replay, CodeBreaker…).")
        } else {
            builder.setMultiChoiceItems(names, checked) { _, which, on ->
                toggle(which, on)
                changed()
            }
            if (list.isNotEmpty()) builder.setNeutralButton("Apagar") { _, _ -> showDelete(context, show, changed) }
        }
        show(builder)
    }

    private fun showAdd(
        context: Context,
        show: (MaterialAlertDialogBuilder) -> AlertDialog,
        changed: () -> Unit,
    ) {
        val name = EditText(context).apply { hint = "Nome (ex.: Dinheiro infinito)" }
        val code = EditText(context).apply {
            hint = "Código (um por linha)"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
            minLines = 3
        }
        val box = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 16, 48, 0)
            addView(name)
            addView(code)
        }
        show(MaterialAlertDialogBuilder(context)
            .setTitle("Nova trapaça")
            .setView(box)
            .setPositiveButton("Salvar") { _, _ ->
                val c = code.text.toString().trim()
                if (c.isNotEmpty()) {
                    add(Cheat(name.text.toString().trim().ifEmpty { "Trapaça ${list.size + 1}" }, c, true))
                    changed()
                }
            }
            .setNegativeButton(android.R.string.cancel, null))
    }

    private fun showDelete(
        context: Context,
        show: (MaterialAlertDialogBuilder) -> AlertDialog,
        changed: () -> Unit,
    ) {
        show(MaterialAlertDialogBuilder(context)
            .setTitle("Apagar qual trapaça?")
            .setItems(list.map { it.name }.toTypedArray()) { _, which ->
                remove(which)
                changed()
            })
    }
}
