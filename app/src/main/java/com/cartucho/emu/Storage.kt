package com.cartucho.emu

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.Settings
import java.io.File

/**
 * Getting game files to the cores.
 *
 * Cores open games by path. A folder picked in the system file picker only
 * gives the app a content:// grant, and Android refuses to reopen such a file
 * by path (even through /proc/self/fd), so:
 *  - small games (GBA/GB, at most 32 MB) are copied to the app's cache;
 *  - 3DS games (gigabytes) are opened in place through their real path, which
 *    needs "All files access" (Android 11+) or the storage permission (9-10).
 */
object Storage {
    private const val EXTERNAL_AUTHORITY = "com.android.externalstorage.documents"

    /** The file system path behind a document from internal storage or an SD card. */
    fun realPath(uriString: String): String? {
        val uri = Uri.parse(uriString)
        if (uri.authority != EXTERNAL_AUTHORITY) return null
        val id = try { DocumentsContract.getDocumentId(uri) } catch (e: Exception) { return null }
        val volume = id.substringBefore(':', "")
        val relative = id.substringAfter(':', "")
        if (volume.isEmpty()) return null
        val root = if (volume.equals("primary", ignoreCase = true))
            Environment.getExternalStorageDirectory().path
        else "/storage/$volume"
        return "$root/$relative"
    }

    fun hasFileAccess(context: Context): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) Environment.isExternalStorageManager()
        else context.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED

    /** Path a core can open for this game, or null if it cannot be reached. */
    fun directPath(context: Context, game: Game): String? {
        val path = realPath(game.uri) ?: return null
        return if (File(path).canRead()) path else null
    }

    /** Intent to the system screen that grants "All files access" (Android 11+). */
    fun allFilesAccessIntent(context: Context): Intent =
        Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + context.packageName))
}
