package com.cartucho.emu

import android.content.Context
import android.content.SharedPreferences
import androidx.preference.PreferenceManager

/** Typed access to the user's settings (res/xml/preferences.xml). */
class Prefs(private val context: Context, private val game: Game? = null) {
    private val p: SharedPreferences = PreferenceManager.getDefaultSharedPreferences(context)
    // This game's own settings (PerGame), read again on each access so
    // changes apply right away.
    private fun own(key: String): String? =
        game?.let { PerGame.load(context, it).optString(key).ifEmpty { null } }

    private fun str(key: String, default: String): String = own(key) ?: p.getString(key, default)!!

    val filter: Int get() = str("filter", "2").toIntOrNull() ?: 2
    val integerScale: Boolean get() = p.getBoolean("integer_scale", false)
    val showFps: Boolean get() = p.getBoolean("show_fps", false)

    val buttonStyle: ButtonStyle get() = ButtonStyle.of(str("button_style", "modern"))
    val overlayOpacity: Float get() = p.getInt("overlay_opacity", 60) / 100f
    val overlayScale: Float get() = p.getInt("overlay_scale", 100) / 100f
    val haptics: Boolean get() = p.getBoolean("haptics", true)
    val hideWithGamepad: Boolean get() = p.getBoolean("hide_with_gamepad", true)

    val fastForwardSpeed: Float get() = str("ff_speed", "3").toFloatOrNull() ?: 3f

    val n3dsResolution: String get() = p.getString("n3ds_resolution", "1")!!
    val n3dsNew3ds: Boolean get() = p.getBoolean("n3ds_new3ds", true)
    val n3dsTextureFilter: String get() = p.getString("n3ds_tex_filter", "none")!!
    val n3dsFastShaders: Boolean get() = p.getBoolean("n3ds_fast_shaders", false)
    val n3dsLayout: String get() = p.getString("n3ds_layout", "auto")!!
    val n3dsSwapScreens: Boolean get() = p.getBoolean("n3ds_swap", false)

    val ndsLayout: String get() = p.getString("nds_layout", "auto")!!
    fun setNdsLayout(value: String) = p.edit().putString("nds_layout", value).apply()

    val motion: Boolean get() = p.getBoolean("motion", true)
    val vibration: Boolean get() = p.getBoolean("vibration", true)
    val microphone: Boolean get() = p.getBoolean("microphone", true)
    val tvMode: Boolean get() = p.getBoolean("tv_mode", true)
    val runAhead: Int get() = str("run_ahead", "0").toIntOrNull() ?: 0
    val lcdColor: String get() = str("lcd_color", "off")
    val gbPalette: String get() = str("gb_palette", "original gameboy")
    val frameBlend: String get() = str("frame_blend", "disabled")
    /** This game keeps its own on-screen button layout. */
    val ownLayout: Boolean get() = game?.let { PerGame.load(context, it).optBoolean("own_layout") } ?: false
    /** The app-wide value of a setting, for showing what "default" means. */
    fun appValue(key: String, default: String): String = p.getString(key, default)!!
    /** PlayStation: DualShock (analog sticks) instead of the plain pad. */
    val ps1DualShock: Boolean get() = p.getBoolean("ps1_dualshock", true)
    /** PlayStation: an analog stick on the touch screen. */
    val ps1TouchStick: Boolean get() = p.getBoolean("ps1_touch_stick", false)
    /** Library look: "console" (carousel and list, like a game console) or "grid" (covers). */
    val uiStyle: String get() = p.getString("ui_style", "console")!!
    val uiSounds: Boolean get() = p.getBoolean("ui_sounds", true)
    val uiMusic: Boolean get() = p.getBoolean("ui_music", true)
    val uiPreviews: Boolean get() = p.getBoolean("ui_previews", true)
    val uiAnimatedBackground: Boolean get() = p.getBoolean("ui_animated_bg", true)
    val resume: Boolean get() = p.getBoolean("resume", true)
    val rewind: Boolean get() = p.getBoolean("rewind", true)
    val quickButtons: Boolean get() = p.getBoolean("quick_buttons", true)

    /** The player's own cover repository ("user/repo" or a URL); empty for none. */
    val coversRepo: String get() = p.getString("covers_repo", "")!!

    val ssUser: String get() = p.getString("ss_user", "")!!.trim()
    val ssPassword: String get() = p.getString("ss_password", "")!!
    val ssDevId: String get() = p.getString("ss_devid", "")!!.trim()
    val ssDevPassword: String get() = p.getString("ss_devpassword", "")!!

    val tgdbKey: String get() = p.getString("tgdb_key", "")!!.trim()

    val downloadCovers: Boolean get() = p.getBoolean("download_covers", true)

    fun setFilter(value: Int) {
        // A game with its own filter keeps changing its own.
        if (game != null && own("filter") != null) PerGame.set(context, game, "filter", value.toString())
        else p.edit().putString("filter", value.toString()).apply()
    }
    fun setN3dsLayout(value: String) = p.edit().putString("n3ds_layout", value).apply()
    fun setN3dsSwap(value: Boolean) = p.edit().putBoolean("n3ds_swap", value).apply()

    companion object {
        /** Raw store for things that are not user settings (layouts...). */
        fun raw(context: Context): SharedPreferences =
            context.getSharedPreferences("cartucho_state", Context.MODE_PRIVATE)
    }
}
