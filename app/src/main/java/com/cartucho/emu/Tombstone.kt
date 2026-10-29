package com.cartucho.emu

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * What Android itself recorded about the game process dying (Android 11+):
 * the reason, and for native crashes the tombstone with the abort message
 * and the crashing thread's stack (library + offset of each frame), which
 * pinpoint the failing code once matched against the unstripped cores CI
 * keeps.
 */
object Tombstone {
    fun recentCrashes(context: Context, max: Int = 2): String {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return ""
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val infos = try {
            am.getHistoricalProcessExitReasons(context.packageName, 0, 10)
        } catch (e: Exception) {
            return ""
        }
        val fmt = SimpleDateFormat("dd/MM HH:mm:ss", Locale.US)
        val sb = StringBuilder()
        var shown = 0
        for (info in infos) {
            if (shown >= max) break
            val reason = info.reason
            if (reason != ApplicationExitInfo.REASON_CRASH_NATIVE && reason != ApplicationExitInfo.REASON_CRASH &&
                reason != ApplicationExitInfo.REASON_ANR) continue
            shown++
            val kind = when (reason) {
                ApplicationExitInfo.REASON_CRASH_NATIVE -> "crash nativo"
                ApplicationExitInfo.REASON_CRASH -> "crash Java/Kotlin"
                else -> "travou (ANR)"
            }
            sb.appendLine("--- Android: $kind em ${fmt.format(Date(info.timestamp))} (${info.processName}) ---")
            info.description?.let { sb.appendLine(it.take(500)) }
            val bytes = try {
                info.traceInputStream?.use { it.readBytes() }
            } catch (e: Exception) {
                null
            } ?: continue
            if (reason == ApplicationExitInfo.REASON_CRASH_NATIVE && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                sb.append(parse(bytes))
            } else {
                sb.appendLine(String(bytes).take(4000))
            }
        }
        return sb.toString()
    }

    // --- A small protobuf reader, enough for the tombstone ---------------------

    private class Field(val number: Int, val varint: Long, val bytes: ByteArray?)

    /** Top-level fields of a message, or null if `data` is not a valid message. */
    private fun fields(data: ByteArray): List<Field>? {
        val out = mutableListOf<Field>()
        var i = 0
        fun varint(): Long {
            var shift = 0
            var result = 0L
            while (true) {
                if (i >= data.size || shift > 63) throw IllegalStateException()
                val b = data[i++].toInt() and 0xff
                result = result or ((b and 0x7f).toLong() shl shift)
                if (b and 0x80 == 0) return result
                shift += 7
            }
        }
        try {
            while (i < data.size) {
                val key = varint()
                val number = (key ushr 3).toInt()
                if (number <= 0) return null
                when ((key and 7).toInt()) {
                    0 -> out += Field(number, varint(), null)
                    1 -> { if (i + 8 > data.size) return null; i += 8; out += Field(number, 0, null) }
                    2 -> {
                        val len = varint().toInt()
                        if (len < 0 || i + len > data.size) return null
                        out += Field(number, 0, data.copyOfRange(i, i + len))
                        i += len
                    }
                    5 -> { if (i + 4 > data.size) return null; i += 4; out += Field(number, 0, null) }
                    else -> return null
                }
            }
        } catch (e: Exception) {
            return null
        }
        return out
    }

    private fun str(b: ByteArray?) = b?.let { String(it, Charsets.UTF_8) } ?: ""

    /**
     * Tombstone (system/core/debuggerd/proto/tombstone.proto): tid = 6,
     * signal_info = 10, abort_message = 14, threads = 16 (map<uint32, Thread>).
     * Thread: id = 1, name = 2, current_backtrace = 4. BacktraceFrame:
     * rel_pc = 1, function_name = 4, function_offset = 5, file_name = 6,
     * build_id = 8.
     */
    private fun parse(data: ByteArray): String {
        val top = fields(data) ?: return "(tombstone ilegível)\n"
        val sb = StringBuilder()
        val tid = top.firstOrNull { it.number == 6 }?.varint ?: -1
        top.firstOrNull { it.number == 14 }?.let { sb.appendLine("Abort message: ${str(it.bytes).take(800)}") }
        top.firstOrNull { it.number == 10 }?.bytes?.let { fields(it) }?.let { sig ->
            val name = sig.firstOrNull { it.number == 2 }?.let { str(it.bytes) } ?: ""
            val code = sig.firstOrNull { it.number == 4 }?.let { str(it.bytes) } ?: ""
            if (name.isNotEmpty()) sb.appendLine("Sinal: $name $code")
        }
        // Map entries: key (1) = thread id, value (2) = Thread.
        val threads = top.filter { it.number == 16 }.mapNotNull { e ->
            val f = fields(e.bytes ?: return@mapNotNull null) ?: return@mapNotNull null
            val id = f.firstOrNull { it.number == 1 }?.varint ?: return@mapNotNull null
            val thread = f.firstOrNull { it.number == 2 }?.bytes?.let { fields(it) } ?: return@mapNotNull null
            id to thread
        }
        val crashing = threads.firstOrNull { it.first == tid } ?: threads.firstOrNull() ?: return sb.toString()
        val thread = crashing.second
        sb.appendLine("Thread ${crashing.first} \"${str(thread.firstOrNull { it.number == 2 }?.bytes)}\":")
        var n = 0
        for (frame in thread.filter { it.number == 4 }) {
            val f = fields(frame.bytes ?: continue) ?: continue
            val relPc = f.firstOrNull { it.number == 1 }?.varint ?: 0
            val fn = str(f.firstOrNull { it.number == 4 }?.bytes)
            val off = f.firstOrNull { it.number == 5 }?.varint ?: 0
            val file = str(f.firstOrNull { it.number == 6 }?.bytes).substringAfterLast('/')
            val build = str(f.firstOrNull { it.number == 8 }?.bytes)
            sb.append(String.format(Locale.US, "  #%02d pc %08x  %s", n, relPc, file))
            if (fn.isNotEmpty()) sb.append(" ($fn+$off)")
            if (n == 0 && build.isNotEmpty()) sb.append(" [$build]")
            sb.appendLine()
            if (++n >= 40) break
        }
        return sb.toString()
    }
}
