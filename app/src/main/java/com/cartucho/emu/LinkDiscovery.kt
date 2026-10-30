package com.cartucho.emu

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import kotlin.concurrent.thread

/**
 * Finds link servers on the local network without typing addresses.
 *
 * Same wire protocol as the VBA-M desktop patch (src/qt/link-discovery.*), so
 * phones and PCs find each other: a browser broadcasts
 * {"vbam":"link-query","v":1} to UDP port 5739 once a second; a waiting
 * server answers with {"vbam":"link-host","v":1,"port":..,"mode":..,...}.
 */
object LinkDiscovery {
    const val DISCOVERY_PORT = 5739
    const val DEFAULT_LINK_PORT = 5738
    private const val VERSION = 1

    /** VBA-M LinkMode values, as sent on the wire. */
    const val WIRE_CABLE = 2
    const val WIRE_RFU = 4
    const val WIRE_GB = 7

    /** Cartucho link mode (0 cable, 1 wireless adapter, 2 GB) -> wire value. */
    fun wireMode(mode: Int) = when (mode) { 1 -> WIRE_RFU; 2 -> WIRE_GB; else -> WIRE_CABLE }
    fun modeFromWire(wire: Int) = when (wire) { WIRE_RFU -> 1; WIRE_GB -> 2; WIRE_CABLE -> 0; else -> -1 }

    /** Announcements of online-play rooms (not link cable) carry this mode. */
    const val MODE_NETPLAY = 9

    fun modeName(mode: Int) = when (mode) {
        MODE_NETPLAY -> "Jogo online"
        1 -> "Adaptador sem fio"
        2 -> "Cabo de Game Boy"
        else -> "Cabo de GBA"
    }

    data class Host(
        val address: String,
        val port: Int,
        val mode: Int,       // Cartucho mode, -1 if unknown
        val players: Int,
        val game: String,
        val crc: Long,
        val name: String,
        val version: String,
        val seenAt: Long,
    )

    data class Announcement(val port: Int, val mode: Int, val players: Int, val game: String, val crc: Long)

    private fun query(): ByteArray =
        JSONObject().put("vbam", "link-query").put("v", VERSION).toString().toByteArray()

    private fun announcement(a: Announcement): ByteArray = JSONObject()
        .put("vbam", "link-host").put("v", VERSION)
        .put("port", a.port).put("mode", wireMode(a.mode)).put("players", a.players)
        .apply { if (a.mode == MODE_NETPLAY) put("cartucho_net", 1) }
        .put("game", a.game.take(64)).put("crc", java.lang.Long.toHexString(a.crc))
        .put("name", (Build.MODEL ?: "Android").take(64))
        .put("ver", "Cartucho ${BuildConfig.VERSION_NAME}")
        .toString().toByteArray()

    private fun parse(data: ByteArray, len: Int): JSONObject? = try {
        if (len > 1024) null else JSONObject(String(data, 0, len)).takeIf { it.optInt("v") == VERSION }
    } catch (e: Exception) {
        null
    }

    /** Every IPv4 broadcast address worth trying, limited broadcast first. */
    private fun broadcastTargets(): List<InetAddress> {
        val out = mutableListOf<InetAddress>(InetAddress.getByName("255.255.255.255"))
        try {
            for (iface in NetworkInterface.getNetworkInterfaces()) {
                if (!iface.isUp || iface.isLoopback) continue
                for (addr in iface.interfaceAddresses) {
                    val b = addr.broadcast ?: continue
                    if (b is Inet4Address && out.none { it == b }) out += b
                }
            }
        } catch (e: Exception) {
        }
        return out
    }

    /** This phone's Wi-Fi/hotspot IPv4 addresses, for showing to the player. */
    /**
     * Addresses on a virtual network (Tailscale, ZeroTier, Radmin, Hamachi…),
     * which reach the other player over the internet: VPN interfaces and
     * Tailscale's 100.64.0.0/10 range.
     */
    fun vpnAddresses(): List<String> = try {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { nif ->
                nif.inetAddresses.toList().filterIsInstance<Inet4Address>().filter { a ->
                    val b = a.address
                    val tailscale = (b[0].toInt() and 0xff) == 100 && (b[1].toInt() and 0xc0) == 64
                    tailscale || nif.name.startsWith("tun") || nif.name.startsWith("zt") || nif.name.startsWith("tailscale")
                }.map { it.hostAddress ?: "" }
            }
            .filter { it.isNotEmpty() }
    } catch (e: Exception) {
        emptyList()
    }

    /** This connection's address on the internet (asks a public service), or null. */
    fun publicAddress(): String? = try {
        val conn = java.net.URL("https://api.ipify.org").openConnection() as java.net.HttpURLConnection
        conn.connectTimeout = 6000
        conn.readTimeout = 6000
        conn.inputStream.bufferedReader().use { it.readText().trim() }.takeIf { it.matches(Regex("[0-9.]+")) }
    } catch (e: Exception) {
        null
    }

    fun localAddresses(): List<String> = try {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.inetAddresses.toList() }
            .filterIsInstance<Inet4Address>()
            .map { it.hostAddress ?: "" }
            .filter { it.isNotEmpty() }
    } catch (e: Exception) {
        emptyList()
    }

    /** Answers discovery queries until stop(). */
    class Advertiser(context: Context, private val ann: Announcement) {
        @Volatile private var running = true
        private var socket: DatagramSocket? = null
        private val lock: WifiManager.MulticastLock? = try {
            (context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager)
                .createMulticastLock("cartucho-link").apply { setReferenceCounted(false); acquire() }
        } catch (e: Exception) {
            null
        }

        init {
            thread(name = "link-advertiser", isDaemon = true) {
                try {
                    val s = DatagramSocket(null).apply {
                        reuseAddress = true
                        soTimeout = 500
                        bind(InetSocketAddress(DISCOVERY_PORT))
                    }
                    socket = s
                    val buf = ByteArray(1500)
                    val reply = announcement(ann)
                    while (running) {
                        val p = DatagramPacket(buf, buf.size)
                        try {
                            s.receive(p)
                        } catch (e: SocketTimeoutException) {
                            continue
                        }
                        val o = parse(p.data, p.length) ?: continue
                        if (o.optString("vbam") != "link-query") continue
                        s.send(DatagramPacket(reply, reply.size, p.address, p.port))
                    }
                } catch (e: Exception) {
                    // Port taken (another app advertising): joining by IP still works.
                } finally {
                    socket?.close()
                }
            }
        }

        fun stop() {
            running = false
            socket?.close()
            try { lock?.release() } catch (e: Exception) {}
        }
    }

    /** Queries the network every second; onChange gets the current list (any thread). */
    class Browser(private val onChange: (List<Host>) -> Unit) {
        @Volatile private var running = true
        private var socket: DatagramSocket? = null
        private val hosts = LinkedHashMap<String, Host>()

        init {
            thread(name = "link-browser", isDaemon = true) {
                try {
                    val s = DatagramSocket().apply { broadcast = true; soTimeout = 250 }
                    socket = s
                    val q = query()
                    val buf = ByteArray(1500)
                    var lastQuery = 0L
                    while (running) {
                        val now = System.currentTimeMillis()
                        if (now - lastQuery >= 1000) {
                            lastQuery = now
                            for (t in broadcastTargets())
                                try { s.send(DatagramPacket(q, q.size, t, DISCOVERY_PORT)) } catch (e: Exception) {}
                            if (synchronized(hosts) { hosts.values.removeAll { now - it.seenAt > 3500 } })
                                publish()
                        }
                        val p = DatagramPacket(buf, buf.size)
                        try {
                            s.receive(p)
                        } catch (e: SocketTimeoutException) {
                            continue
                        }
                        val o = parse(p.data, p.length) ?: continue
                        if (o.optString("vbam") != "link-host") continue
                        val port = o.optInt("port")
                        if (port !in 1..65535) continue
                        val addr = p.address.hostAddress ?: continue
                        val h = Host(
                            address = addr, port = port,
                            mode = if (o.optInt("cartucho_net") == 1) MODE_NETPLAY else modeFromWire(o.optInt("mode")),
                            players = o.optInt("players", 2), game = o.optString("game").trim(),
                            crc = o.optString("crc").toLongOrNull(16) ?: 0L,
                            name = o.optString("name").trim(), version = o.optString("ver").trim(),
                            seenAt = System.currentTimeMillis(),
                        )
                        val key = "$addr:$port"
                        val changed = synchronized(hosts) {
                            val old = hosts.put(key, h)
                            old == null || old.copy(seenAt = 0) != h.copy(seenAt = 0)
                        }
                        if (changed) publish()
                    }
                } catch (e: Exception) {
                } finally {
                    socket?.close()
                }
            }
        }

        private fun publish() = onChange(synchronized(hosts) { hosts.values.toList() })

        fun stop() {
            running = false
            socket?.close()
        }
    }
}
