package com.cartucho.emu

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.service.wallpaper.WallpaperService
import android.view.MotionEvent
import android.view.SurfaceHolder
import androidx.core.content.ContextCompat
import androidx.core.graphics.ColorUtils
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin
import kotlin.random.Random

/**
 * Live wallpaper: the console look's drifting glow in the colours of a
 * console (by default the one of the last game played), pixels floating up
 * like sparks, and the console's photo, faded, low on the screen. Touching
 * the screen throws a burst of pixels. Draws only while visible, about 30
 * times a second.
 */
class CartuchoWallpaper : WallpaperService() {
    override fun onCreateEngine(): Engine = PixelEngine()

    private inner class PixelEngine : Engine() {
        private val handler = Handler(Looper.getMainLooper())
        private val backdrop = Backdrop()
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
        private val density = resources.displayMetrics.density
        private var visible = false
        private var width = 0
        private var height = 0
        private var shift = 0.5f  // middle page until the launcher says otherwise
        private val start = SystemClock.elapsedRealtime()

        private var system: GameSystem? = null
        private var tint = Color.rgb(107, 78, 255)
        private var photo: Bitmap? = null
        private var showPhoto = true
        private var amount = 1f

        private inner class Pixel(var x: Float, var y: Float, var vx: Float, var vy: Float,
                                  val size: Float, val color: Int, var life: Float, val burst: Boolean)
        private val pixels = ArrayList<Pixel>()
        private val rnd = Random(SystemClock.uptimeMillis())
        private var last = 0L

        private val tick = Runnable { draw() }

        override fun onCreate(surfaceHolder: SurfaceHolder) {
            super.onCreate(surfaceHolder)
            setTouchEventsEnabled(true)
        }

        override fun onVisibilityChanged(visible: Boolean) {
            this.visible = visible
            if (visible) {
                loadTheme()
                last = 0L
                draw()
            } else handler.removeCallbacks(tick)
        }

        override fun onSurfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
            super.onSurfaceChanged(holder, format, width, height)
            this.width = width
            this.height = height
            pixels.clear()
            draw()
        }

        override fun onSurfaceDestroyed(holder: SurfaceHolder) {
            visible = false
            handler.removeCallbacks(tick)
            super.onSurfaceDestroyed(holder)
        }

        override fun onDestroy() {
            handler.removeCallbacks(tick)
            photo?.recycle()
            super.onDestroy()
        }

        override fun onOffsetsChanged(xOffset: Float, yOffset: Float, xStep: Float, yStep: Float, xPixels: Int, yPixels: Int) {
            shift = xOffset
        }

        override fun onTouchEvent(event: MotionEvent) {
            if (event.actionMasked != MotionEvent.ACTION_DOWN) return
            // A burst of pixels where the screen was touched.
            repeat(18) {
                val a = rnd.nextFloat() * 2 * PI.toFloat()
                val speed = (60 + rnd.nextFloat() * 220) * density
                pixels += Pixel(event.x, event.y, cos(a) * speed, sin(a) * speed,
                    (3 + rnd.nextInt(5)) * density, pixelColor(), 1f, burst = true)
            }
        }

        /** The console to show: the one picked in the settings, or the last game's. */
        private fun loadTheme() {
            val prefs = Prefs.raw(this@CartuchoWallpaper)
            val pick = prefs.getString("wallpaper_theme", "last") ?: "last"
            showPhoto = prefs.getBoolean("wallpaper_photo", true)
            amount = when (prefs.getString("wallpaper_pixels", "normal")) { "few" -> 0.5f; "many" -> 1.8f; else -> 1f }
            val s = when (pick) {
                "last" -> runCatching {
                    val store = LibraryStore(this@CartuchoWallpaper)
                    store.loadGames().maxByOrNull { store.lastPlayed(it) }?.takeIf { store.lastPlayed(it) > 0 }?.system
                }.getOrNull()
                "cycle" -> GameSystem.entries[((System.currentTimeMillis() / (30 * 60 * 1000L)) % GameSystem.entries.size).toInt()]
                else -> GameSystem.entries.firstOrNull { it.name == pick }
            }
            if (s != system || (photo == null && s != null && showPhoto)) {
                system = s
                tint = s?.let { ContextCompat.getColor(this@CartuchoWallpaper, it.color) } ?: Color.rgb(107, 78, 255)
                photo?.recycle()
                photo = s?.let { BitmapFactory.decodeResource(resources, ConsoleUi.artFor(it.name)) }
            }
        }

        private fun pixelColor(): Int = when (rnd.nextInt(4)) {
            0 -> Color.WHITE
            1 -> ColorUtils.blendARGB(tint, Color.WHITE, 0.45f)
            2 -> Color.rgb(255, 181, 71)
            else -> tint
        }

        private fun step(dt: Float) {
            // Keep a gentle stream of sparks rising from the bottom.
            val target = (28 * amount * (width * height) / (1080f * 2400f)).toInt().coerceIn(8, 80)
            val floating = pixels.count { !it.burst }
            if (floating < target && rnd.nextFloat() < 0.35f) {
                pixels += Pixel(rnd.nextFloat() * width, height + 10f, (rnd.nextFloat() - 0.5f) * 14 * density,
                    -(18 + rnd.nextFloat() * 40) * density, (2 + rnd.nextInt(5)) * density, pixelColor(),
                    0.6f + rnd.nextFloat() * 0.4f, burst = false)
            }
            val it = pixels.iterator()
            while (it.hasNext()) {
                val p = it.next()
                p.x += p.vx * dt
                p.y += p.vy * dt
                if (p.burst) {
                    p.vx *= 0.94f
                    p.vy = p.vy * 0.94f + 30 * density * dt
                    p.life -= dt * 0.9f
                } else if (p.y < height * 0.15f) {
                    p.life -= dt * 0.6f
                }
                if (p.life <= 0f || p.y < -20f) it.remove()
            }
        }

        private fun draw() {
            if (!visible || width == 0) return
            val now = SystemClock.elapsedRealtime()
            val dt = if (last == 0L) 0f else ((now - last) / 1000f).coerceAtMost(0.1f)
            last = now
            step(dt)
            val holder = surfaceHolder
            val canvas: Canvas? = try {
                if (Build.VERSION.SDK_INT >= 26) holder.lockHardwareCanvas() else holder.lockCanvas()
            } catch (e: Exception) {
                null
            }
            if (canvas != null) {
                try {
                    val t = (now - start) / 1000f
                    val w = width.toFloat()
                    val h = height.toFloat()
                    backdrop.draw(canvas, w, h, tint, t, shift)
                    photo?.takeIf { showPhoto && !it.isRecycled }?.let { bmp ->
                        // Low on the screen, faded, moving a little with the pages.
                        val maxW = w * 0.72f
                        val s = minOf(maxW / bmp.width, h * 0.28f / bmp.height)
                        val pw = bmp.width * s
                        val ph = bmp.height * s
                        val cx = w / 2 - (shift - 0.5f) * w * 0.12f
                        val cy = h * 0.70f + sin(t * 0.4f) * 6 * density
                        paint.alpha = 92
                        canvas.drawBitmap(bmp, null, RectF(cx - pw / 2, cy - ph / 2, cx + pw / 2, cy + ph / 2), paint)
                        paint.alpha = 255
                    }
                    for (p in pixels) {
                        val twinkle = 0.75f + 0.25f * sin(t * 3f + p.x * 0.05f)
                        paint.color = ColorUtils.setAlphaComponent(p.color, (255 * p.life.coerceIn(0f, 1f) * twinkle).toInt())
                        canvas.drawRect(p.x, p.y, p.x + p.size, p.y + p.size, paint)
                    }
                } finally {
                    runCatching { holder.unlockCanvasAndPost(canvas) }
                }
            }
            handler.removeCallbacks(tick)
            if (visible) handler.postDelayed(tick, 33)
        }
    }
}
