package com.cartucho.emu

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.view.View
import android.view.animation.DecelerateInterpolator
import androidx.core.graphics.ColorUtils

/**
 * The console-style screens' background: a dark base with soft glowing
 * shapes drifting slowly, tinted with the colour of the selected console and
 * fading to the next one when it changes.
 */
class AnimatedBackground(context: Context) : View(context) {
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

    private val backdrop = Backdrop()

    override fun onDraw(canvas: Canvas) {
        if (width == 0 || height == 0) return
        backdrop.draw(canvas, width.toFloat(), height.toFloat(), current(), (System.nanoTime() - start) / 1e9f)
        // Slow drift: 30 frames per second is plenty and spares the battery.
        if (animating && isShown) postInvalidateDelayed(33)
    }
}
