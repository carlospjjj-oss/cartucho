package com.cartucho.emu

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import androidx.core.graphics.ColorUtils
import kotlin.math.cos
import kotlin.math.sin

/**
 * The console-style backdrop: a dark base with soft glowing shapes drifting
 * on slow paths, tinted with a console's colour. Shared by the app's screens
 * (AnimatedBackground) and the live wallpaper.
 */
class Backdrop {
    private val base = Paint()
    private val glow = Paint(Paint.ANTI_ALIAS_FLAG)

    /** `t` in seconds; `shift` moves the shapes sideways (wallpaper scrolling, 0..1). */
    fun draw(canvas: Canvas, w: Float, h: Float, tint: Int, t: Float, shift: Float = 0f) {
        if (w <= 0f || h <= 0f) return
        val dark = Color.rgb(11, 10, 20)
        base.shader = LinearGradient(0f, 0f, w * 0.4f, h, ColorUtils.blendARGB(dark, tint, 0.28f), dark, Shader.TileMode.CLAMP)
        canvas.drawRect(0f, 0f, w, h, base)
        val dx = -shift * 0.15f
        val blobs = listOf(
            Triple(0.25f + 0.12f * sin(t * 0.11f) + dx, 0.35f + 0.10f * cos(t * 0.07f), 0.55f),
            Triple(0.78f + 0.10f * cos(t * 0.09f) + dx * 0.6f, 0.25f + 0.12f * sin(t * 0.13f), 0.45f),
            Triple(0.55f + 0.15f * sin(t * 0.05f + 1f) + dx * 1.3f, 0.85f + 0.08f * cos(t * 0.1f), 0.6f),
        )
        for ((i, b) in blobs.withIndex()) {
            val r = maxOf(w, h) * b.third
            val c = ColorUtils.setAlphaComponent(
                if (i == 1) ColorUtils.blendARGB(tint, Color.WHITE, 0.15f) else tint, if (i == 2) 46 else 64)
            glow.shader = RadialGradient(w * b.first, h * b.second, r, c, Color.TRANSPARENT, Shader.TileMode.CLAMP)
            canvas.drawCircle(w * b.first, h * b.second, r, glow)
        }
    }
}
