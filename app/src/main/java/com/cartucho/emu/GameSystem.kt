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
    N3DS("3DS", "Nintendo 3DS", setOf("3ds", "cci", "cxi", "3dsx", "app", "elf", "axf"), "libazahar_libretro.so", R.color.n3ds, 1.2f),
    NES("NES", "Nintendo (NES)", setOf("nes", "fds", "unf", "unif"), "libfceumm_libretro.so", R.color.nes, 1.14f),
    SNES("SNES", "Super Nintendo", setOf("sfc", "smc", "swc", "fig", "bs", "st"), "libsnes9x_libretro.so", R.color.snes, 1.4f),
    MD("MD", "Mega Drive", setOf("md", "gen", "smd", "68k", "sgd", "bin"), "libgenesis_plus_gx_libretro.so", R.color.md, 0.72f),
    SMS("SMS", "Master System", setOf("sms", "sg"), "libgenesis_plus_gx_libretro.so", R.color.sms, 0.72f),
    GG("GG", "Game Gear", setOf("gg"), "libgenesis_plus_gx_libretro.so", R.color.gg, 0.72f),
    PS1("PS1", "PlayStation", setOf("cue", "chd", "pbp", "m3u", "ccd"), "libpcsx_rearmed_libretro.so", R.color.ps1, 1.0f);

    /**
     * The core opens the game itself, by path (big disc images, .cue files
     * pointing at their tracks): played in place, which needs all-files access.
     */
    val needsPath: Boolean get() = this == N3DS || this == PS1

    companion object {
        /** Mega Drive cartridges are rarely past 8 MB; bigger .bin files are disc tracks. */
        private const val MAX_MD_BIN = 8L * 1024 * 1024

        fun forFileName(name: String, size: Long = 0L): GameSystem? {
            val ext = name.substringAfterLast('.', "").lowercase()
            if (ext.isEmpty()) return null
            if (ext == "bin" && size > MAX_MD_BIN) return null
            return entries.firstOrNull { ext in it.extensions }
        }

        fun fromName(name: String?): GameSystem? = entries.firstOrNull { it.name == name }
    }
}
