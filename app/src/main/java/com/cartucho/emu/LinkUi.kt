package com.cartucho.emu

import androidx.appcompat.app.AlertDialog
import android.opengl.GLSurfaceView
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * The link cable screens: host a session (and advertise it on the local
 * network) or join one found on the network or by IP. The game stays paused
 * while connecting. Works between phones and with the desktop VBA-M build of
 * the same version.
 *
 * showModal() opens a dialog that keeps the game paused while it is open.
 */
class LinkUi(
    private val activity: AppCompatActivity,
    private val glView: GLSurfaceView,
    private val game: Game,
    private val romCrc: Long,
    private val modal: Modal,
) {
    /** Shows dialogs that keep the game paused while open. */
    interface Modal {
        fun show(builder: MaterialAlertDialogBuilder, onDismiss: (() -> Unit)? = null): AlertDialog
    }

    private fun showModal(builder: MaterialAlertDialogBuilder, onDismiss: (() -> Unit)? = null) =
        modal.show(builder, onDismiss)

    private val main = Handler(Looper.getMainLooper())
    private var advertiser: LinkDiscovery.Advertiser? = null
    private var browser: LinkDiscovery.Browser? = null

    val connected get() = NativeBridge.linkState() == 0

    /** Game Boy games link with the GB serial cable; GBA games choose. */
    private val isGb get() = game.system == GameSystem.GB

    fun show() {
        if (connected) {
            showModal(dialog()
                .setTitle("Link cable")
                .setMessage(NativeBridge.linkMessage() ?: "Conectado.")
                .setPositiveButton("Desconectar") { _, _ ->
                    glView.queueEvent { NativeBridge.linkStop() }
                    toast("Link desconectado")
                }
                .setNegativeButton("Fechar", null))
            return
        }
        showModal(dialog()
            .setTitle("Link cable pela rede")
            .setItems(arrayOf("Hospedar partida", "Entrar numa partida")) { _, which ->
                if (which == 0) chooseHostMode() else browse()
            })
    }

    private fun dialog() = MaterialAlertDialogBuilder(activity)

    private fun chooseHostMode() {
        if (isGb) {
            host(mode = 2, players = 2)
            return
        }
        val names = arrayOf(
            "Cabo de GBA — trocas e batalhas",
            "Adaptador sem fio — Union Room (Pokémon FireRed, LeafGreen, Emerald)",
        )
        showModal(dialog()
            .setTitle("Tipo de link")
            .setItems(names) { _, which ->
                if (which == 1) host(mode = 1, players = 2) else choosePlayers()
            })
    }

    private fun choosePlayers() {
        showModal(dialog()
            .setTitle("Quantos jogadores?")
            .setItems(arrayOf("2", "3", "4")) { _, which -> host(mode = 0, players = which + 2) })
    }

    private fun host(mode: Int, players: Int) {
        advertiser = LinkDiscovery.Advertiser(
            activity,
            LinkDiscovery.Announcement(LinkDiscovery.DEFAULT_LINK_PORT, mode, players, game.title, romCrc),
        )
        val ips = LinkDiscovery.localAddresses().joinToString(", ").ifEmpty { "sem rede" }
        glView.queueEvent { NativeBridge.linkStart(mode, true, players, "", LinkDiscovery.DEFAULT_LINK_PORT) }
        waitForConnection(
            title = "Esperando jogadores…",
            intro = "${LinkDiscovery.modeName(mode)}, $players jogadores.\n" +
                "No outro aparelho (mesmo Wi-Fi), abra o jogo e toque em Link cable → Entrar numa partida.\n" +
                "Seu IP: $ips",
        )
    }

    private fun browse() {
        val list = ListView(activity)
        val hosts = mutableListOf<LinkDiscovery.Host>()
        list.adapter = object : ArrayAdapter<LinkDiscovery.Host>(activity, android.R.layout.simple_list_item_2, android.R.id.text1, hosts) {
            override fun getView(position: Int, convertView: android.view.View?, parent: android.view.ViewGroup): android.view.View {
                val v = super.getView(position, convertView, parent)
                val h = hosts[position]
                val title = h.game.ifEmpty { "(sem jogo)" }
                v.findViewById<TextView>(android.R.id.text1).text = "$title — ${h.name.ifEmpty { h.address }}"
                var details = "${LinkDiscovery.modeName(h.mode)} · ${h.players} jogadores · ${h.address}"
                if (h.crc != 0L && romCrc != 0L && h.crc != romCrc) details += " · outro jogo"
                v.findViewById<TextView>(android.R.id.text2).text = details
                return v
            }
        }
        val status = TextView(activity).apply {
            text = "Procurando partidas no Wi-Fi…"
            setPadding(48, 24, 48, 8)
        }
        val box = android.widget.LinearLayout(activity).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            addView(status)
            addView(list, android.widget.LinearLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, 600))
        }
        lateinit var d: AlertDialog
        browser = LinkDiscovery.Browser { found ->
            main.post {
                hosts.clear()
                hosts.addAll(found)
                (list.adapter as ArrayAdapter<*>).notifyDataSetChanged()
                status.text = if (found.isEmpty()) "Procurando partidas no Wi-Fi…" else "Toque numa partida para entrar:"
            }
        }
        list.setOnItemClickListener { _, _, position, _ ->
            val h = hosts[position]
            val mode = if (h.mode >= 0) h.mode else if (isGb) 2 else 0
            if (isGb != (mode == 2)) {
                toast("Essa partida é de ${if (mode == 2) "Game Boy" else "GBA"}; abra um jogo do mesmo console.")
                return@setOnItemClickListener
            }
            join(mode, h.players, h.address, h.port)
            d.dismiss()
        }
        d = showModal(dialog()
            .setTitle("Entrar numa partida")
            .setView(box)
            .setNeutralButton("Digitar IP") { _, _ -> typeAddress() }
            .setNegativeButton(android.R.string.cancel, null)) {
            browser?.stop()
            browser = null
        }
    }

    private fun typeAddress() {
        val input = EditText(activity).apply {
            hint = "192.168.0.10"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
        }
        showModal(dialog()
            .setTitle("IP do aparelho que está hospedando")
            .setView(FrameLayout(activity).apply { setPadding(48, 16, 48, 0); addView(input) })
            .setPositiveButton("Conectar") { _, _ ->
                val text = input.text.toString().trim()
                if (text.isNotEmpty()) {
                    val host = text.substringBefore(':')
                    val port = text.substringAfter(':', "").toIntOrNull() ?: LinkDiscovery.DEFAULT_LINK_PORT
                    join(if (isGb) 2 else 0, 2, host, port)
                }
            }
            .setNegativeButton(android.R.string.cancel, null))
    }

    private fun join(mode: Int, players: Int, address: String, port: Int) {
        glView.queueEvent { NativeBridge.linkStart(mode, false, players, address, port) }
        waitForConnection("Conectando…", "Entrando na partida em $address.")
    }

    /** Shows progress until the native side reports connected or failed. */
    private fun waitForConnection(title: String, intro: String) {
        val text = TextView(activity).apply { setPadding(48, 24, 48, 8); this.text = intro }
        var finished = false
        val d = showModal(dialog()
            .setTitle(title)
            .setView(text)
            .setCancelable(false)
            .setNegativeButton(android.R.string.cancel) { _, _ ->
                finished = true
                glView.queueEvent { NativeBridge.linkStop() }
                stopAdvertising()
            })
        val tick = object : Runnable {
            override fun run() {
                if (finished) return
                val state = NativeBridge.linkState()
                val msg = NativeBridge.linkMessage()
                text.text = if (msg.isNullOrEmpty()) intro else "$intro\n\n$msg"
                when (state) {
                    0 -> {
                        finished = true
                        stopAdvertising()
                        d.dismiss()
                        toast("Link conectado! ${msg ?: ""}")
                    }
                    1, -1 -> {
                        finished = true
                        stopAdvertising()
                        d.dismiss()
                        failure(msg ?: "Não foi possível conectar.")
                    }
                    else -> main.postDelayed(this, 250)
                }
            }
        }
        main.postDelayed(tick, 400)
    }

    private fun failure(message: String) {
        showModal(dialog()
            .setTitle("Link não conectou")
            .setMessage("$message\n\nConfira se os dois aparelhos estão no mesmo Wi-Fi e com o mesmo jogo aberto.")
            .setPositiveButton(android.R.string.ok, null))
    }

    private fun stopAdvertising() {
        advertiser?.stop()
        advertiser = null
    }

    fun release() {
        stopAdvertising()
        browser?.stop()
    }

    private fun toast(s: String) = Toast.makeText(activity, s, Toast.LENGTH_LONG).show()
}
