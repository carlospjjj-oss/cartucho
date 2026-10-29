package com.cartucho.emu

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
                    MaterialAlertDialogBuilder(ctx).setMessage(message).setPositiveButton(android.R.string.ok, null).show()
                }
            }.start()
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
