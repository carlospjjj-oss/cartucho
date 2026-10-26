package com.cartucho.emu

import android.app.ActivityManager
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Error reports the player can paste into a message: device, app version,
 * the game that was running, and what the app and the emulators logged
 * right before a problem.
 *
 * Sources, all under files/reports:
 *  - crash-native-*.txt: written by the native crash handler (android_jni.cpp)
 *    from the in-memory log, armed by armNative() when a game starts;
 *  - crash-app-*.txt: uncaught Kotlin exceptions (install());
 *  - error-*.txt: a game that failed to open (gameError());
 *  - session.txt: the game being played, rewritten at every start.
 */
object ErrorReport {
    private fun dir(context: Context) = File(context.filesDir, "reports").apply { mkdirs() }
    private fun stamp() = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())

    /** Records uncaught exceptions of this process before it dies. */
    fun install(context: Context) {
        val app = context.applicationContext
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        if (previous is Handler) return
        Thread.setDefaultUncaughtExceptionHandler(Handler(app, previous))
    }

    private class Handler(
        val context: Context,
        val previous: Thread.UncaughtExceptionHandler?,
    ) : Thread.UncaughtExceptionHandler {
        override fun uncaughtException(t: Thread, e: Throwable) {
            try {
                val sw = StringWriter()
                e.printStackTrace(PrintWriter(sw))
                var text = "APP CRASH in thread ${t.name}\n$sw"
                try {
                    text += "\n--- last log lines ---\n" + NativeBridge.recentLog()
                } catch (ignored: Throwable) {
                }
                File(dir(context), "crash-app-${stamp()}.txt").writeText(text)
            } catch (ignored: Throwable) {
            }
            previous?.uncaughtException(t, e)
        }
    }

    /** A game starts: remember which, and arm the native crash dump. */
    fun gameStarting(context: Context, game: Game) {
        File(dir(context), "session.txt").writeText(
            "Último jogo: ${game.title} (${game.system.fullName})\n" +
                "Arquivo: ${if (game.archive.isNotEmpty()) "${game.archive} → " else ""}${game.fileName}\n" +
                "Tamanho: ${game.size / 1024} KB\n" +
                "Iniciado: ${Date()}\n"
        )
        NativeBridge.setCrashFile(File(dir(context), "crash-native-${stamp()}.txt").path)
    }

    /** The game closed normally: the armed crash file stays empty; drop it. */
    fun gameEnded(context: Context) {
        dir(context).listFiles()?.forEach {
            if (it.name.startsWith("crash-native-") && it.length() == 0L) it.delete()
        }
    }

    /** A game could not be opened. */
    fun gameError(context: Context, message: String) {
        val log = try { NativeBridge.recentLog() } catch (e: Throwable) { "" }
        File(dir(context), "error-${stamp()}.txt").writeText("ERRO: $message\n--- last log lines ---\n$log")
    }

    /** Problems recorded since the player last looked at a report. */
    fun hasNew(context: Context): Boolean {
        val seen = Prefs.raw(context).getLong("report_seen", 0L)
        return problems(context).any { it.lastModified() > seen }
    }

    private fun problems(context: Context) = (dir(context).listFiles() ?: emptyArray())
        .filter { it.name != "session.txt" && it.length() > 0 }
        .sortedByDescending { it.lastModified() }

    fun build(context: Context): String {
        val sb = StringBuilder()
        sb.appendLine("=== Relatório do Cartucho ===")
        sb.appendLine("App: ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})")
        sb.appendLine("Aparelho: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE})")
        sb.appendLine("Android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        sb.appendLine("CPU: ${Build.SUPPORTED_ABIS.joinToString()} · ${Runtime.getRuntime().availableProcessors()} núcleos")
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val mem = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
        sb.appendLine("Memória: ${mem.totalMem / (1024 * 1024)} MB (livre ${mem.availMem / (1024 * 1024)} MB)")
        sb.appendLine("Acesso a todos os arquivos: ${Storage.hasFileAccess(context)}")
        val prefs = Prefs(context)
        sb.appendLine("Filtro ${prefs.filter} · 3DS res ${prefs.n3dsResolution} · layout 3DS ${prefs.n3dsLayout} · DS ${prefs.ndsLayout}")
        File(dir(context), "session.txt").takeIf { it.exists() }?.let { sb.appendLine().append(it.readText()) }
        val list = problems(context).take(3)
        if (list.isEmpty()) sb.appendLine("\nNenhum erro registrado.")
        for (f in list) {
            sb.appendLine().appendLine("--- ${f.name} ---")
            // The tail is what matters; keep the report pasteable.
            sb.appendLine(f.readText().takeLast(12000))
        }
        return sb.toString()
    }

    /** Shows the report with Copy / Share. */
    fun show(activity: AppCompatActivity) {
        val text = build(activity)
        Prefs.raw(activity).edit().putLong("report_seen", System.currentTimeMillis()).apply()
        val view = TextView(activity).apply {
            this.text = text
            textSize = 11f
            setTextIsSelectable(true)
            setPadding(40, 24, 40, 24)
            typeface = android.graphics.Typeface.MONOSPACE
        }
        MaterialAlertDialogBuilder(activity)
            .setTitle("Relatório de erro")
            .setView(ScrollView(activity).apply { addView(view) })
            .setPositiveButton("Compartilhar") { _, _ ->
                activity.startActivity(Intent.createChooser(
                    Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_SUBJECT, "Relatório do Cartucho")
                        .putExtra(Intent.EXTRA_TEXT, text),
                    "Enviar relatório"))
            }
            .setNeutralButton("Copiar") { _, _ ->
                val cm = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("Relatório do Cartucho", text))
            }
            .setNegativeButton("Limpar erros") { _, _ ->
                dir(activity).listFiles()?.forEach { if (it.name != "session.txt") it.delete() }
            }
            .show()
    }
}
