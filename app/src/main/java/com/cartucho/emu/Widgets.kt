package com.cartucho.emu

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.RectF
import android.graphics.Shader
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.RemoteViews
import androidx.core.content.ContextCompat
import java.util.concurrent.Executors

/**
 * Home-screen widgets:
 * - "Continuar jogando": the last game, with the picture of where you
 *   stopped, its console and play time; one tap goes back into it.
 * - "Jogos recentes": the last four games' covers.
 *
 * Refreshed when the library comes to the front (after playing), after a
 * scan, and every hour or so for the "há 2 h" texts.
 */
object Widgets {
    private val io = Executors.newSingleThreadExecutor()

    /** Rebuilds every widget of the app (any thread; the work runs in the background). */
    fun refresh(context: Context) {
        val app = context.applicationContext
        io.execute { runCatching { updateAll(app) } }
    }

    private fun updateAll(context: Context) {
        val mgr = AppWidgetManager.getInstance(context)
        val cont = mgr.getAppWidgetIds(ComponentName(context, ContinueWidget::class.java))
        val recent = mgr.getAppWidgetIds(ComponentName(context, RecentWidget::class.java))
        if (cont.isEmpty() && recent.isEmpty()) return
        val store = LibraryStore(context)
        val games = store.loadGames().filter { store.lastPlayed(it) > 0 }.sortedByDescending { store.lastPlayed(it) }
        val covers = Covers(context)
        for (id in cont) mgr.updateAppWidget(id, continueViews(context, mgr, id, games.firstOrNull(), store, covers))
        if (recent.isNotEmpty()) {
            val views = recentViews(context, games.take(4), covers)
            for (id in recent) mgr.updateAppWidget(id, views)
        }
    }

    private fun playIntent(context: Context, game: Game?, request: Int): PendingIntent {
        val intent = Intent(context, LibraryActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (game != null) intent.setAction(LibraryActivity.ACTION_PLAY).putExtra(LibraryActivity.EXTRA_GAME_URI, game.uri)
        return PendingIntent.getActivity(context, request, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
    }

    private fun continueViews(context: Context, mgr: AppWidgetManager, id: Int, game: Game?, store: LibraryStore,
                              covers: Covers): RemoteViews {
        val v = RemoteViews(context.packageName, R.layout.widget_continue)
        if (game == null) {
            v.setViewVisibility(R.id.widget_empty, View.VISIBLE)
            v.setViewVisibility(R.id.widget_play, View.GONE)
            v.setTextViewText(R.id.widget_title, "")
            v.setTextViewText(R.id.widget_info, "")
            v.setViewVisibility(R.id.widget_console, View.GONE)
            v.setOnClickPendingIntent(R.id.widget_root, playIntent(context, null, 0))
            return v
        }
        // The widget's size in pixels, to make the picture just big enough.
        val opts: Bundle = mgr.getAppWidgetOptions(id)
        val density = context.resources.displayMetrics.density
        val wDp = opts.getInt(AppWidgetManager.OPTION_APPWIDGET_MAX_WIDTH, 180).coerceIn(110, 420)
        val hDp = opts.getInt(AppWidgetManager.OPTION_APPWIDGET_MAX_HEIGHT, 180).coerceIn(80, 420)
        val w = (wDp * density).toInt().coerceAtMost(720)
        val h = (hDp * density * w / (wDp * density)).toInt().coerceAtMost(720)
        val color = ContextCompat.getColor(context, game.system.color)
        // Where you stopped first, then the cover, then the console's photo.
        val picture = PlayStats.thumb(context, game) ?: covers.bitmapBlocking(game)
            ?: BitmapFactory.decodeResource(context.resources, ConsoleUi.artFor(game.system.name))
        v.setImageViewBitmap(R.id.widget_image, card(picture, w, h, 22 * density, color))
        v.setViewVisibility(R.id.widget_empty, View.GONE)
        v.setViewVisibility(R.id.widget_play, View.VISIBLE)
        v.setViewVisibility(R.id.widget_console, View.VISIBLE)
        v.setTextViewText(R.id.widget_console, game.system.shortName)
        if (Build.VERSION.SDK_INT >= 31)
            v.setColorStateList(R.id.widget_console, "setBackgroundTintList", ColorStateList.valueOf(color))
        v.setTextViewText(R.id.widget_title, game.title)
        val time = PlayStats.playTime(context, game)
        v.setTextViewText(R.id.widget_info, listOfNotNull(
            PlayStats.formatAgo(store.lastPlayed(game)).replaceFirstChar { it.uppercase() },
            if (time > 0) PlayStats.formatTime(time) + " jogadas" else null,
        ).joinToString(" · "))
        v.setOnClickPendingIntent(R.id.widget_root, playIntent(context, game, 1))
        v.setOnClickPendingIntent(R.id.widget_play, playIntent(context, game, 2))
        return v
    }

    private fun recentViews(context: Context, games: List<Game>, covers: Covers): RemoteViews {
        val v = RemoteViews(context.packageName, R.layout.widget_recent)
        val density = context.resources.displayMetrics.density
        val images = intArrayOf(R.id.recent_image_0, R.id.recent_image_1, R.id.recent_image_2, R.id.recent_image_3)
        val titles = intArrayOf(R.id.recent_title_0, R.id.recent_title_1, R.id.recent_title_2, R.id.recent_title_3)
        val slots = intArrayOf(R.id.recent_0, R.id.recent_1, R.id.recent_2, R.id.recent_3)
        for (i in 0 until 4) {
            val g = games.getOrNull(i)
            if (g == null) {
                v.setViewVisibility(slots[i], if (i == 0) View.VISIBLE else View.INVISIBLE)
                if (i == 0) {
                    v.setTextViewText(titles[i], "Nenhum jogo ainda")
                    v.setImageViewResource(images[i], R.drawable.art_recent)
                    v.setOnClickPendingIntent(slots[i], playIntent(context, null, 10))
                }
                continue
            }
            v.setViewVisibility(slots[i], View.VISIBLE)
            val pic = covers.bitmapBlocking(g) ?: PlayStats.thumb(context, g)
                ?: BitmapFactory.decodeResource(context.resources, ConsoleUi.artFor(g.system.name))
            val scale = 260f / maxOf(pic.width, pic.height)
            val small = Bitmap.createScaledBitmap(pic, (pic.width * scale).toInt().coerceAtLeast(1),
                (pic.height * scale).toInt().coerceAtLeast(1), true)
            v.setImageViewBitmap(images[i], roundCorners(small, 10 * density * scale.coerceAtMost(1f) + 6))
            v.setTextViewText(titles[i], g.title)
            v.setOnClickPendingIntent(slots[i], playIntent(context, g, 20 + i))
        }
        return v
    }

    /** The picture filling a rounded card, darker towards the bottom for the text. */
    private fun card(src: Bitmap, w: Int, h: Int, radius: Float, tint: Int): Bitmap {
        val out = Bitmap.createBitmap(w.coerceAtLeast(1), h.coerceAtLeast(1), Bitmap.Config.ARGB_8888)
        val c = Canvas(out)
        val p = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
        c.drawRoundRect(RectF(0f, 0f, w.toFloat(), h.toFloat()), radius, radius, p)
        p.xfermode = PorterDuffXfermode(PorterDuff.Mode.SRC_IN)
        // Centre-crop.
        val s = maxOf(w.toFloat() / src.width, h.toFloat() / src.height)
        val dw = src.width * s
        val dh = src.height * s
        c.drawBitmap(src, null, RectF((w - dw) / 2, (h - dh) / 2, (w + dw) / 2, (h + dh) / 2), p)
        p.xfermode = PorterDuffXfermode(PorterDuff.Mode.SRC_ATOP)
        p.shader = LinearGradient(0f, h * 0.35f, 0f, h.toFloat(),
            Color.TRANSPARENT, Color.argb(215, Color.red(tint) / 5, Color.green(tint) / 5, Color.blue(tint) / 5), Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, w.toFloat(), h.toFloat(), p)
        return out
    }

    private fun roundCorners(src: Bitmap, radius: Float): Bitmap {
        val out = Bitmap.createBitmap(src.width, src.height, Bitmap.Config.ARGB_8888)
        val c = Canvas(out)
        val p = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
        c.drawRoundRect(RectF(0f, 0f, src.width.toFloat(), src.height.toFloat()), radius, radius, p)
        p.xfermode = PorterDuffXfermode(PorterDuff.Mode.SRC_IN)
        c.drawBitmap(src, 0f, 0f, p)
        return out
    }

    class ContinueWidget : AppWidgetProvider() {
        override fun onUpdate(context: Context, manager: AppWidgetManager, ids: IntArray) = refresh(context)
        override fun onAppWidgetOptionsChanged(context: Context, manager: AppWidgetManager, id: Int, newOptions: Bundle) =
            refresh(context)
    }

    class RecentWidget : AppWidgetProvider() {
        override fun onUpdate(context: Context, manager: AppWidgetManager, ids: IntArray) = refresh(context)
    }
}
