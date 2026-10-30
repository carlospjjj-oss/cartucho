package com.cartucho.emu

import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import com.google.android.material.appbar.MaterialToolbar
import android.widget.LinearLayout
import android.view.View

class SettingsActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            fitsSystemWindows = true
        }
        val toolbar = MaterialToolbar(this).apply {
            title = getString(R.string.settings)
            setNavigationIcon(androidx.appcompat.R.drawable.abc_ic_ab_back_material)
            setNavigationOnClickListener { finish() }
        }
        root.addView(toolbar)
        val container = FrameLayout(this).apply { id = View.generateViewId() }
        root.addView(container, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        setContentView(root)
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction().replace(container.id, SettingsFragment()).commit()
        }
    }

    class SettingsFragment : PreferenceFragmentCompat() {
        private var pendingBios: Bios? = null

        private val pickBios = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            val bios = pendingBios ?: return@registerForActivityResult
            uri ?: return@registerForActivityResult
            val message = when (bios.import(requireContext(), uri)) {
                Bios.Result.OK -> "${bios.title} importada."
                Bios.Result.WRONG_SIZE -> "Esse arquivo não parece ser a ${bios.title} (tamanho diferente de ${bios.size} bytes)."
                Bios.Result.ERROR -> "Não foi possível ler o arquivo."
            }
            Toast.makeText(requireContext(), message, Toast.LENGTH_LONG).show()
            refreshBios()
        }

        private val exportBackup = registerForActivityResult(
            ActivityResultContracts.CreateDocument("application/zip")) { uri ->
            uri ?: return@registerForActivityResult
            runBackup("Fazendo backup…") {
                val r = Backup.export(requireContext(), uri)
                "Backup feito: ${r.files} arquivos (${r.bytes / 1024} KB)."
            }
        }

        private val restoreBackup = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            uri ?: return@registerForActivityResult
            MaterialAlertDialogBuilder(requireContext())
                .setTitle("Restaurar backup?")
                .setMessage("Os saves e estados do backup substituem os atuais com o mesmo nome. Os outros ficam como estão.")
                .setPositiveButton("Restaurar") { _, _ ->
                    runBackup("Restaurando…") {
                        val r = Backup.restore(requireContext(), uri)
                        "Restaurado: ${r.files} arquivos (${r.bytes / 1024} KB)."
                    }
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }

        /** Runs a backup task off the main thread with a waiting dialog. */
        private fun runBackup(title: String, task: () -> String) {
            val ctx = requireContext()
            val waiting = MaterialAlertDialogBuilder(ctx).setTitle(title).setCancelable(false).show()
            Thread {
                val message = try {
                    task()
                } catch (e: Exception) {
                    e.message ?: "Não deu certo (${e.javaClass.simpleName})."
                }
                activity?.runOnUiThread {
                    waiting.dismiss()
                    if (isAdded) refreshCloud()
                    MaterialAlertDialogBuilder(ctx).setMessage(message).setPositiveButton(android.R.string.ok, null).show()
                }
            }.start()
        }

        private val createCloudFile = registerForActivityResult(
            ActivityResultContracts.CreateDocument("application/zip")) { uri ->
            uri ?: return@registerForActivityResult
            CloudSync.setFile(requireContext(), uri)
            runBackup("Enviando para a nuvem…") { CloudSync.send(requireContext()) }
            refreshCloud()
        }

        private val openCloudFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            uri ?: return@registerForActivityResult
            CloudSync.setFile(requireContext(), uri)
            refreshCloud()
            MaterialAlertDialogBuilder(requireContext())
                .setTitle("Baixar os saves desse arquivo?")
                .setMessage("Os saves do arquivo substituem os deste celular com o mesmo nome.")
                .setPositiveButton("Baixar") { _, _ ->
                    runBackup("Baixando da nuvem…") { CloudSync.restore(requireContext()) }
                }
                .setNegativeButton("Agora não", null)
                .show()
        }

        private fun refreshCloud() {
            val ctx = requireContext()
            val uri = CloudSync.uri(ctx)
            val name = uri?.let { u ->
                runCatching {
                    ctx.contentResolver.query(u, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)
                        ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
                }.getOrNull() ?: Uri.decode(u.lastPathSegment ?: "")
            }
            findPreference<Preference>("cloud_file")?.summary = when {
                uri == null -> "Nenhum. Toque para escolher um arquivo no Google Drive, OneDrive, Dropbox…"
                else -> name + (CloudSync.lastError(ctx)?.let { "\n⚠ $it" } ?: "")
            }
            val last = CloudSync.lastSent(ctx)
            findPreference<Preference>("cloud_send")?.summary =
                if (last == 0L) "Ainda não enviado"
                else "Último envio: " + java.text.DateFormat.getDateTimeInstance(
                    java.text.DateFormat.SHORT, java.text.DateFormat.SHORT).format(java.util.Date(last))
            findPreference<Preference>("cloud_send")?.isEnabled = uri != null
            findPreference<Preference>("cloud_restore")?.isEnabled = uri != null
        }

        private fun cloudFile() {
            val ctx = requireContext()
            val items = mutableListOf(
                "Criar um arquivo novo" to { createCloudFile.launch("Cartucho-saves.zip") },
                "Usar o arquivo de outro celular" to { openCloudFile.launch(arrayOf("application/zip", "application/octet-stream", "*/*")) },
            )
            if (CloudSync.uri(ctx) != null) items += "Parar de usar a nuvem" to { CloudSync.forget(ctx); refreshCloud() }
            MaterialAlertDialogBuilder(ctx)
                .setTitle("Saves na nuvem")
                .setItems(items.map { it.first }.toTypedArray()) { _, which -> items[which].second() }
                .show()
        }

        private val pickMusic = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            uri ?: return@registerForActivityResult
            runCatching {
                requireContext().contentResolver.takePersistableUriPermission(uri, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            Prefs.raw(requireContext()).edit().putString("ui_music_uri", uri.toString()).apply()
            refreshMusic()
        }

        private fun refreshMusic() {
            val custom = Prefs.raw(requireContext()).getString("ui_music_uri", null)
            findPreference<Preference>("ui_music_pick")?.summary =
                if (custom == null) "Música do Cartucho — toque para escolher um arquivo seu"
                else "Arquivo escolhido: ${Uri.decode(Uri.parse(custom).lastPathSegment ?: "").substringAfterLast('/')} — toque para trocar"
        }

        private fun refreshRa() {
            val ctx = requireContext()
            findPreference<Preference>("ra_account")?.summary =
                if (RetroAchievements.signedIn(ctx))
                    "Conectado como ${RetroAchievements.user(ctx)} (${RetroAchievements.score(ctx)} pontos) — toque para sair"
                else "Toque para entrar com sua conta (é grátis em retroachievements.org)"
        }

        private fun raAccount() {
            val ctx = requireContext()
            if (RetroAchievements.signedIn(ctx)) {
                MaterialAlertDialogBuilder(ctx)
                    .setTitle(RetroAchievements.user(ctx))
                    .setMessage("Sair da conta do RetroAchievements neste celular?")
                    .setPositiveButton("Sair") { _, _ -> RetroAchievements.signOut(ctx); refreshRa() }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
                return
            }
            val pad = (20 * resources.displayMetrics.density).toInt()
            val user = android.widget.EditText(ctx).apply { hint = "Usuário"; isSingleLine = true }
            val pass = android.widget.EditText(ctx).apply {
                hint = "Senha"
                isSingleLine = true
                inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
            }
            val box = LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(pad, pad / 2, pad, 0)
                addView(user)
                addView(pass)
                addView(android.widget.TextView(ctx).apply {
                    text = "A senha é usada só para entrar; o Cartucho guarda apenas o código de acesso."
                    setPadding(0, pad / 2, 0, 0)
                })
            }
            MaterialAlertDialogBuilder(ctx)
                .setTitle("RetroAchievements")
                .setView(box)
                .setPositiveButton("Entrar") { _, _ ->
                    val u = user.text.toString().trim()
                    val p = pass.text.toString()
                    if (u.isEmpty() || p.isEmpty()) return@setPositiveButton
                    val waiting = MaterialAlertDialogBuilder(ctx).setTitle("Entrando…").setCancelable(false).show()
                    Thread {
                        val error = RetroAchievements.signIn(ctx, u, p)
                        activity?.runOnUiThread {
                            waiting.dismiss()
                            refreshRa()
                            MaterialAlertDialogBuilder(ctx)
                                .setMessage(error ?: "Pronto! As conquistas aparecem quando você joga.")
                                .setPositiveButton(android.R.string.ok, null)
                                .show()
                        }
                    }.start()
                }
                .setNeutralButton("Criar conta") { _, _ ->
                    startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW,
                        android.net.Uri.parse("https://retroachievements.org/createaccount.php")))
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }

        private fun refreshBios() {
            for (bios in Bios.entries) {
                findPreference<Preference>(bios.key)?.summary =
                    if (bios.installed(requireContext())) "Instalada — toque para trocar ou remover"
                    else "Não instalada (usando a substituta embutida) — toque para importar"
            }
        }

        override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
            setPreferencesFromResource(R.xml.preferences, rootKey)
            // Passwords: hidden while typing.
            for (key in listOf("ss_password", "ss_devpassword")) {
                findPreference<androidx.preference.EditTextPreference>(key)?.setOnBindEditTextListener {
                    it.inputType = android.text.InputType.TYPE_CLASS_TEXT or
                        android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
                }
            }
            findPreference<Preference>("ss_status")?.summary =
                Prefs.raw(requireContext()).getString("ss_status", null) ?: "Ainda não usado"
            findPreference<Preference>("tgdb_status")?.summary =
                Prefs.raw(requireContext()).getString("tgdb_status", null) ?: "Ainda não usado"
            findPreference<Preference>("media_clear")?.apply {
                val media = GameMedia(requireContext(), Covers(requireContext()))
                summary = "Ocupando ${media.sizeBytes() / (1024 * 1024)} MB"
                setOnPreferenceClickListener {
                    media.clear()
                    summary = "Ocupando 0 MB"
                    true
                }
            }
            findPreference<Preference>("update_now")?.apply {
                summary = "Versão instalada: 0.1.${Updater.currentBuild(requireContext())}"
                setOnPreferenceClickListener { Updater.checkNow(requireActivity()); true }
            }
            findPreference<Preference>("photo_credits")?.setOnPreferenceClickListener {
                MaterialAlertDialogBuilder(requireContext())
                    .setTitle("Fotos dos consoles")
                    .setMessage("Fotos de Evan-Amos, do Wikimedia Commons (commons.wikimedia.org), com o fundo removido e o tamanho reduzido.\n\nGame-Boy-FL.jpg — Evan-Amos, Public domain\nNintendo-Game-Boy-Advance-Purple-FL.jpg — Evan-Amos, Public domain\nNintendo-DS-Fat-Blue.jpg — Evan-Amos, Public domain\nNintendo-3DS-AquaOpen.jpg — Evan-Amos, Public domain\nNES-Console-Set.png — Evan-Amos, Public domain\nSNES-Mod1-Console-Set.png — Evan-Amos, CC BY-SA 3.0\nSega-Mega-Drive-JP-Mk1-Console-Set.png — Evan-Amos, Public domain\nSega-Master-System-Set.png — Evan-Amos, Public domain\nGame-Gear-Handheld.jpg — Evan-Amos, Public domain\nPSX-Console-wController.png — Evan-Amos, Public domain\n\nAs marcas nos aparelhos pertencem aos seus donos; o Cartucho não tem ligação com eles.")
                    .setPositiveButton(android.R.string.ok, null).show()
                true
            }
            findPreference<Preference>("backup_export")?.setOnPreferenceClickListener {
                exportBackup.launch(Backup.defaultName()); true
            }
            findPreference<Preference>("backup_restore")?.setOnPreferenceClickListener {
                restoreBackup.launch(arrayOf("application/zip", "application/octet-stream", "*/*")); true
            }
            findPreference<Preference>("ra_account")?.setOnPreferenceClickListener {
                raAccount(); true
            }
            refreshRa()
            refreshMusic()
            findPreference<Preference>("ui_music_pick")?.setOnPreferenceClickListener {
                val hasCustom = Prefs.raw(requireContext()).getString("ui_music_uri", null) != null
                val items = mutableListOf("Escolher um arquivo de música")
                if (hasCustom) items += "Voltar para a música do Cartucho"
                MaterialAlertDialogBuilder(requireContext())
                    .setTitle("Música de fundo")
                    .setItems(items.toTypedArray()) { _, which ->
                        if (which == 0) pickMusic.launch(arrayOf("audio/*"))
                        else { Prefs.raw(requireContext()).edit().remove("ui_music_uri").apply(); refreshMusic() }
                    }
                    .show()
                true
            }
            findPreference<Preference>("cloud_file")?.setOnPreferenceClickListener { cloudFile(); true }
            findPreference<Preference>("cloud_send")?.setOnPreferenceClickListener {
                runBackup("Enviando para a nuvem…") { CloudSync.send(requireContext()) }
                true
            }
            findPreference<Preference>("cloud_restore")?.setOnPreferenceClickListener {
                MaterialAlertDialogBuilder(requireContext())
                    .setTitle("Baixar da nuvem?")
                    .setMessage("Os saves do arquivo substituem os deste celular com o mesmo nome.")
                    .setPositiveButton("Baixar") { _, _ ->
                        runBackup("Baixando da nuvem…") { CloudSync.restore(requireContext()) }
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
                true
            }
            refreshCloud()
            findPreference<Preference>("pad_map_open")?.setOnPreferenceClickListener {
                PadMap.showDialog(requireContext()); true
            }
            findPreference<Preference>("error_report")?.setOnPreferenceClickListener {
                ErrorReport.show(requireActivity() as AppCompatActivity); true
            }
            for (bios in Bios.entries) {
                findPreference<Preference>(bios.key)?.setOnPreferenceClickListener {
                    if (bios.installed(requireContext())) {
                        MaterialAlertDialogBuilder(requireContext())
                            .setTitle(bios.title)
                            .setPositiveButton("Trocar") { _, _ -> pendingBios = bios; pickBios.launch(arrayOf("*/*")) }
                            .setNegativeButton("Remover") { _, _ -> bios.remove(requireContext()); refreshBios() }
                            .setNeutralButton(android.R.string.cancel, null)
                            .show()
                    } else {
                        pendingBios = bios
                        pickBios.launch(arrayOf("*/*"))
                    }
                    true
                }
            }
            refreshBios()
            preferenceScreen.addPreference(Preference(requireContext()).apply {
                title = "Cartucho ${BuildConfig.VERSION_NAME}"
                summary = "Game Boy Advance e Game Boy: VBA-M. Nintendo DS: melonDS. Nintendo 3DS: Azahar. " +
                    "NES: FCEUmm. Super Nintendo: Snes9x. Mega Drive, Master System e Game Gear: " +
                    "Genesis Plus GX. PlayStation: PCSX ReARMed. " +
                    "Ambos são software livre (GPL)."
                isSelectable = false
            })
        }
    }
}
