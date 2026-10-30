package com.cartucho.emu

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.LinearLayout
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Keeps the app up to date from the GitHub "latest" release: looks for a
 * newer Cartucho-N.apk, downloads it and hands it to the system installer.
 * The player always confirms; nothing installs silently.
 */
object Updater {
    private const val API = "https://api.github.com/repos/carlospjjj-oss/cartucho/releases/tags/latest"
    private const val CHECK_EVERY_MS = 6 * 3600 * 1000L
    private val io = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())

    data class Release(val build: Int, val url: String, val size: Long, val notes: String)

    fun currentBuild(context: Context): Int =
        context.packageManager.getPackageInfo(context.packageName, 0).let {
            if (Build.VERSION.SDK_INT >= 28) it.longVersionCode.toInt() else @Suppress("DEPRECATION") it.versionCode
        }

    /** Fetches the latest release; null when offline or nothing found. */
    fun fetch(): Release? = try {
        val c = URL(API).openConnection() as HttpURLConnection
        c.connectTimeout = 10_000
        c.readTimeout = 15_000
        c.setRequestProperty("Accept", "application/vnd.github+json")
        c.setRequestProperty("User-Agent", "Cartucho")
        val json = JSONObject(c.inputStream.bufferedReader().use { it.readText() })
        val assets = json.getJSONArray("assets")
        var best: Release? = null
        for (i in 0 until assets.length()) {
            val a = assets.getJSONObject(i)
            val n = Regex("""Cartucho-(\d+)\.apk""").matchEntire(a.getString("name"))?.groupValues?.get(1)?.toInt() ?: continue
            if (best == null || n > best.build)
                best = Release(n, a.getString("browser_download_url"), a.optLong("size"), json.optString("body"))
        }
        best
    } catch (e: Exception) {
        null
    }

    /**
     * On start: at most every few hours, and only when the player left the
     * option on. Offers the update in a dialog.
     */
    fun checkOnStart(activity: Activity) {
        val prefs = Prefs.raw(activity)
        if (!prefs.getBoolean("auto_update", true)) return
        val now = System.currentTimeMillis()
        if (now - prefs.getLong("update_checked", 0) < CHECK_EVERY_MS) return
        prefs.edit().putLong("update_checked", now).apply()
        io.execute {
            val r = fetch() ?: return@execute
            val skipped = prefs.getInt("update_skipped", 0)
            if (r.build <= currentBuild(activity) || r.build == skipped) return@execute
            main.post { if (!activity.isFinishing) offer(activity, r, allowSkip = true) }
        }
    }

    /** From the settings: always checks and says what it found. */
    fun checkNow(activity: Activity) {
        val waiting = MaterialAlertDialogBuilder(activity).setMessage("Procurando atualização…").setCancelable(false).show()
        io.execute {
            val r = fetch()
            main.post {
                waiting.dismiss()
                when {
                    r == null -> message(activity, "Não foi possível ver as atualizações agora (sem internet, ou o GitHub não respondeu).")
                    r.build <= currentBuild(activity) -> message(activity, "O Cartucho já está na versão mais nova (0.1.${currentBuild(activity)}).")
                    else -> offer(activity, r, allowSkip = false)
                }
            }
        }
    }

    private fun message(activity: Activity, text: String) {
        MaterialAlertDialogBuilder(activity).setMessage(text).setPositiveButton(android.R.string.ok, null).show()
    }

    private fun offer(activity: Activity, r: Release, allowSkip: Boolean) {
        val notes = r.notes.lines().filter { it.startsWith("- ") }.take(8).joinToString("\n")
        val b = MaterialAlertDialogBuilder(activity)
            .setTitle("Nova versão: 0.1.${r.build}")
            .setMessage(buildString {
                append("Você tem a 0.1.${currentBuild(activity)}.")
                if (r.size > 0) append(" Download de ${r.size / (1024 * 1024)} MB.")
                if (notes.isNotEmpty()) append("\n\nNovidades:\n").append(notes)
            })
            .setPositiveButton("Atualizar") { _, _ -> download(activity, r) }
            .setNegativeButton("Depois", null)
        if (allowSkip) b.setNeutralButton("Pular esta") { _, _ ->
            Prefs.raw(activity).edit().putInt("update_skipped", r.build).apply()
        }
        b.show()
    }

    private fun download(activity: Activity, r: Release) {
        // The system asks once to allow installs from Cartucho.
        if (Build.VERSION.SDK_INT >= 26 && !activity.packageManager.canRequestPackageInstalls()) {
            MaterialAlertDialogBuilder(activity)
                .setMessage("Para atualizar, permita que o Cartucho instale apps. Depois volte aqui e toque em Atualizar de novo.")
                .setPositiveButton("Abrir ajuste") { _, _ ->
                    activity.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${activity.packageName}")))
                }
                .setNegativeButton("Cancelar", null).show()
            Prefs.raw(activity).edit().putLong("update_checked", 0).apply()
            return
        }
        val bar = ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal).apply { max = 100 }
        val label = TextView(activity)
        val box = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            val p = (24 * activity.resources.displayMetrics.density).toInt()
            setPadding(p, p / 2, p, 0)
            addView(bar)
            addView(label)
        }
        var cancelled = false
        val dialog = MaterialAlertDialogBuilder(activity).setTitle("Baixando 0.1.${r.build}").setView(box)
            .setCancelable(false).setNegativeButton("Cancelar") { _, _ -> cancelled = true }.show()
        io.execute {
            val dir = File(activity.cacheDir, "update").apply { deleteRecursively(); mkdirs() }
            val file = File(dir, "Cartucho-${r.build}.apk")
            val ok = try {
                var url = URL(r.url)
                var c: HttpURLConnection
                // GitHub redirects to its file storage.
                while (true) {
                    c = url.openConnection() as HttpURLConnection
                    c.instanceFollowRedirects = false
                    c.setRequestProperty("User-Agent", "Cartucho")
                    c.connectTimeout = 15_000
                    c.readTimeout = 30_000
                    if (c.responseCode in 300..399) { url = URL(url, c.getHeaderField("Location")); continue }
                    break
                }
                val total = c.contentLengthLong.takeIf { it > 0 } ?: r.size
                c.inputStream.use { input ->
                    file.outputStream().use { out ->
                        val buf = ByteArray(64 * 1024)
                        var done = 0L
                        var last = -1
                        while (!cancelled) {
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            done += n
                            val pct = if (total > 0) (done * 100 / total).toInt() else 0
                            if (pct != last) {
                                last = pct
                                main.post { bar.progress = pct; label.text = "${done / (1024 * 1024)} de ${total / (1024 * 1024)} MB" }
                            }
                        }
                    }
                }
                !cancelled
            } catch (e: Exception) {
                false
            }
            main.post {
                dialog.dismiss()
                if (cancelled) return@post
                if (!ok) { message(activity, "O download falhou. Tente de novo mais tarde."); return@post }
                install(activity, file)
            }
        }
    }

    private fun install(activity: Activity, apk: File) {
        try {
            val installer = activity.packageManager.packageInstaller
            val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
            params.setAppPackageName(activity.packageName)
            if (Build.VERSION.SDK_INT >= 31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
            val id = installer.createSession(params)
            installer.openSession(id).use { s ->
                s.openWrite("cartucho.apk", 0, apk.length()).use { out -> apk.inputStream().use { it.copyTo(out) }; s.fsync(out) }
                val intent = Intent(activity, InstallResult::class.java)
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
                s.commit(PendingIntent.getBroadcast(activity, id, intent, flags).intentSender)
            }
        } catch (e: Exception) {
            message(activity, "Não foi possível instalar: ${e.message}")
        }
    }

    /** The installer's answers: asks the player to confirm, or reports a failure. */
    class InstallResult : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                    @Suppress("DEPRECATION")
                    val confirm = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                        else intent.getParcelableExtra(Intent.EXTRA_INTENT)
                    confirm?.let { context.startActivity(it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
                }
                PackageInstaller.STATUS_SUCCESS -> {}
                PackageInstaller.STATUS_FAILURE_ABORTED -> {}
                else -> {
                    val why = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE).orEmpty()
                    android.widget.Toast.makeText(context, "Atualização não instalada. $why", android.widget.Toast.LENGTH_LONG).show()
                }
            }
        }
    }
}
