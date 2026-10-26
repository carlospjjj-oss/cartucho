package com.cartucho.emu

import android.content.Context
import android.content.SharedPreferences
import androidx.preference.PreferenceManager

/** Typed access to the user's settings (res/xml/preferences.xml). */
class Prefs(context: Context) {
    private val p: SharedPreferences = PreferenceManager.getDefaultSharedPreferences(context)

    val filter: Int get() = p.getString("filter", "2")!!.toIntOrNull() ?: 2
    val integerScale: Boolean get() = p.getBoolean("integer_scale", false)
    val showFps: Boolean get() = p.getBoolean("show_fps", false)

    val overlayOpacity: Float get() = p.getInt("overlay_opacity", 60) / 100f
    val overlayScale: Float get() = p.getInt("overlay_scale", 100) / 100f
    val haptics: Boolean get() = p.getBoolean("haptics", true)
    val hideWithGamepad: Boolean get() = p.getBoolean("hide_with_gamepad", true)

    val fastForwardSpeed: Float get() = p.getString("ff_speed", "3")!!.toFloatOrNull() ?: 3f

    val n3dsResolution: String get() = p.getString("n3ds_resolution", "1")!!
    val n3dsNew3ds: Boolean get() = p.getBoolean("n3ds_new3ds", true)
    val n3dsTextureFilter: String get() = p.getString("n3ds_tex_filter", "none")!!
    val n3dsFastShaders: Boolean get() = p.getBoolean("n3ds_fast_shaders", false)
    val n3dsLayout: String get() = p.getString("n3ds_layout", "auto")!!
    val n3dsSwapScreens: Boolean get() = p.getBoolean("n3ds_swap", false)

    val ndsLayout: String get() = p.getString("nds_layout", "auto")!!
    fun setNdsLayout(value: String) = p.edit().putString("nds_layout", value).apply()

    val rewind: Boolean get() = p.getBoolean("rewind", true)
    val quickButtons: Boolean get() = p.getBoolean("quick_buttons", true)

    /** The player's own cover repository ("user/repo" or a URL); empty for none. */
    val coversRepo: String get() = p.getString("covers_repo", "")!!

    val downloadCovers: Boolean get() = p.getBoolean("download_covers", true)

    fun setFilter(value: Int) = p.edit().putString("filter", value.toString()).apply()
    fun setN3dsLayout(value: String) = p.edit().putString("n3ds_layout", value).apply()
    fun setN3dsSwap(value: Boolean) = p.edit().putBoolean("n3ds_swap", value).apply()

    companion object {
        /** Raw store for things that are not user settings (layouts...). */
        fun raw(context: Context): SharedPreferences =
            context.getSharedPreferences("cartucho_state", Context.MODE_PRIVATE)
    }
}
