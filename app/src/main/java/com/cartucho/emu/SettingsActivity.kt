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

        private fun refreshBios() {
            for (bios in Bios.entries) {
                findPreference<Preference>(bios.key)?.summary =
                    if (bios.installed(requireContext())) "Instalada — toque para trocar ou remover"
                    else "Não instalada (usando a substituta embutida) — toque para importar"
            }
        }

        override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
            setPreferencesFromResource(R.xml.preferences, rootKey)
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
                summary = "Game Boy Advance e Game Boy: VBA-M. Nintendo 3DS: Azahar. " +
                    "Ambos são software livre (GPL)."
                isSelectable = false
            })
        }
    }
}
