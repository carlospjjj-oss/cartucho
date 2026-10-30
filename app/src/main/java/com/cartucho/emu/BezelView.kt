package com.cartucho.emu

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BlurMaskFilter
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.view.View
import androidx.core.content.ContextCompat
import androidx.core.graphics.ColorUtils

/**
 * A decorative frame around the game picture, in the colours of its console,
 * filling the empty bars at the sides (landscape) or above and below. The
 * styles are drawn here, not pictures: a handheld's plastic body with its
 * screen bezel, or a tube TV for the home consoles.
 */
@SuppressLint("ViewConstructor")
class BezelView(context: Context, private val system: GameSystem) : View(context) {
    enum class Style { OFF, AUTO, HANDHELD, TV, ARCADE }

    var style = Style.AUTO
        set(v) { field = v; rebuild() }
    /** The photo of the console in the side area; off while touch controls sit there. */
    var showArt = true
        set(v) { if (field != v) { field = v; rebuild() } }

    private var game = Rect()
    private var cache: Bitmap? = null
    private val density = resources.displayMetrics.density
    private val color = ContextCompat.getColor(context, system.color)
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

    init {
        setWillNotDraw(false)
        isClickable = false
        isFocusable = false
    }

    private val handheld get() = system in setOf(GameSystem.GB, GameSystem.GBA, GameSystem.GG)
    private val resolved: Style get() = when (style) {
        Style.AUTO -> if (handheld) Style.HANDHELD else Style.TV
        else -> style
    }

    /** Where the game picture is (view pixels); the frame goes around it. */
    fun setGameRect(r: Rect) {
        if (r == game || r.isEmpty) return
        game = Rect(r)
        rebuild()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) = rebuild()

    private fun rebuild() {
        cache?.recycle()
        cache = null
        invalidate()
    }

    /** Only when there is room: bars of at least 6% of the screen around the picture. */
    private fun worthDrawing(): Boolean {
        if (style == Style.OFF || game.isEmpty || width == 0 || height == 0) return false
        if (system == GameSystem.NDS || system == GameSystem.N3DS) return false
        val side = minOf(game.left, width - game.right)
        val tops = minOf(game.top, height - game.bottom)
        return side > width * 0.06f || tops > height * 0.06f
    }

    override fun onDraw(canvas: Canvas) {
        if (!worthDrawing()) {
            if (cache == null) android.util.Log.d("Cartucho", "bezel: nothing to draw, ${width}x$height game=$game style=$style")
            return
        }
        if (cache == null) android.util.Log.d("Cartucho", "bezel: drawing ${resolved} around $game in ${width}x$height")
        val bmp = cache ?: render().also { cache = it }
        canvas.drawBitmap(bmp, 0f, 0f, null)
    }

    private fun render(): Bitmap {
        val bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        when (resolved) {
            Style.HANDHELD -> drawHandheld(c)
            Style.ARCADE -> drawArcade(c)
            else -> drawTv(c)
        }
        // The picture itself stays clear.
        paint.reset()
        paint.xfermode = android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.CLEAR)
        c.drawRect(game, paint)
        paint.xfermode = null
        return bmp
    }

    private fun dp(v: Float) = v * density

    // --- Backdrop shared by the styles ----------------------------------------

    private fun backdrop(c: Canvas, base: Int) {
        val w = width.toFloat()
        val h = height.toFloat()
        paint.reset()
        paint.shader = RadialGradient(w / 2, h / 2, maxOf(w, h) * 0.7f,
            ColorUtils.blendARGB(base, Color.BLACK, 0.35f), ColorUtils.blendARGB(base, Color.BLACK, 0.85f), Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, w, h, paint)
        // Fine diagonal lines for some texture.
        paint.reset()
        paint.color = Color.argb(14, 255, 255, 255)
        paint.strokeWidth = dp(1f)
        var x = -h
        while (x < w) {
            c.drawLine(x, h, x + h, 0f, paint)
            x += dp(7f)
        }
        if (showArt) sideArt(c)
    }

    /** The console's photo, faded, in the wider side bar. */
    private fun sideArt(c: Canvas) {
        val res = ConsoleUi.artFor(system.name)
        val photo = BitmapFactory.decodeResource(resources, res) ?: return
        val left = game.left.toFloat()
        val right = (width - game.right).toFloat()
        val area = if (left >= right) RectF(0f, 0f, left, height.toFloat()) else RectF(game.right.toFloat(), 0f, width.toFloat(), height.toFloat())
        if (area.width() < dp(90f)) { photo.recycle(); return }
        val maxW = area.width() * 0.8f
        val scale = minOf(maxW / photo.width, height * 0.45f / photo.height)
        val pw = photo.width * scale
        val ph = photo.height * scale
        val dst = RectF(area.centerX() - pw / 2, height * 0.5f - ph / 2, area.centerX() + pw / 2, height * 0.5f + ph / 2)
        paint.reset()
        paint.isFilterBitmap = true
        paint.alpha = 110
        c.drawBitmap(photo, null, dst, paint)
        photo.recycle()
    }

    // --- Handheld: the plastic body around a recessed screen -------------------

    private fun drawHandheld(c: Canvas) {
        val body = when (system) {
            GameSystem.GB -> Color.rgb(196, 192, 204)
            GameSystem.GBA -> Color.rgb(98, 72, 196)
            else -> Color.rgb(40, 40, 46)
        }
        backdrop(c, color)
        val pad = minOf(game.height(), game.width()) * 0.09f
        val glass = RectF(game.left - pad, game.top - pad * 0.8f, game.right + pad, game.bottom + pad * 1.6f)
        val shell = RectF(glass.left - pad * 0.9f, glass.top - pad * 0.9f, glass.right + pad * 0.9f, glass.bottom + pad * 0.9f)
        // Body, with a shadow under it and a soft light from above.
        paint.reset()
        paint.color = Color.argb(150, 0, 0, 0)
        paint.maskFilter = BlurMaskFilter(dp(18f), BlurMaskFilter.Blur.NORMAL)
        c.drawRoundRect(RectF(shell).apply { offset(0f, dp(8f)) }, pad * 1.4f, pad * 1.4f, paint)
        paint.reset()
        paint.shader = LinearGradient(0f, shell.top, 0f, shell.bottom,
            ColorUtils.blendARGB(body, Color.WHITE, 0.18f), ColorUtils.blendARGB(body, Color.BLACK, 0.22f), Shader.TileMode.CLAMP)
        c.drawRoundRect(shell, pad * 1.4f, pad * 1.4f, paint)
        // Edge highlight.
        paint.reset()
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2f)
        paint.color = Color.argb(80, 255, 255, 255)
        c.drawRoundRect(RectF(shell).apply { inset(dp(1.5f), dp(1.5f)) }, pad * 1.35f, pad * 1.35f, paint)
        // Dark glass around the screen with its lines and the power light.
        paint.reset()
        val glassColor = if (system == GameSystem.GB) Color.rgb(84, 80, 102) else Color.rgb(18, 18, 24)
        paint.color = glassColor
        c.drawRoundRect(glass, pad * 0.6f, pad * 0.6f, paint)
        if (system == GameSystem.GB) {
            paint.color = Color.rgb(140, 32, 92)
            c.drawRect(glass.left + pad * 0.4f, glass.top + pad * 0.35f, glass.right - pad * 0.4f, glass.top + pad * 0.42f, paint)
            paint.color = Color.rgb(36, 48, 120)
            c.drawRect(glass.left + pad * 0.4f, glass.top + pad * 0.5f, glass.right - pad * 0.4f, glass.top + pad * 0.57f, paint)
        }
        paint.color = Color.rgb(255, 48, 64)
        paint.maskFilter = BlurMaskFilter(dp(4f), BlurMaskFilter.Blur.SOLID)
        c.drawCircle(glass.left + pad * 0.5f, game.centerY().toFloat() - pad, pad * 0.16f, paint)
        paint.maskFilter = null
        // Name under the screen, in the console's style but not its logo.
        paint.color = Color.argb(170, 255, 255, 255)
        paint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD_ITALIC)
        paint.textSize = pad * 0.55f
        paint.textAlign = Paint.Align.CENTER
        c.drawText(system.fullName.uppercase(), glass.centerX(), glass.bottom - pad * 0.45f, paint)
        // Inner shadow on the screen edge.
        paint.reset()
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(3f)
        paint.color = Color.argb(160, 0, 0, 0)
        c.drawRect(RectF(game).apply { inset(-dp(1.5f), -dp(1.5f)) }, paint)
    }

    // --- Home consoles: a tube TV ---------------------------------------------

    private fun drawTv(c: Canvas) {
        backdrop(c, color)
        val pad = minOf(game.height(), game.width()) * 0.07f
        val tube = RectF(game).apply { inset(-pad * 0.35f, -pad * 0.35f) }
        val cabinet = RectF(tube.left - pad * 1.3f, tube.top - pad * 1.1f, tube.right + pad * 1.3f, tube.bottom + pad * 2.1f)
        paint.reset()
        paint.color = Color.argb(160, 0, 0, 0)
        paint.maskFilter = BlurMaskFilter(dp(22f), BlurMaskFilter.Blur.NORMAL)
        c.drawRoundRect(RectF(cabinet).apply { offset(0f, dp(10f)) }, pad, pad, paint)
        // Cabinet: dark plastic with a hint of the console colour.
        paint.reset()
        val plastic = ColorUtils.blendARGB(Color.rgb(34, 32, 38), color, 0.18f)
        paint.shader = LinearGradient(0f, cabinet.top, 0f, cabinet.bottom,
            ColorUtils.blendARGB(plastic, Color.WHITE, 0.12f), ColorUtils.blendARGB(plastic, Color.BLACK, 0.4f), Shader.TileMode.CLAMP)
        c.drawRoundRect(cabinet, pad, pad, paint)
        paint.reset()
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2f)
        paint.color = Color.argb(60, 255, 255, 255)
        c.drawRoundRect(RectF(cabinet).apply { inset(dp(1.5f), dp(1.5f)) }, pad * 0.95f, pad * 0.95f, paint)
        // The tube: a bulging dark glass slightly larger than the picture.
        paint.reset()
        paint.color = Color.rgb(8, 8, 10)
        val path = Path().apply { addRoundRect(tube, pad * 0.9f, pad * 0.9f, Path.Direction.CW) }
        c.drawPath(path, paint)
        // Accent line in the console colour around the glass.
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2.5f)
        paint.color = ColorUtils.setAlphaComponent(ColorUtils.blendARGB(color, Color.WHITE, 0.3f), 200)
        c.drawRoundRect(RectF(tube).apply { inset(-dp(4f), -dp(4f)) }, pad, pad, paint)
        // Speaker grille and knobs on the chin.
        paint.reset()
        paint.color = Color.argb(90, 0, 0, 0)
        val chinY = tube.bottom + pad * 1.05f
        var x = cabinet.left + pad * 1.4f
        while (x < cabinet.centerX() - pad) {
            c.drawRoundRect(x, chinY - pad * 0.35f, x + dp(3f), chinY + pad * 0.35f, dp(1.5f), dp(1.5f), paint)
            x += dp(7f)
        }
        for (i in 0 until 2) {
            val kx = cabinet.right - pad * (1.6f + i * 1.1f)
            paint.color = Color.rgb(20, 20, 22)
            c.drawCircle(kx, chinY, pad * 0.36f, paint)
            paint.color = Color.argb(90, 255, 255, 255)
            c.drawCircle(kx - pad * 0.08f, chinY - pad * 0.08f, pad * 0.12f, paint)
        }
        paint.color = ColorUtils.blendARGB(color, Color.WHITE, 0.35f)
        paint.maskFilter = BlurMaskFilter(dp(4f), BlurMaskFilter.Blur.SOLID)
        c.drawCircle(cabinet.centerX() + pad * 0.6f, chinY, pad * 0.12f, paint)
        paint.maskFilter = null
        paint.color = Color.argb(150, 255, 255, 255)
        paint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
        paint.textSize = pad * 0.45f
        paint.letterSpacing = 0.3f
        paint.textAlign = Paint.Align.CENTER
        c.drawText("CARTUCHO", cabinet.centerX() + pad * 2.6f, chinY + pad * 0.16f, paint)
    }

    // --- Arcade: a cabinet with lit side art -----------------------------------

    private fun drawArcade(c: Canvas) {
        backdrop(c, color)
        val h = height.toFloat()
        val pad = minOf(game.height(), game.width()) * 0.06f
        val frame = RectF(game).apply { inset(-pad, -pad) }
        // Neon stripes running up the sides.
        paint.reset()
        val colors = intArrayOf(Color.rgb(255, 64, 129), Color.rgb(255, 196, 0), Color.rgb(0, 229, 255), color)
        for ((i, col) in colors.withIndex()) {
            paint.color = col
            paint.alpha = 210
            paint.maskFilter = BlurMaskFilter(dp(6f), BlurMaskFilter.Blur.SOLID)
            val o = pad * (0.6f + i * 0.55f)
            paint.strokeWidth = pad * 0.22f
            c.drawLine(frame.left - o, 0f, frame.left - o, h, paint)
            c.drawLine(frame.right + o, 0f, frame.right + o, h, paint)
        }
        paint.maskFilter = null
        paint.shader = LinearGradient(0f, frame.top, 0f, frame.bottom, Color.rgb(30, 30, 36), Color.rgb(10, 10, 12), Shader.TileMode.CLAMP)
        c.drawRoundRect(frame, pad * 0.5f, pad * 0.5f, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(3f)
        paint.color = ColorUtils.blendARGB(color, Color.WHITE, 0.4f)
        paint.maskFilter = BlurMaskFilter(dp(5f), BlurMaskFilter.Blur.SOLID)
        c.drawRoundRect(frame, pad * 0.5f, pad * 0.5f, paint)
        paint.maskFilter = null
    }
}
