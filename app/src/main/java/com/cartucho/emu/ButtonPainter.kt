package com.cartucho.emu

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import kotlin.math.min

/** How the on-screen buttons look. */
enum class ButtonStyle(val key: String) {
    /** Solid, colored, softly lit buttons with a shadow (like a real pad). */
    MODERN("modern"),
    /** Dark glass with glowing colored outlines. */
    NEON("neon"),
    /** Light, translucent and colorless (the original look). */
    CLASSIC("classic");

    companion object {
        fun of(key: String?) = entries.firstOrNull { it.key == key } ?: MODERN
    }
}

/**
 * Draws the controls of [ControllerOverlay]. Colors follow each console:
 * Game Boy's magenta A/B, Game Boy Advance's violet, and the red / yellow /
 * blue / green A B X Y of the DS and 3DS.
 */
class ButtonPainter(private val density: Float) {
    var style = ButtonStyle.MODERN
    var system = GameSystem.GBA

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }
    private val path = Path()
    private val rect = RectF()

    // --- Colors -----------------------------------------------------------------

    private val accent = Color.rgb(124, 92, 255)
    private val slate = Color.rgb(58, 66, 84)
    private val dark = Color.rgb(34, 38, 48)

    /** Main color of a control. */
    fun colorOf(id: String, kind: ControllerOverlay.Kind): Int = when (kind) {
        ControllerOverlay.Kind.FAST_FORWARD -> Color.rgb(255, 167, 38)
        ControllerOverlay.Kind.REWIND -> Color.rgb(41, 182, 246)
        ControllerOverlay.Kind.QUICK_SAVE -> Color.rgb(102, 187, 106)
        ControllerOverlay.Kind.QUICK_LOAD -> Color.rgb(171, 71, 188)
        ControllerOverlay.Kind.MENU -> slate
        ControllerOverlay.Kind.DPAD, ControllerOverlay.Kind.STICK -> slate
        ControllerOverlay.Kind.BUTTON -> when (id) {
            "a" -> when (system) {
                GameSystem.GB -> Color.rgb(196, 30, 98)
                GameSystem.GBA -> Color.rgb(124, 77, 255)
                else -> Color.rgb(239, 68, 68)
            }
            "b" -> when (system) {
                GameSystem.GB -> Color.rgb(196, 30, 98)
                GameSystem.GBA -> Color.rgb(236, 64, 122)
                else -> Color.rgb(250, 189, 35)
            }
            "x" -> Color.rgb(59, 130, 246)
            "y" -> Color.rgb(34, 197, 94)
            "mic" -> Color.rgb(20, 184, 166)
            "l", "r", "zl", "zr" -> slate
            else -> dark  // start, select
        }
    }

    // --- Helpers ----------------------------------------------------------------

    private fun alpha(color: Int, a: Float): Int =
        Color.argb((Color.alpha(color) * a).toInt().coerceIn(0, 255), Color.red(color), Color.green(color), Color.blue(color))

    private fun mix(color: Int, other: Int, t: Float): Int = Color.rgb(
        (Color.red(color) + (Color.red(other) - Color.red(color)) * t).toInt(),
        (Color.green(color) + (Color.green(other) - Color.green(color)) * t).toInt(),
        (Color.blue(color) + (Color.blue(other) - Color.blue(color)) * t).toInt(),
    )

    private fun lighter(c: Int, t: Float) = mix(c, Color.WHITE, t)
    private fun darker(c: Int, t: Float) = mix(c, Color.BLACK, t)

    /** Overall alpha from the transparency setting (0.15..1). */
    private fun level(opacity: Float) = when (style) {
        ButtonStyle.CLASSIC -> opacity
        // Solid styles read well a bit stronger than the slider says.
        else -> (0.35f + 0.65f * opacity).coerceAtMost(1f)
    }

    // --- Round and pill buttons ---------------------------------------------------

    /**
     * A button centred at (x, y) with half sizes hw / hh; round when equal,
     * a pill otherwise. [icon] draws a symbol instead of [label].
     */
    fun drawButton(
        canvas: Canvas, x: Float, y: Float, hw: Float, hh: Float, round: Boolean,
        color: Int, label: String, icon: Icon?, pressed: Boolean, active: Boolean, opacity: Float,
    ) {
        val a = level(opacity)
        rect.set(x - hw, y - hh, x + hw, y + hh)
        val radius = if (round) hw else hh
        when (style) {
            ButtonStyle.MODERN -> {
                val base = if (active) lighter(color, 0.15f) else color
                val sink = if (pressed) 0f else 2.5f * density
                // Shadow under the button; pressing pushes the button into it.
                fill.shader = null
                fill.color = alpha(Color.BLACK, 0.35f * a)
                rect.offset(0f, 3f * density)
                canvas.drawRoundRect(rect, radius, radius, fill)
                rect.offset(0f, -3f * density + (3f * density - sink))
                val top = if (pressed) darker(base, 0.1f) else lighter(base, 0.28f)
                val bottom = if (pressed) darker(base, 0.3f) else darker(base, 0.12f)
                fill.shader = LinearGradient(0f, rect.top, 0f, rect.bottom,
                    alpha(top, a), alpha(bottom, a), Shader.TileMode.CLAMP)
                canvas.drawRoundRect(rect, radius, radius, fill)
                fill.shader = null
                // Thin light rim at the top edge.
                stroke.strokeWidth = 1.2f * density
                stroke.shader = LinearGradient(0f, rect.top, 0f, rect.bottom,
                    alpha(Color.WHITE, 0.45f * a), alpha(Color.WHITE, 0f), Shader.TileMode.CLAMP)
                rect.inset(0.6f * density, 0.6f * density)
                canvas.drawRoundRect(rect, radius, radius, stroke)
                stroke.shader = null
                drawLabel(canvas, x, y + (3f * density - sink) - 1.5f * density, hw, hh, label, icon,
                    alpha(Color.WHITE, (0.95f * a).coerceAtLeast(0.6f)), shadow = true)
            }
            ButtonStyle.NEON -> {
                fill.shader = null
                fill.color = alpha(if (pressed) darker(color, 0.25f) else Color.rgb(12, 14, 22), (if (pressed) 0.85f else 0.55f) * a)
                canvas.drawRoundRect(rect, radius, radius, fill)
                val glow = if (pressed || active) lighter(color, 0.35f) else color
                stroke.strokeWidth = 2.5f * density
                stroke.color = alpha(glow, a)
                stroke.setShadowLayer(8f * density, 0f, 0f, alpha(glow, 0.9f * a))
                rect.inset(1.5f * density, 1.5f * density)
                canvas.drawRoundRect(rect, radius, radius, stroke)
                stroke.clearShadowLayer()
                drawLabel(canvas, x, y, hw, hh, label, icon, alpha(lighter(color, 0.55f), a.coerceAtLeast(0.6f)), shadow = false)
            }
            ButtonStyle.CLASSIC -> {
                val o = (opacity * 255).toInt().coerceIn(20, 255) / 255f
                fill.shader = null
                fill.color = if (pressed || active) Color.argb((o * 0.85f * 255).toInt(), 107, 78, 255)
                             else Color.argb((o * 0.35f * 255).toInt(), 255, 255, 255)
                canvas.drawRoundRect(rect, radius, radius, fill)
                stroke.strokeWidth = 2f * density
                stroke.color = Color.argb((o * 0.9f * 255).toInt(), 255, 255, 255)
                canvas.drawRoundRect(rect, radius, radius, stroke)
                drawLabel(canvas, x, y, hw, hh, label, icon,
                    Color.argb((o * 255).toInt().coerceAtLeast(120), 255, 255, 255), shadow = false)
            }
        }
    }

    enum class Icon { MENU, FAST_FORWARD, REWIND, SAVE, LOAD, MIC }

    private fun drawLabel(canvas: Canvas, x: Float, y: Float, hw: Float, hh: Float, label: String,
                          icon: Icon?, color: Int, shadow: Boolean) {
        if (shadow) text.setShadowLayer(2f * density, 0f, 1f * density, Color.argb(110, 0, 0, 0))
        else text.clearShadowLayer()
        if (icon != null) {
            drawIcon(canvas, icon, x, y, min(hw, hh) * 0.5f, color, shadow)
            return
        }
        text.color = color
        text.textSize = min(hw, hh) * if (label.length > 2) 0.58f else 0.9f
        text.letterSpacing = if (label.length > 2) 0.06f else 0f
        canvas.drawText(label, x, y - (text.descent() + text.ascent()) / 2f, text)
    }

    private fun drawIcon(canvas: Canvas, icon: Icon, x: Float, y: Float, s: Float, color: Int, shadow: Boolean) {
        fill.shader = null
        fill.color = color
        if (shadow) fill.setShadowLayer(2f * density, 0f, 1f * density, Color.argb(110, 0, 0, 0))
        path.reset()
        when (icon) {
            Icon.MENU -> {
                val w = s * 0.9f
                val t = s * 0.16f
                for (k in -1..1) {
                    rect.set(x - w, y + k * s * 0.55f - t, x + w, y + k * s * 0.55f + t)
                    canvas.drawRoundRect(rect, t, t, fill)
                }
            }
            Icon.FAST_FORWARD, Icon.REWIND -> {
                val dir = if (icon == Icon.FAST_FORWARD) 1f else -1f
                for (k in 0..1) {
                    val ox = x + dir * (k * s * 0.8f - s * 0.45f)
                    path.moveTo(ox - dir * s * 0.45f, y - s * 0.65f)
                    path.lineTo(ox + dir * s * 0.55f, y)
                    path.lineTo(ox - dir * s * 0.45f, y + s * 0.65f)
                    path.close()
                }
                canvas.drawPath(path, fill)
            }
            Icon.SAVE, Icon.LOAD -> {
                // Arrow into (save) or out of (load) a tray.
                val up = icon == Icon.LOAD
                val t = s * 0.17f
                rect.set(x - t, y - s * 0.75f + (if (up) s * 0.35f else 0f), x + t, y + s * 0.15f + (if (up) s * 0.2f else 0f))
                canvas.drawRect(rect, fill)
                if (up) {
                    path.moveTo(x - s * 0.5f, y - s * 0.3f); path.lineTo(x, y - s * 0.85f); path.lineTo(x + s * 0.5f, y - s * 0.3f)
                } else {
                    path.moveTo(x - s * 0.5f, y - s * 0.05f); path.lineTo(x, y + s * 0.5f); path.lineTo(x + s * 0.5f, y - s * 0.05f)
                }
                path.close()
                canvas.drawPath(path, fill)
                rect.set(x - s * 0.8f, y + s * 0.62f, x + s * 0.8f, y + s * 0.62f + t * 1.6f)
                canvas.drawRoundRect(rect, t, t, fill)
            }
            Icon.MIC -> {
                rect.set(x - s * 0.3f, y - s * 0.8f, x + s * 0.3f, y + s * 0.25f)
                canvas.drawRoundRect(rect, s * 0.3f, s * 0.3f, fill)
                stroke.strokeWidth = s * 0.15f
                stroke.color = color
                rect.set(x - s * 0.55f, y - s * 0.45f, x + s * 0.55f, y + s * 0.45f)
                canvas.drawArc(rect, 0f, 180f, false, stroke)
                canvas.drawLine(x, y + s * 0.45f, x, y + s * 0.8f, stroke)
            }
        }
        fill.clearShadowLayer()
    }

    // --- D-pad --------------------------------------------------------------------

    /** The cross, [mask] holding the pressed directions (bits Retro.UP...). */
    fun drawDpad(canvas: Canvas, x: Float, y: Float, half: Float, mask: Int, opacity: Float) {
        val arm = half * 0.36f
        val a = level(opacity)
        val corner = arm * 0.35f
        fun cross(p: Path, inset: Float) {
            p.reset()
            p.addRoundRect(RectF(x - arm + inset, y - half + inset, x + arm - inset, y + half - inset), corner, corner, Path.Direction.CW)
            val across = Path()
            across.addRoundRect(RectF(x - half + inset, y - arm + inset, x + half - inset, y + arm - inset), corner, corner, Path.Direction.CW)
            p.op(across, Path.Op.UNION)  // one outline, no seams in the middle
        }
        val arms = listOf(
            Retro.UP to RectF(x - arm, y - half, x + arm, y - arm),
            Retro.DOWN to RectF(x - arm, y + arm, x + arm, y + half),
            Retro.LEFT to RectF(x - half, y - arm, x - arm, y + arm),
            Retro.RIGHT to RectF(x + arm, y - arm, x + half, y + arm),
        )
        when (style) {
            ButtonStyle.MODERN -> {
                val shape = Path()
                cross(shape, 0f)
                fill.shader = null
                fill.color = alpha(Color.BLACK, 0.35f * a)
                canvas.save(); canvas.translate(0f, 3f * density); canvas.drawPath(shape, fill); canvas.restore()
                fill.shader = LinearGradient(0f, y - half, 0f, y + half,
                    alpha(Color.rgb(74, 82, 102), a), alpha(Color.rgb(36, 40, 52), a), Shader.TileMode.CLAMP)
                canvas.drawPath(shape, fill)
                fill.shader = null
                for ((bit, r) in arms) if (mask and (1 shl bit) != 0) {
                    fill.color = alpha(accent, 0.9f * a)
                    canvas.drawRoundRect(r, corner, corner, fill)
                }
                // Centre dimple.
                fill.shader = RadialGradient(x, y, arm * 0.7f, alpha(Color.rgb(28, 31, 40), a),
                    alpha(Color.rgb(50, 56, 72), a), Shader.TileMode.CLAMP)
                canvas.drawCircle(x, y, arm * 0.62f, fill)
                fill.shader = null
                stroke.strokeWidth = 1.2f * density
                stroke.color = alpha(Color.WHITE, 0.18f * a)
                canvas.drawPath(shape, stroke)
                arrows(canvas, x, y, half, arm, mask, alpha(Color.WHITE, 0.8f * a), alpha(Color.WHITE, a))
            }
            ButtonStyle.NEON -> {
                val shape = Path()
                cross(shape, 0f)
                fill.shader = null
                fill.color = alpha(Color.rgb(12, 14, 22), 0.55f * a)
                canvas.drawPath(shape, fill)
                for ((bit, r) in arms) if (mask and (1 shl bit) != 0) {
                    fill.color = alpha(accent, 0.7f * a)
                    canvas.drawRoundRect(r, corner, corner, fill)
                }
                val glow = Color.rgb(0, 229, 255)
                stroke.strokeWidth = 2.5f * density
                stroke.color = alpha(glow, a)
                stroke.setShadowLayer(8f * density, 0f, 0f, alpha(glow, 0.9f * a))
                canvas.drawPath(shape, stroke)
                stroke.clearShadowLayer()
                arrows(canvas, x, y, half, arm, mask, alpha(lighter(glow, 0.4f), 0.8f * a), alpha(Color.WHITE, a))
            }
            ButtonStyle.CLASSIC -> {
                val o = (opacity * 255).toInt().coerceIn(20, 255) / 255f
                fill.shader = null
                fill.color = Color.argb((o * 0.35f * 255).toInt(), 255, 255, 255)
                canvas.drawRect(x - arm, y - arm, x + arm, y + arm, fill)
                for ((bit, r) in arms) {
                    fill.color = if (mask and (1 shl bit) != 0) Color.argb((o * 0.85f * 255).toInt(), 107, 78, 255)
                                 else Color.argb((o * 0.35f * 255).toInt(), 255, 255, 255)
                    canvas.drawRoundRect(r, corner, corner, fill)
                }
                stroke.strokeWidth = 2f * density
                stroke.color = Color.argb((o * 0.9f * 255).toInt(), 255, 255, 255)
                canvas.drawRoundRect(RectF(x - arm, y - half, x + arm, y + half), corner, corner, stroke)
                canvas.drawRoundRect(RectF(x - half, y - arm, x + half, y + arm), corner, corner, stroke)
            }
        }
    }

    private fun arrows(canvas: Canvas, x: Float, y: Float, half: Float, arm: Float, mask: Int, color: Int, pressedColor: Int) {
        val s = arm * 0.42f
        val d = half - arm * 0.75f
        fill.shader = null
        for ((bit, angle) in listOf(Retro.UP to 0f, Retro.RIGHT to 90f, Retro.DOWN to 180f, Retro.LEFT to 270f)) {
            fill.color = if (mask and (1 shl bit) != 0) pressedColor else color
            canvas.save()
            canvas.rotate(angle, x, y)
            path.reset()
            path.moveTo(x, y - d - s * 0.6f)
            path.lineTo(x + s, y - d + s * 0.5f)
            path.lineTo(x - s, y - d + s * 0.5f)
            path.close()
            canvas.drawPath(path, fill)
            canvas.restore()
        }
    }

    // --- Analog stick ---------------------------------------------------------------

    fun drawStick(canvas: Canvas, x: Float, y: Float, r: Float, kx: Float, ky: Float, pressed: Boolean, opacity: Float) {
        val a = level(opacity)
        fill.shader = null
        when (style) {
            ButtonStyle.CLASSIC -> {
                val o = (opacity * 255).toInt().coerceIn(20, 255) / 255f
                fill.color = Color.argb((o * 0.25f * 255).toInt(), 255, 255, 255)
                canvas.drawCircle(x, y, r, fill)
                stroke.strokeWidth = 2f * density
                stroke.color = Color.argb((o * 0.9f * 255).toInt(), 255, 255, 255)
                canvas.drawCircle(x, y, r, stroke)
                fill.color = if (pressed) Color.argb((o * 0.9f * 255).toInt(), 107, 78, 255)
                             else Color.argb((o * 0.6f * 255).toInt(), 255, 255, 255)
                canvas.drawCircle(x + kx * r * 0.55f, y + ky * r * 0.55f, r * 0.45f, fill)
            }
            ButtonStyle.NEON -> {
                fill.color = alpha(Color.rgb(12, 14, 22), 0.5f * a)
                canvas.drawCircle(x, y, r, fill)
                val glow = Color.rgb(0, 229, 255)
                stroke.strokeWidth = 2.5f * density
                stroke.color = alpha(glow, a)
                stroke.setShadowLayer(8f * density, 0f, 0f, alpha(glow, 0.9f * a))
                canvas.drawCircle(x, y, r - 1.5f * density, stroke)
                stroke.color = alpha(if (pressed) accent else glow, a)
                canvas.drawCircle(x + kx * r * 0.55f, y + ky * r * 0.55f, r * 0.42f, stroke)
                stroke.clearShadowLayer()
            }
            ButtonStyle.MODERN -> {
                fill.shader = RadialGradient(x, y, r, alpha(Color.rgb(30, 34, 44), a),
                    alpha(Color.rgb(58, 66, 84), a), Shader.TileMode.CLAMP)
                canvas.drawCircle(x, y, r, fill)
                fill.shader = null
                val kxp = x + kx * r * 0.55f
                val kyp = y + ky * r * 0.55f
                fill.color = alpha(Color.BLACK, 0.35f * a)
                canvas.drawCircle(kxp, kyp + 3f * density, r * 0.45f, fill)
                val top = if (pressed) accent else Color.rgb(96, 106, 130)
                fill.shader = RadialGradient(kxp - r * 0.12f, kyp - r * 0.15f, r * 0.5f,
                    alpha(lighter(top, 0.3f), a), alpha(darker(top, 0.25f), a), Shader.TileMode.CLAMP)
                canvas.drawCircle(kxp, kyp, r * 0.45f, fill)
                fill.shader = null
            }
        }
    }
}
