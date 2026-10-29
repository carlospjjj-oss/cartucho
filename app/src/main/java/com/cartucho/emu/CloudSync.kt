package com.cartucho.emu

import android.content.Context
import android.content.Intent
import android.net.Uri

/**
 * Saves in the cloud: one backup file the player picked in the system file
 * picker (Google Drive, OneDrive, Dropbox... any storage app), rewritten after
 * playing. The storage app uploads it; another phone restores from the same
 * file.
 */
object CloudSync {
    private const val KEY_URI = "cloud_uri"
    private const val KEY_LAST = "cloud_last"
    private const val KEY_ERROR = "cloud_error"

    fun uri(context: Context): Uri? = Prefs.raw(context).getString(KEY_URI, null)?.let(Uri::parse)
    fun auto(context: Context) = Prefs.raw(context).getBoolean("cloud_auto", true)
    fun lastSent(context: Context) = Prefs.raw(context).getLong(KEY_LAST, 0L)
    fun lastError(context: Context): String? = Prefs.raw(context).getString(KEY_ERROR, null)

    /** Remembers the file (keeping access to it after restarts). */
    fun setFile(context: Context, uri: Uri) {
        runCatching {
            context.contentResolver.takePersistableUriPermission(uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        }
        Prefs.raw(context).edit().putString(KEY_URI, uri.toString()).remove(KEY_LAST).remove(KEY_ERROR).apply()
    }

    fun forget(context: Context) {
        uri(context)?.let { u ->
            runCatching {
                context.contentResolver.releasePersistableUriPermission(u,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            }
        }
        Prefs.raw(context).edit().remove(KEY_URI).remove(KEY_LAST).remove(KEY_ERROR).apply()
    }

    /** Writes the cloud copy now. Off the main thread; returns the message to show. */
    @Synchronized
    fun send(context: Context): String {
        val target = uri(context) ?: return "Nenhum arquivo de nuvem escolhido."
        return try {
            val r = Backup.export(context, target, light = true)
            Prefs.raw(context).edit().putLong(KEY_LAST, System.currentTimeMillis()).remove(KEY_ERROR).apply()
            "Enviado: ${r.files} arquivos (${r.bytes / 1024} KB)."
        } catch (e: Exception) {
            val m = "Não deu para gravar no arquivo da nuvem (${e.message ?: e.javaClass.simpleName}). " +
                "Escolha o arquivo de novo nas Configurações."
            Prefs.raw(context).edit().putString(KEY_ERROR, m).apply()
            m
        }
    }

    /** After playing: send if anything changed since the last copy. */
    fun sendIfChanged(context: Context) {
        if (uri(context) == null || !auto(context)) return
        if (Backup.lastChange(context) <= lastSent(context)) return
        send(context)
    }

    fun restore(context: Context): String {
        val source = uri(context) ?: return "Nenhum arquivo de nuvem escolhido."
        return try {
            val r = Backup.restore(context, source)
            Prefs.raw(context).edit().putLong(KEY_LAST, System.currentTimeMillis()).apply()
            "Baixado: ${r.files} arquivos (${r.bytes / 1024} KB)."
        } catch (e: Exception) {
            e.message ?: "Não deu para ler o arquivo da nuvem."
        }
    }
}
