package com.cartucho.emu

import androidx.annotation.ColorRes

/** The consoles Cartucho plays, and which core runs each. */
enum class GameSystem(
    val shortName: String,
    val fullName: String,
    val extensions: Set<String>,
    /** File name of the core in the APK's native library folder. */
    val coreLibrary: String,
    @ColorRes val color: Int,
    /** Width / height of one picture, for placeholder covers. */
    val coverAspect: Float,
) {
    GBA("GBA", "Game Boy Advance", setOf("gba", "agb"), "libvbam_libretro.so", R.color.gba, 1.5f),
    GB("GB", "Game Boy / Color", setOf("gb", "gbc", "sgb", "dmg", "cgb"), "libvbam_libretro.so", R.color.gb, 1.11f),
    NDS("DS", "Nintendo DS", setOf("nds", "dsi", "ids", "srl"), "libmelondsds_libretro.so", R.color.nds, 0.75f),
    N3DS("3DS", "Nintendo 3DS", setOf("3ds", "cci", "cxi", "3dsx", "app", "elf", "axf"), "libazahar_libretro.so", R.color.n3ds, 1.2f);

    companion object {
        fun forFileName(name: String): GameSystem? {
            val ext = name.substringAfterLast('.', "").lowercase()
            if (ext.isEmpty()) return null
            return entries.firstOrNull { ext in it.extensions }
        }

        fun fromName(name: String?): GameSystem? = entries.firstOrNull { it.name == name }
    }
}
