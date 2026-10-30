package com.cartucho.emu

import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * "Jogar online": two devices play the same game together, each one a
 * controller. One creates the room (player 1) and the other joins (player 2)
 * from the list of rooms on the same Wi-Fi or by address over the internet.
 * The guest's game is replaced by the host's for the session, and the
 * guest's own saves are never touched.
 *
 * Starting and stopping run on the game's thread (glView.queueEvent), which
 * is the one driving the session.
 */
class NetplayUi(
    private val activity: AppCompatActivity,
    private val glView: android.opengl.GLSurfaceView,
    private val game: Game,
    private val gameId: String,
    private val modal: LinkUi.Modal,
) {
    companion object {
        const val PORT = 5740
        const val OFF = 0
        const val WAITING = 1
        const val CONNECTING = 2
        const val SYNCING = 3
        const val PLAYING = 4
        const val CLOSED = 5
        const val FAILED = 6

        /** Consoles whose emulators keep exact, small states (needed to run frames again). */
        fun supported(system: GameSystem) = system !in setOf(GameSystem.NDS, GameSystem.N3DS)
    }

    private val main = Handler(Looper.getMainLooper())
    private var advertiser: LinkDiscovery.Advertiser? = null
    private var browser: LinkDiscovery.Browser? = null
    private val idHash = gameId.hashCode().toLong() and 0xffffffffL

    private fun status() = NativeBridge.netStatus()
    val state get() = status()[0]
    /** Connected or on the way: speed, rewind and loading states stay off. */
    val active get() = state in WAITING..PLAYING
    val playing get() = state == PLAYING

    private fun dialog() = MaterialAlertDialogBuilder(activity)

    fun show() {
        val st = status()
        if (st[0] in WAITING..PLAYING) {
            val who = if (st[3] == 0) "Você é o jogador 1." else "Você é o jogador 2."
            val info = when (st[0]) {
                PLAYING -> "Jogando online. $who\nAtraso da conexão: ${if (st[1] >= 0) "${st[1]} ms" else "medindo…"}"
                else -> NativeBridge.netMessage() ?: ""
            }
            modal.show(dialog()
                .setTitle("Jogo online")
                .setMessage(info)
                .setPositiveButton("Sair da partida") { _, _ -> stop() }
                .setNegativeButton("Fechar", null))
            return
        }
        modal.show(dialog()
            .setTitle("Jogar online")
            .setItems(arrayOf("Criar sala (mesmo Wi-Fi)", "Entrar numa sala (mesmo Wi-Fi)", "🌐 Pela internet")) { _, which ->
                when (which) {
                    0 -> host(internet = false)
                    1 -> browse()
                    else -> internetMenu()
                }
            })
    }

    private fun internetMenu() {
        modal.show(dialog()
            .setTitle("Jogar pela internet")
            .setMessage("Os dois aparelhos precisam se enxergar pela internet. O jeito mais fácil: " +
                "instalem o app Tailscale (grátis) nos dois e entrem na mesma conta — também funciona " +
                "com ZeroTier ou Radmin VPN.\n\nSem esses apps, quem cria a sala precisa abrir a porta " +
                "$PORT (TCP) no roteador.\n\nOs dois precisam ter o mesmo jogo (o mesmo arquivo).")
            .setPositiveButton("Criar sala") { _, _ -> host(internet = true) }
            .setNeutralButton("Entrar por endereço") { _, _ -> typeAddress() }
            .setNegativeButton(android.R.string.cancel, null))
    }

    private fun host(internet: Boolean) {
        glView.queueEvent { NativeBridge.netHost(PORT, gameId) }
        advertiser = LinkDiscovery.Advertiser(activity,
            LinkDiscovery.Announcement(PORT, LinkDiscovery.MODE_NETPLAY, 2, game.title, idHash))
        val ips = LinkDiscovery.localAddresses().joinToString(", ").ifEmpty { "sem rede" }
        if (!internet) {
            waitFor("Esperando o jogador 2…",
                "No outro aparelho, abra o mesmo jogo e toque em Menu → Jogar online → Entrar numa sala.\n" +
                    "Seu endereço: $ips\n\nVocê é o jogador 1; o jogo continua enquanto espera.")
            return
        }
        val vpn = LinkDiscovery.vpnAddresses()
        val base = "No outro aparelho: Menu → Jogar online → Pela internet → Entrar por endereço, e digite " +
            (if (vpn.isNotEmpty()) "o endereço da rede virtual:\n${vpn.joinToString("  ou  ")}"
             else "seu endereço da internet (com a porta $PORT aberta no roteador):")
        val update = waitFor("Esperando pela internet…", base + if (vpn.isEmpty()) "\nbuscando…" else "")
        if (vpn.isEmpty()) Thread {
            val ip = LinkDiscovery.publicAddress()
            main.post { update(base + "\n" + (ip ?: "não foi possível descobrir (sem internet?)")) }
        }.start()
    }

    private fun browse() {
        val list = ListView(activity)
        val rooms = mutableListOf<LinkDiscovery.Host>()
        list.adapter = object : ArrayAdapter<LinkDiscovery.Host>(activity, android.R.layout.simple_list_item_2, android.R.id.text1, rooms) {
            override fun getView(position: Int, convertView: android.view.View?, parent: android.view.ViewGroup): android.view.View {
                val v = super.getView(position, convertView, parent)
                val r = rooms[position]
                v.findViewById<TextView>(android.R.id.text1).text = "${r.game.ifEmpty { "(sem nome)" }} — ${r.name.ifEmpty { r.address }}"
                v.findViewById<TextView>(android.R.id.text2).text =
                    r.address + if (r.crc != idHash) " · outro jogo: abra o mesmo" else " · toque para entrar"
                return v
            }
        }
        val status = TextView(activity).apply {
            text = "Procurando salas no Wi-Fi…"
            setPadding(48, 24, 48, 8)
        }
        val box = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            addView(status)
            addView(list, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 600))
        }
        lateinit var d: AlertDialog
        browser = LinkDiscovery.Browser { found ->
            main.post {
                rooms.clear()
                rooms.addAll(found.filter { it.mode == LinkDiscovery.MODE_NETPLAY })
                (list.adapter as ArrayAdapter<*>).notifyDataSetChanged()
                status.text = if (rooms.isEmpty()) "Procurando salas no Wi-Fi…" else "Toque numa sala para entrar:"
            }
        }
        list.setOnItemClickListener { _, _, position, _ ->
            val r = rooms[position]
            if (r.crc != idHash) {
                toast("Essa sala é de outro jogo (${r.game}). Abra o mesmo arquivo de jogo.")
                return@setOnItemClickListener
            }
            d.dismiss()
            join(r.address, r.port)
        }
        d = modal.show(dialog()
            .setTitle("Entrar numa sala")
            .setView(box)
            .setNeutralButton("Digitar endereço") { _, _ -> typeAddress() }
            .setNegativeButton(android.R.string.cancel, null)) {
            browser?.stop()
            browser = null
        }
    }

    private val recent: List<String>
        get() = Prefs.raw(activity).getString("net_recent", "")!!.split('\n').filter { it.isNotBlank() }

    private fun typeAddress() {
        val input = EditText(activity).apply {
            hint = "192.168.0.10, 100.101.1.2 ou nome"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setText(recent.firstOrNull() ?: "")
            selectAll()
        }
        val box = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 16, 48, 0)
            addView(input)
            for (r in recent.drop(1)) addView(TextView(activity).apply {
                text = "↺ $r"
                setPadding(8, 20, 8, 8)
                setOnClickListener { input.setText(r) }
            })
        }
        modal.show(dialog()
            .setTitle("Endereço de quem criou a sala")
            .setView(box)
            .setPositiveButton("Conectar") { _, _ ->
                val text = input.text.toString().trim()
                if (text.isEmpty()) return@setPositiveButton
                val list = (listOf(text) + recent.filter { it != text }).take(5)
                Prefs.raw(activity).edit().putString("net_recent", list.joinToString("\n")).apply()
                join(text.substringBefore(':'), text.substringAfter(':', "").toIntOrNull() ?: PORT)
            }
            .setNegativeButton(android.R.string.cancel, null))
    }

    private fun join(address: String, port: Int) {
        // Names (a Tailscale machine, say) are looked up on the network thread.
        glView.queueEvent { NativeBridge.netJoin(address, port, gameId) }
        waitFor("Entrando…", "Conectando a $address. O jogo de quem criou a sala vai aparecer aqui; " +
            "seus saves deste jogo não são alterados.")
    }

    /** Progress until playing or failed; returns a function that replaces the text. */
    private fun waitFor(title: String, intro: String): (String) -> Unit {
        var introText = intro
        val text = TextView(activity).apply { setPadding(48, 24, 48, 8); this.text = intro }
        var finished = false
        val since = System.currentTimeMillis()
        val d = modal.show(dialog()
            .setTitle(title)
            .setView(text)
            .setCancelable(false)
            .setNegativeButton(android.R.string.cancel) { _, _ ->
                finished = true
                stop()
            })
        val tick = object : Runnable {
            override fun run() {
                if (finished) return
                val msg = NativeBridge.netMessage().orEmpty()
                text.text = if (msg.isEmpty()) introText else "$introText\n\n$msg"
                when (state) {
                    PLAYING -> {
                        finished = true
                        stopAdvertising()
                        d.dismiss()
                        toast("Conectado! Jogando online como jogador ${status()[3] + 1}.")
                    }
                    OFF -> if (System.currentTimeMillis() - since < 2000) main.postDelayed(this, 250) else {
                        finished = true
                        stopAdvertising()
                        d.dismiss()
                        failure(msg.ifEmpty { "Não foi possível começar." })
                    }
                    FAILED, CLOSED -> {
                        finished = true
                        stopAdvertising()
                        d.dismiss()
                        failure(msg.ifEmpty { "Não foi possível conectar." })
                        glView.queueEvent { NativeBridge.netStop() }
                    }
                    else -> main.postDelayed(this, 250)
                }
            }
        }
        main.postDelayed(tick, 300)
        return { s -> introText = s; text.text = s }
    }

    private fun failure(message: String) {
        modal.show(dialog()
            .setTitle("Jogo online não conectou")
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null))
    }

    fun stop() {
        stopAdvertising()
        glView.queueEvent { NativeBridge.netStop() }
    }

    private fun stopAdvertising() {
        advertiser?.stop()
        advertiser = null
    }

    /** One line for the on-screen display, or null. */
    fun osd(): String? {
        val st = status()
        return when (st[0]) {
            PLAYING -> if (st[2] == 1) "🌐 Aguardando o outro jogador…" else "🌐 J${st[3] + 1} · ${if (st[1] >= 0) "${st[1]} ms" else "…"}"
            CLOSED -> null
            else -> null
        }
    }

    /** The other player left: say it once and go back to playing alone. */
    private var announcedClose = false
    fun takeClosedMessage(): String? {
        if (state != CLOSED) { announcedClose = false; return null }
        if (announcedClose) return null
        announcedClose = true
        val guest = status()[7] == 1
        return (NativeBridge.netMessage() ?: "Partida online encerrada") +
            if (guest) ". Você continua no jogo do outro jogador; seus saves não mudam." else "."
    }

    fun release() {
        stopAdvertising()
        browser?.stop()
        if (active) glView.queueEvent { NativeBridge.netStop() }
    }

    private fun toast(s: String) = Toast.makeText(activity, s, Toast.LENGTH_LONG).show()
}
