package com.cartucho.emu

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.view.View
import android.view.animation.DecelerateInterpolator
import androidx.core.graphics.ColorUtils
import kotlin.math.cos
import kotlin.math.sin

/**
 * The console-style screens' background: a dark base with soft glowing
 * shapes drifting slowly, tinted with the colour of the selected console and
 * fading to the next one when it changes.
 */
class AnimatedBackground(context: Context) : View(context) {
    private val base = Paint()
    private val glow = Paint(Paint.ANTI_ALIAS_FLAG)
    private var from = Color.rgb(107, 78, 255)
    private var to = from
    private var mix = 1f
    private val start = System.nanoTime()
    /** False stops the drifting (battery saver, or while hidden). */
    var animating = true
        set(value) {
            field = value
            if (value) postInvalidateOnAnimation()
        }

    private val fade = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 600
        interpolator = DecelerateInterpolator()
        addUpdateListener { mix = it.animatedValue as Float; invalidate() }
    }

    /** Fades to a new tint. */
    fun setTint(color: Int) {
        if (color == to) return
        from = current()
        to = color
        fade.cancel()
        mix = 0f
        fade.start()
    }

    private fun current() = ColorUtils.blendARGB(from, to, mix)

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w == 0f || h == 0f) return
        val tint = current()
        val dark = Color.rgb(11, 10, 20)
        base.shader = LinearGradient(0f, 0f, w * 0.4f, h, ColorUtils.blendARGB(dark, tint, 0.28f), dark, Shader.TileMode.CLAMP)
        canvas.drawRect(0f, 0f, w, h, base)
        val t = (System.nanoTime() - start) / 1e9f
        // Three slow blobs on lissajous paths.
        val blobs = listOf(
            Triple(0.25f + 0.12f * sin(t * 0.11f), 0.35f + 0.10f * cos(t * 0.07f), 0.55f),
            Triple(0.78f + 0.10f * cos(t * 0.09f), 0.25f + 0.12f * sin(t * 0.13f), 0.45f),
            Triple(0.55f + 0.15f * sin(t * 0.05f + 1f), 0.85f + 0.08f * cos(t * 0.1f), 0.6f),
        )
        for ((i, b) in blobs.withIndex()) {
            val r = maxOf(w, h) * b.third
            val c = ColorUtils.setAlphaComponent(
                if (i == 1) ColorUtils.blendARGB(tint, Color.WHITE, 0.15f) else tint, if (i == 2) 46 else 64)
            glow.shader = RadialGradient(w * b.first, h * b.second, r, c, Color.TRANSPARENT, Shader.TileMode.CLAMP)
            canvas.drawCircle(w * b.first, h * b.second, r, glow)
        }
        // Slow drift: 30 frames per second is plenty and spares the battery.
        if (animating && isShown) postInvalidateDelayed(33)
    }
}
