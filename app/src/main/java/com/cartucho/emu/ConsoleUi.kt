package com.cartucho.emu

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.graphics.Typeface
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.GradientDrawable
import android.media.MediaPlayer
import android.os.BatteryManager
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextClock
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.core.graphics.ColorUtils
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.LinearSnapHelper
import androidx.recyclerview.widget.RecyclerView
import java.io.File
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.min

/**
 * The library as a game console shows it (like Batocera / EmulationStation):
 * a carousel of consoles, then a list of games with a big panel for the one
 * selected (cover or moving preview, facts, saves). Made for a controller or
 * TV remote, works by touch too: the hints at the bottom are buttons.
 */
@SuppressLint("SetTextI18n", "ViewConstructor")
class ConsoleUi(private val context: Context, private val host: Host) {

    interface Host {
        val covers: Covers
        val info: GameInfo
        val store: LibraryStore
        fun games(): List<Game>
        fun facts(game: Game): GameInfo.Facts?
        fun playTime(game: Game): Long
        fun launch(game: Game)
        fun gameMenu(game: Game)
        fun mainMenu()
        fun search(onQuery: (String) -> Unit)
    }

    /** One card of the carousel: a console, or a collection (recent, favourites, search). */
    private data class Collection(
        val key: String,
        val label: String,      // big text on the card
        val title: String,
        val color: Int,
        val games: List<Game>,
        val subtitle: String,
    ) {
        /** The card's picture: a photo of the console (tools/art/photos.py) or a drawing for the collections. */
        val art get() = artFor(key)
    }

    private val density = context.resources.displayMetrics.density
    private fun dp(v: Float) = (v * density).toInt()
    private fun dp(v: Int) = dp(v.toFloat())
    private val main = Handler(Looper.getMainLooper())
    private val io = Executors.newSingleThreadExecutor()
    private val prefs = Prefs(context)
    val sounds = FrontendSounds(context)
    private val landscape get() = context.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    private val accent = ContextCompat.getColor(context, R.color.accent)
    private val textMain = ContextCompat.getColor(context, R.color.on_surface)
    private val textDim = ContextCompat.getColor(context, R.color.on_surface_dim)

    val root = FrameLayout(context)
    private val background = AnimatedBackground(context)
    private val home = FrameLayout(context)
    private val list = FrameLayout(context)
    private val hints = LinearLayout(context)

    private var collections: List<Collection> = emptyList()
    private var centered = 0
    private var inList = false
    private var current: Collection? = null
    private var tab = 0
    private var tabs: List<Pair<String, (Game) -> Boolean>> = emptyList()
    private var shown: List<Game> = emptyList()
    private var selected = 0


    // --- Data ------------------------------------------------------------------------

    /** The library changed (scan, play times, favourites): rebuild what is shown. */
    fun refresh() {
        val games = host.games()
        val out = mutableListOf<Collection>()
        val recent = games.filter { host.store.lastPlayed(it) > 0 }.sortedByDescending { host.store.lastPlayed(it) }.take(30)
        if (recent.isNotEmpty()) out += Collection("recent", "▶", "Continuar jogando", Color.rgb(255, 140, 66), recent,
            "Último: ${recent[0].title}, ${PlayStats.formatAgo(host.store.lastPlayed(recent[0]))}")
        val favs = games.filter { host.store.isFavorite(it) }
        if (favs.isNotEmpty()) out += Collection("fav", "★", "Favoritos", Color.rgb(255, 181, 71), favs, "${favs.size} jogos")
        for (s in GameSystem.entries) {
            val g = games.filter { it.system == s }
            if (g.isEmpty()) continue
            val time = g.sumOf { host.playTime(it) }
            val last = g.maxByOrNull { host.store.lastPlayed(it) }?.takeIf { host.store.lastPlayed(it) > 0 }
            out += Collection(s.name, s.shortName, s.fullName, ContextCompat.getColor(context, s.color), g,
                listOfNotNull("${g.size} jogos", if (time > 0) "${PlayStats.formatTime(time)} jogadas" else null,
                    last?.let { "Último: ${it.title}" }).joinToString(" · "))
        }
        if (games.map { it.system }.toSet().size > 1)
            out += Collection("all", "∞", "Todos os jogos", Color.rgb(124, 92, 255), games, "${games.size} jogos")
        val keepKey = collections.getOrNull(centered)?.key
        collections = out
        carouselAdapter.notifyDataSetChanged()
        if (collections.isNotEmpty()) {
            val at = collections.indexOfFirst { it.key == keepKey }.takeIf { it >= 0 } ?: 0
            carousel.scrollToPosition(at)
            setCentered(at)
        }
        emptyHint.visibility = if (collections.isEmpty()) View.VISIBLE else View.GONE
        if (inList) current?.let { c -> collections.firstOrNull { it.key == c.key }?.let { openList(it, keepSelection = true) } }
        else showHomeHints()
    }

    // --- Home: the console carousel ------------------------------------------------------

    private val carousel = RecyclerView(context)
    private val homeTitle = TextView(context)
    private val homeSubtitle = TextView(context)
    private val dots = LinearLayout(context)
    private val emptyHint = TextView(context)
    private var cardWidth = 0

    private inner class CardHolder(val card: FrameLayout, val glow: View, val art: ImageView, val label: TextView,
                                   val name: TextView, val count: TextView) :
        RecyclerView.ViewHolder(card)

    private val carouselAdapter = object : RecyclerView.Adapter<CardHolder>() {
        override fun getItemCount() = collections.size
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): CardHolder {
            val card = FrameLayout(context).apply {
                elevation = dp(10).toFloat()
                isFocusable = false
            }
            val label = TextView(context).apply {
                setTextColor(Color.WHITE)
                typeface = Typeface.DEFAULT_BOLD
                gravity = Gravity.CENTER
                letterSpacing = 0.06f
            }
            val name = TextView(context).apply {
                setTextColor(Color.WHITE)
                typeface = Typeface.DEFAULT_BOLD
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            }
            val count = TextView(context).apply {
                setTextColor(Color.argb(220, 255, 255, 255))
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            }
            // A soft light behind the drawing, then the drawing itself.
            val glow = View(context)
            card.addView(glow, FrameLayout.LayoutParams(0, 0, Gravity.CENTER_HORIZONTAL or Gravity.TOP))
            val art = ImageView(context).apply { scaleType = ImageView.ScaleType.FIT_CENTER }
            card.addView(art, FrameLayout.LayoutParams(0, 0, Gravity.CENTER_HORIZONTAL or Gravity.TOP))
            card.addView(label, FrameLayout.LayoutParams(MATCH, MATCH).apply { bottomMargin = dp(40) })
            val bottom = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(16), 0, dp(16), dp(14))
                addView(name)
                addView(count)
            }
            card.addView(bottom, FrameLayout.LayoutParams(MATCH, WRAP, Gravity.BOTTOM))
            return CardHolder(card, glow, art, label, name, count)
        }

        override fun onBindViewHolder(h: CardHolder, position: Int) {
            val c = collections[position]
            h.card.layoutParams = RecyclerView.LayoutParams(cardWidth, (cardWidth * 1.15f).toInt()).apply {
                leftMargin = dp(10); rightMargin = dp(10)
            }
            h.card.background = GradientDrawable(GradientDrawable.Orientation.TL_BR,
                intArrayOf(ColorUtils.blendARGB(c.color, Color.WHITE, 0.12f), ColorUtils.blendARGB(c.color, Color.BLACK, 0.55f))).apply {
                cornerRadius = dp(22).toFloat()
            }
            val cardHeight = (cardWidth * 1.15f).toInt()
            val artSize = (cardWidth * 0.78f).toInt()
            val artTop = ((cardHeight - dp(52) - artSize) / 2).coerceAtLeast(dp(6))
            (h.art.layoutParams as FrameLayout.LayoutParams).apply { width = artSize; height = artSize; topMargin = artTop }
            (h.glow.layoutParams as FrameLayout.LayoutParams).apply { width = artSize; height = artSize; topMargin = artTop }
            h.glow.background = GradientDrawable().apply {
                gradientType = GradientDrawable.RADIAL_GRADIENT
                gradientRadius = artSize / 2f
                colors = intArrayOf(ColorUtils.blendARGB(c.color, Color.WHITE, 0.55f).let { ColorUtils.setAlphaComponent(it, 150) },
                    Color.TRANSPARENT)
            }
            h.art.setImageResource(c.art)
            h.label.text = ""
            h.name.text = c.title
            h.count.text = "${c.games.size} jogos"
            h.card.setOnClickListener {
                val pos = h.bindingAdapterPosition
                if (pos == centered) open() else {
                    sounds.play(FrontendSounds.Sfx.MOVE)
                    carousel.smoothScrollToPosition(pos)
                }
            }
        }
    }

    private fun buildHome() {
        val w = context.resources.displayMetrics.widthPixels
        cardWidth = if (landscape) (w * 0.2f).toInt().coerceAtMost(dp(260)) else (w * 0.46f).toInt()
        home.addView(TextView(context).apply {
            text = "Cartucho"
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
            setCompoundDrawablesRelativeWithIntrinsicBounds(0, 0, 0, 0)
        }, FrameLayout.LayoutParams(WRAP, WRAP).apply { leftMargin = dp(22); topMargin = dp(14) })

        carousel.layoutManager = LinearLayoutManager(context, LinearLayoutManager.HORIZONTAL, false)
        carousel.adapter = carouselAdapter
        carousel.clipToPadding = false
        carousel.overScrollMode = View.OVER_SCROLL_NEVER
        carousel.isFocusable = false
        LinearSnapHelper().attachToRecyclerView(carousel)
        carousel.addOnScrollListener(object : RecyclerView.OnScrollListener() {
            override fun onScrolled(rv: RecyclerView, dx: Int, dy: Int) = styleCards()
            override fun onScrollStateChanged(rv: RecyclerView, state: Int) {
                if (state == RecyclerView.SCROLL_STATE_IDLE) {
                    val c = closestToCenter()
                    if (c >= 0 && c != centered) {
                        setCentered(c)
                        sounds.play(FrontendSounds.Sfx.MOVE)
                    }
                }
            }
        })
        carousel.addOnLayoutChangeListener { v, _, _, _, _, _, _, _, _ ->
            val side = ((v.width - cardWidth) / 2 - dp(10)).coerceAtLeast(0)
            if (carousel.paddingLeft != side) {
                carousel.setPadding(side, 0, side, 0)
                carousel.post { carousel.scrollToPosition(centered); styleCards() }
            } else styleCards()
        }
        val cardHeight = (cardWidth * 1.15f).toInt()
        home.addView(carousel, FrameLayout.LayoutParams(MATCH, cardHeight + dp(40), Gravity.CENTER_VERTICAL).apply {
            bottomMargin = if (landscape) dp(70) else dp(110)
        })
        homeTitle.apply {
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 26f)
            gravity = Gravity.CENTER
        }
        homeSubtitle.apply {
            setTextColor(Color.argb(210, 230, 226, 250))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            gravity = Gravity.CENTER
            setPadding(dp(24), dp(4), dp(24), 0)
            maxLines = 2
        }
        dots.apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        val below = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            addView(homeTitle)
            addView(homeSubtitle)
            addView(dots, LinearLayout.LayoutParams(WRAP, dp(24)).apply { topMargin = dp(10) })
        }
        home.addView(below, FrameLayout.LayoutParams(MATCH, WRAP, Gravity.BOTTOM).apply {
            bottomMargin = if (landscape) dp(70) else dp(120)
        })
        emptyHint.apply {
            text = "Nenhum jogo ainda.\nAperte START (ou toque em Menu) → Adicionar pasta."
            setTextColor(textMain)
            gravity = Gravity.CENTER
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            visibility = View.GONE
        }
        home.addView(emptyHint, FrameLayout.LayoutParams(MATCH, MATCH))
    }

    private fun closestToCenter(): Int {
        val mid = carousel.width / 2f
        var best = -1
        var bestDist = Float.MAX_VALUE
        for (i in 0 until carousel.childCount) {
            val v = carousel.getChildAt(i)
            val d = abs((v.left + v.right) / 2f - mid)
            if (d < bestDist) {
                bestDist = d
                best = carousel.getChildAdapterPosition(v)
            }
        }
        return best
    }

    /** Cards shrink and fade with their distance from the centre; the centre one gets the outline. */
    private fun styleCards() {
        val mid = carousel.width / 2f
        for (i in 0 until carousel.childCount) {
            val v = carousel.getChildAt(i)
            val d = min(1f, abs((v.left + v.right) / 2f - mid) / (cardWidth + dp(20)))
            val s = 1f - 0.18f * d
            v.scaleX = s
            v.scaleY = s
            v.alpha = 1f - 0.45f * d
            val pos = carousel.getChildAdapterPosition(v)
            (v.background as? GradientDrawable)?.setStroke(if (d < 0.2f && pos == centered) dp(3) else 0, accent)
        }
    }

    private fun setCentered(pos: Int) {
        centered = pos
        val c = collections.getOrNull(pos) ?: return
        homeTitle.text = c.title
        homeSubtitle.text = c.subtitle
        background.setTint(c.color)
        dots.removeAllViews()
        val n = collections.size
        for (i in 0 until n) dots.addView(View(context).apply {
            background = GradientDrawable().apply {
                cornerRadius = dp(5).toFloat()
                setColor(if (i == pos) accent else Color.argb(110, 160, 150, 220))
            }
        }, LinearLayout.LayoutParams(if (i == pos) dp(26) else dp(9), dp(9)).apply { leftMargin = dp(4); rightMargin = dp(4) })
        styleCards()
    }

    private fun moveCarousel(step: Int) {
        val to = (centered + step).coerceIn(0, collections.size - 1)
        if (to == centered) return
        sounds.play(FrontendSounds.Sfx.MOVE)
        setCentered(to)
        carousel.smoothScrollToPosition(to)
    }

    private fun open() {
        val c = collections.getOrNull(centered) ?: return
        sounds.play(FrontendSounds.Sfx.SELECT)
        openList(c, keepSelection = false)
    }

    private fun showHomeHints() = setHints(
        Hint("A", HINT_A, "Abrir") { open() },
        Hint("Y", HINT_Y, "Continuar") { continueLast() },
        Hint("X", HINT_X, "Buscar") { search() },
        Hint("START", 0, "Menu") { host.mainMenu() },
    )

    private fun continueLast() {
        val last = host.games().maxByOrNull { host.store.lastPlayed(it) }?.takeIf { host.store.lastPlayed(it) > 0 } ?: return
        sounds.play(FrontendSounds.Sfx.LAUNCH)
        host.launch(last)
    }

    private fun search() {
        host.search { q ->
            val found = host.games().filter { it.title.contains(q, ignoreCase = true) }
            openList(Collection("search", "🔍", "Busca: $q", Color.rgb(59, 130, 246), found, "${found.size} jogos"), false)
        }
    }

    // --- The game list ---------------------------------------------------------------------

    private val gameList = RecyclerView(context)
    private val tabRow = LinearLayout(context)
    private val badge = TextView(context)
    private val badgeArt = ImageView(context)
    private val listTitle = TextView(context)
    private val cover = ImageView(context)
    private val preview = TextureView(context)
    private val name = TextView(context)
    private val meta = TextView(context)
    private val chips = LinearLayout(context)
    private val description = TextView(context)
    private val shots = LinearLayout(context)
    private var player: MediaPlayer? = null
    private var listBuilt = false

    private fun buildList() {
        listBuilt = true
        val header = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(20), dp(12), dp(20), 0)
        }
        badge.apply {
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            setPadding(dp(10), dp(4), dp(10), dp(4))
        }
        listTitle.apply {
            setTextColor(Color.argb(230, 220, 216, 240))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f)
            setPadding(dp(12), 0, 0, 0)
            maxLines = 1
        }
        header.addView(badgeArt, LinearLayout.LayoutParams(dp(44), dp(44)).apply { rightMargin = dp(8) })
        header.addView(badge)
        header.addView(listTitle)
        tabRow.orientation = LinearLayout.HORIZONTAL
        val tabScroll = HorizontalScrollView(context).apply {
            isHorizontalScrollBarEnabled = false
            setPadding(dp(16), dp(8), dp(16), 0)
            clipToPadding = false
            addView(tabRow)
        }

        gameList.layoutManager = LinearLayoutManager(context)
        gameList.adapter = listAdapter
        gameList.isFocusable = false
        gameList.clipToPadding = false
        gameList.setPadding(dp(10), dp(6), dp(10), dp(64))

        // The panel: cover / moving preview, facts, saves.
        val panel = FrameLayout(context).apply {
            background = GradientDrawable().apply {
                cornerRadius = dp(20).toFloat()
                setColor(Color.argb(215, 22, 20, 34))
            }
            setPadding(dp(16), dp(16), dp(16), dp(16))
        }
        val picture = FrameLayout(context).apply {
            background = GradientDrawable().apply { cornerRadius = dp(12).toFloat(); setColor(Color.rgb(7, 6, 12)) }
            clipToOutline = true
            outlineProvider = android.view.ViewOutlineProvider.BACKGROUND
        }
        cover.scaleType = ImageView.ScaleType.FIT_CENTER
        picture.addView(cover, FrameLayout.LayoutParams(MATCH, MATCH))
        picture.addView(preview, FrameLayout.LayoutParams(MATCH, MATCH))
        preview.alpha = 0f
        name.apply {
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
            maxLines = 2
        }
        meta.apply { setTextColor(textDim); setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f); setLineSpacing(0f, 1.2f) }
        chips.orientation = LinearLayout.HORIZONTAL
        description.apply {
            setTextColor(Color.argb(220, 169, 166, 188))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            maxLines = if (landscape) 5 else 3
            ellipsize = android.text.TextUtils.TruncateAt.END
            setLineSpacing(0f, 1.15f)
        }
        shots.orientation = LinearLayout.HORIZONTAL
        val play = TextView(context).apply {
            text = "▶  Jogar"
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            gravity = Gravity.CENTER
            background = GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                intArrayOf(Color.rgb(124, 92, 255), Color.rgb(90, 63, 224))).apply { cornerRadius = dp(24).toFloat() }
            setOnClickListener { playSelected() }
        }
        val texts = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(name)
            addView(meta, LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(4) })
            addView(HorizontalScrollView(context).apply { isHorizontalScrollBarEnabled = false; addView(chips) },
                LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(8) })
            addView(description, LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(8) })
            addView(play, LinearLayout.LayoutParams(dp(150), dp(46)).apply { topMargin = dp(12) })
        }

        if (landscape) {
            val listW = 0.42f
            list.addView(header, FrameLayout.LayoutParams(MATCH, WRAP))
            list.addView(tabScroll, FrameLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(48) })
            list.addView(gameList, FrameLayout.LayoutParams((root.width.takeIf { it > 0 } ?: context.resources.displayMetrics.widthPixels).let { (it * listW).toInt() }, MATCH).apply {
                topMargin = dp(96)
            })
            val top = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(picture, LinearLayout.LayoutParams(dp(230), dp(230)))
                addView(texts, LinearLayout.LayoutParams(0, WRAP, 1f).apply { leftMargin = dp(18) })
            }
            val col = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                addView(top)
                addView(shots, LinearLayout.LayoutParams(MATCH, dp(96)).apply { topMargin = dp(14) })
            }
            panel.addView(col, FrameLayout.LayoutParams(MATCH, WRAP))
            val wPx = context.resources.displayMetrics.widthPixels
            list.addView(panel, FrameLayout.LayoutParams((wPx * (1 - listW)).toInt() - dp(36), MATCH, Gravity.END).apply {
                topMargin = dp(60); bottomMargin = dp(64); rightMargin = dp(20)
            })
        } else {
            list.addView(header, FrameLayout.LayoutParams(MATCH, WRAP))
            val top = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(picture, LinearLayout.LayoutParams(dp(140), dp(140)))
                addView(texts, LinearLayout.LayoutParams(0, WRAP, 1f).apply { leftMargin = dp(14) })
            }
            panel.addView(top, FrameLayout.LayoutParams(MATCH, WRAP))
            list.addView(panel, FrameLayout.LayoutParams(MATCH, WRAP).apply {
                topMargin = dp(52); leftMargin = dp(12); rightMargin = dp(12)
            })
            list.addView(tabScroll, FrameLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(300) })
            list.addView(gameList, FrameLayout.LayoutParams(MATCH, MATCH).apply { topMargin = dp(344) })
            description.maxLines = 2
        }
        preview.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) = schedulePreview()
            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) = fitPreview()
            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean { stopPreview(); return true }
            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }
    }

    private inner class RowHolder(val row: LinearLayout, val star: TextView, val title: TextView, val side: TextView) :
        RecyclerView.ViewHolder(row)

    private val listAdapter = object : RecyclerView.Adapter<RowHolder>() {
        override fun getItemCount() = shown.size
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RowHolder {
            val row = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(14), 0, dp(14), 0)
                layoutParams = RecyclerView.LayoutParams(MATCH, dp(44))
            }
            val star = TextView(context).apply { setTextColor(accent); setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f) }
            val title = TextView(context).apply {
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
                maxLines = 1
                ellipsize = android.text.TextUtils.TruncateAt.END
            }
            val side = TextView(context).apply { setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f) }
            row.addView(star)
            row.addView(title, LinearLayout.LayoutParams(0, WRAP, 1f))
            row.addView(side)
            return RowHolder(row, star, title, side)
        }

        override fun onBindViewHolder(h: RowHolder, position: Int) {
            val g = shown[position]
            val on = position == selected
            h.star.text = if (host.store.isFavorite(g)) "★ " else ""
            h.title.text = g.title
            h.title.setTextColor(if (on) Color.WHITE else Color.argb(225, 207, 203, 227))
            h.title.typeface = if (on) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
            val time = host.playTime(g)
            h.side.text = when {
                time > 0 -> PlayStats.formatTime(time)
                else -> host.facts(g)?.year ?: g.system.shortName
            }
            h.side.setTextColor(if (on) Color.argb(230, 232, 227, 255) else Color.argb(160, 143, 138, 168))
            h.row.background = if (on) GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                intArrayOf(Color.rgb(124, 92, 255), Color.rgb(90, 63, 224))).apply { cornerRadius = dp(12).toFloat() } else null
            h.row.elevation = if (on) dp(6).toFloat() else 0f
            h.row.setOnClickListener {
                val pos = h.bindingAdapterPosition
                if (pos == selected) playSelected() else select(pos, fromTouch = true)
            }
            h.row.setOnLongClickListener { select(h.bindingAdapterPosition, fromTouch = true); host.gameMenu(g); true }
        }
    }

    private fun openList(c: Collection, keepSelection: Boolean) {
        if (!listBuilt) buildList()
        val keep = if (keepSelection) shown.getOrNull(selected)?.uri else null
        current = c
        inList = true
        home.visibility = View.GONE
        list.visibility = View.VISIBLE
        background.setTint(c.color)
        badge.text = c.label.let { if (it.length > 4) c.title.take(3) else it }
        badge.background = GradientDrawable().apply { cornerRadius = dp(10).toFloat(); setColor(c.color) }
        badgeArt.setImageResource(c.art)
        listTitle.text = "${c.title} · ${c.games.size} jogos"
        // Tabs: all, favourites, recent, then the most common genres.
        val genres = c.games.mapNotNull { host.facts(it)?.genre?.substringBefore(" (")?.substringBefore(",")?.trim()?.ifEmpty { null } }
            .groupingBy { it }.eachCount().entries.sortedByDescending { it.value }.take(4).map { it.key }
        tabs = listOf<Pair<String, (Game) -> Boolean>>("Todos" to { _ -> true }) +
            (if (c.key != "fav") listOf<Pair<String, (Game) -> Boolean>>("★ Favoritos" to { g -> host.store.isFavorite(g) }) else emptyList()) +
            (if (c.key != "recent") listOf<Pair<String, (Game) -> Boolean>>("Recentes" to { g -> host.store.lastPlayed(g) > 0 }) else emptyList()) +
            genres.map { genre -> genre to { g: Game -> host.facts(g)?.genre?.startsWith(genre) == true } }
        if (!keepSelection) tab = 0
        tab = tab.coerceIn(0, tabs.size - 1)
        applyTab(keep)
        showListHints()
    }

    private fun applyTab(keepUri: String? = null) {
        val c = current ?: return
        val filter = tabs.getOrNull(tab)?.second ?: { _ -> true }
        shown = c.games.filter(filter).let { l ->
            if (tabs.getOrNull(tab)?.first == "Recentes" || c.key == "recent") l.sortedByDescending { host.store.lastPlayed(it) } else l
        }
        tabRow.removeAllViews()
        tabs.forEachIndexed { i, (label, _) ->
            tabRow.addView(TextView(context).apply {
                text = label
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                setPadding(dp(12), dp(5), dp(12), dp(5))
                setTextColor(if (i == tab) Color.rgb(27, 26, 38) else Color.argb(220, 189, 184, 214))
                typeface = if (i == tab) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
                background = GradientDrawable().apply {
                    cornerRadius = dp(14).toFloat()
                    setColor(if (i == tab) accent else Color.argb(200, 38, 36, 52))
                }
                setOnClickListener { setTab(i) }
            }, LinearLayout.LayoutParams(WRAP, WRAP).apply { rightMargin = dp(8) })
        }
        selected = keepUri?.let { u -> shown.indexOfFirst { it.uri == u } }?.takeIf { it >= 0 } ?: 0
        listAdapter.notifyDataSetChanged()
        gameList.scrollToPosition(selected)
        showSelected()
    }

    private fun setTab(i: Int) {
        if (i == tab || i !in tabs.indices) return
        sounds.play(FrontendSounds.Sfx.MOVE)
        tab = i
        applyTab()
    }

    private fun select(pos: Int, fromTouch: Boolean = false) {
        if (pos !in shown.indices || pos == selected) return
        val old = selected
        selected = pos
        listAdapter.notifyItemChanged(old)
        listAdapter.notifyItemChanged(pos)
        if (!fromTouch) {
            // Keep the selection away from the edges.
            val lm = gameList.layoutManager as LinearLayoutManager
            val visible = (lm.findLastCompletelyVisibleItemPosition() - lm.findFirstCompletelyVisibleItemPosition()).coerceAtLeast(1)
            lm.scrollToPositionWithOffset(pos, (gameList.height / 2 - dp(22)).coerceAtLeast(0).let { if (visible < 3) 0 else it })
        }
        sounds.play(FrontendSounds.Sfx.MOVE)
        showSelected()
    }

    private val showRunnable = Runnable { showDetails() }

    /** Fills the panel; the slower parts (description, preview) after a short pause. */
    private fun showSelected() {
        stopPreview()
        val g = shown.getOrNull(selected)
        if (g == null) {
            name.text = "Nenhum jogo aqui"
            meta.text = ""
            chips.removeAllViews()
            description.text = ""
            cover.setImageDrawable(null)
            shots.removeAllViews()
            return
        }
        name.text = g.title
        val f = host.facts(g)
        meta.text = listOfNotNull(
            listOfNotNull(f?.year?.ifEmpty { null }, f?.genre?.ifEmpty { null },
                f?.players?.ifEmpty { null }?.let { if (it == "1") "1 jogador" else "1–$it jogadores" }).joinToString(" · ").ifEmpty { null },
            listOfNotNull(f?.developer?.ifEmpty { null }, f?.publisher?.ifEmpty { null }?.takeIf { it != f.developer }).joinToString(" / ").ifEmpty { null },
            g.system.fullName.takeIf { f == null },
        ).joinToString("\n")
        chips.removeAllViews()
        val time = host.playTime(g)
        if (time > 0) addChip("⏱ ${PlayStats.formatTime(time)}")
        val slots = (1..6).count { File(context.filesDir, "states/${g.system.name}/${g.saveName}.state$it").exists() }
        if (slots > 0) addChip("💾 $slots ${if (slots == 1) "save" else "saves"}")
        if (host.store.isFavorite(g)) addChip("★ Favorito")
        description.text = ""
        cover.setImageDrawable(null)
        // No cover: the console's drawing instead of an empty frame.
        host.covers.load(g, cover) { found -> if (!found && cover.tag != null) cover.setImageResource(artFor(g.system.name)) }
        shots.removeAllViews()
        main.removeCallbacks(showRunnable)
        main.postDelayed(showRunnable, 250)
    }

    private fun addChip(text: String) {
        chips.addView(TextView(context).apply {
            this.text = text
            setTextColor(Color.argb(235, 230, 226, 250))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setPadding(dp(9), dp(4), dp(9), dp(4))
            background = GradientDrawable().apply { cornerRadius = dp(10).toFloat(); setColor(Color.argb(220, 38, 36, 52)) }
        }, LinearLayout.LayoutParams(WRAP, WRAP).apply { rightMargin = dp(6) })
    }

    private fun showDetails() {
        val g = shown.getOrNull(selected) ?: return
        val uri = g.uri
        io.execute {
            val kept = host.info.keptDescription(g)
            val pictures = mutableListOf<Pair<String, android.graphics.Bitmap?>>()
            PlayStats.thumb(context, g)?.let { pictures += ("Continuar · " + PlayStats.formatAgo(PlayStats.lastPlayed(context, g))) to it }
            for (slot in 1..6) {
                val f = File(context.filesDir, "states/${g.system.name}/${g.saveName}.state$slot.png")
                if (f.exists()) pictures += "Slot $slot" to android.graphics.BitmapFactory.decodeFile(f.path)
                if (pictures.size >= 4) break
            }
            val facts = host.facts(g)
            main.post {
                if (shown.getOrNull(selected)?.uri != uri) return@post
                description.text = kept?.takeIf { it.isNotEmpty() } ?: facts?.let { "" } ?: ""
                shots.removeAllViews()
                for ((label, bmp) in pictures) {
                    val box = FrameLayout(context).apply {
                        background = GradientDrawable().apply { cornerRadius = dp(10).toFloat(); setColor(Color.rgb(7, 6, 12)) }
                        clipToOutline = true
                        outlineProvider = android.view.ViewOutlineProvider.BACKGROUND
                    }
                    box.addView(ImageView(context).apply {
                        scaleType = ImageView.ScaleType.CENTER_CROP
                        bmp?.let { setImageDrawable(BitmapDrawable(context.resources, it).apply { isFilterBitmap = it.width > 300 }) }
                    }, FrameLayout.LayoutParams(MATCH, MATCH))
                    box.addView(TextView(context).apply {
                        text = label
                        setTextColor(Color.WHITE)
                        setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
                        typeface = Typeface.DEFAULT_BOLD
                        setPadding(dp(6), dp(2), dp(6), dp(2))
                        background = GradientDrawable().apply { cornerRadius = dp(6).toFloat(); setColor(Color.argb(170, 0, 0, 0)) }
                    }, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.BOTTOM or Gravity.START).apply { leftMargin = dp(6); bottomMargin = dp(6) })
                    shots.addView(box, LinearLayout.LayoutParams(0, MATCH, 1f).apply { rightMargin = dp(8) })
                }
                schedulePreview()
            }
        }
    }

    // --- Moving preview -----------------------------------------------------------------------

    private fun schedulePreview() {
        if (!prefs.uiPreviews || !inList || !preview.isAvailable) return
        val g = shown.getOrNull(selected) ?: return
        val file = PreviewClip.file(context, g)
        if (!file.exists()) return
        stopPreview()
        val uri = g.uri
        val mp = MediaPlayer()
        player = mp
        try {
            mp.setSurface(Surface(preview.surfaceTexture))
            mp.setDataSource(file.path)
            mp.isLooping = true
            mp.setVolume(0f, 0f)
            mp.setOnPreparedListener {
                if (player !== mp || shown.getOrNull(selected)?.uri != uri) return@setOnPreparedListener
                fitPreview()
                it.start()
                preview.animate().alpha(1f).setDuration(400).start()
            }
            mp.setOnErrorListener { _, _, _ -> stopPreview(); true }
            mp.prepareAsync()
        } catch (e: Exception) {
            stopPreview()
        }
    }

    /** Fit-centre the clip in the square: scale the texture, keep its shape. */
    private fun fitPreview() {
        val mp = player ?: return
        val vw = mp.videoWidth.toFloat()
        val vh = mp.videoHeight.toFloat()
        if (vw <= 0 || vh <= 0 || preview.width == 0) return
        val w = preview.width.toFloat()
        val h = preview.height.toFloat()
        val s = min(w / vw, h / vh)
        val m = Matrix()
        m.setScale(vw * s / w, vh * s / h, w / 2, h / 2)
        preview.setTransform(m)
    }

    private fun stopPreview() {
        preview.animate().cancel()
        preview.alpha = 0f
        player?.let { runCatching { it.stop() }; it.release() }
        player = null
    }

    // --- Actions and keys ------------------------------------------------------------------------

    private fun playSelected() {
        val g = shown.getOrNull(selected) ?: return
        sounds.play(FrontendSounds.Sfx.LAUNCH)
        host.launch(g)
    }

    private fun backToHome() {
        if (!inList) return
        sounds.play(FrontendSounds.Sfx.BACK)
        stopPreview()
        inList = false
        list.visibility = View.GONE
        home.visibility = View.VISIBLE
        collections.getOrNull(centered)?.let { background.setTint(it.color) }
        showHomeHints()
    }

    private fun toggleFavorite() {
        val g = shown.getOrNull(selected) ?: return
        host.store.setFavorite(g, !host.store.isFavorite(g))
        sounds.play(FrontendSounds.Sfx.SELECT)
        refresh()
    }

    private fun showListHints() = setHints(
        Hint("A", HINT_A, "Jogar") { playSelected() },
        Hint("B", HINT_B, "Voltar") { backToHome() },
        Hint("Y", HINT_Y, "Favorito") { toggleFavorite() },
        Hint("X", HINT_X, "Opções") { shown.getOrNull(selected)?.let { host.gameMenu(it) } },
    )

    /** Controller / remote / keyboard. Returns true when used. */
    fun onKey(e: KeyEvent): Boolean {
        // A key whose press was used has its release used too, and only then.
        if (e.action != KeyEvent.ACTION_DOWN) return consumed.remove(e.keyCode)
        val used = handleDown(e)
        if (used) consumed += e.keyCode
        return used
    }

    private val consumed = HashSet<Int>()

    private fun handleDown(e: KeyEvent): Boolean {
        val code = e.keyCode
        if (!inList) when (code) {
            KeyEvent.KEYCODE_DPAD_LEFT -> { moveCarousel(-1); return true }
            KeyEvent.KEYCODE_DPAD_RIGHT -> { moveCarousel(1); return true }
            KeyEvent.KEYCODE_BUTTON_L1 -> { moveCarousel(-3); return true }
            KeyEvent.KEYCODE_BUTTON_R1 -> { moveCarousel(3); return true }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_BUTTON_A -> { open(); return true }
            KeyEvent.KEYCODE_BUTTON_Y -> { continueLast(); return true }
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_SEARCH -> { search(); return true }
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> { host.mainMenu(); return true }
        } else when (code) {
            KeyEvent.KEYCODE_DPAD_UP -> { select(selected - 1); return true }
            KeyEvent.KEYCODE_DPAD_DOWN -> { select(selected + 1); return true }
            KeyEvent.KEYCODE_DPAD_LEFT -> { select((selected - 10).coerceAtLeast(0)); return true }
            KeyEvent.KEYCODE_DPAD_RIGHT -> { select((selected + 10).coerceAtMost(shown.size - 1)); return true }
            KeyEvent.KEYCODE_BUTTON_L1 -> { setTab(tab - 1); return true }
            KeyEvent.KEYCODE_BUTTON_R1 -> { setTab(tab + 1); return true }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_BUTTON_A -> {
                if (e.repeatCount == 0) playSelected(); return true
            }
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_ESCAPE -> { backToHome(); return true }
            KeyEvent.KEYCODE_BUTTON_Y -> { toggleFavorite(); return true }
            KeyEvent.KEYCODE_BUTTON_X -> { shown.getOrNull(selected)?.let { host.gameMenu(it) }; return true }
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> { host.mainMenu(); return true }
        }
        return false
    }

    /** Back gesture / button on a phone. True when it was used (list → home). */
    fun onBack(): Boolean {
        if (!inList) return false
        backToHome()
        return true
    }

    fun resume() {
        sounds.effectsOn = Prefs(context).uiSounds
        sounds.musicOn = Prefs(context).uiMusic
        background.animating = Prefs(context).uiAnimatedBackground
        if (sounds.musicOn) sounds.startMusic() else sounds.pauseMusic()
        updateBattery()
        if (inList) schedulePreview()
    }

    fun pause() {
        sounds.pauseMusic()
        background.animating = false
        stopPreview()
    }

    fun release() {
        stopPreview()
        sounds.release()
    }

    // --- Bits ---------------------------------------------------------------------------------------

    private data class Hint(val key: String, val color: Int, val label: String, val action: () -> Unit)

    private fun setHints(vararg items: Hint) {
        hints.removeAllViews()
        for (h in items) {
            val item = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(8), dp(6), dp(12), dp(6))
                isClickable = true
                background = ContextCompat.getDrawable(context, android.R.drawable.list_selector_background)
                setOnClickListener { h.action() }
            }
            val key = TextView(context).apply {
                text = h.key
                setTextColor(Color.WHITE)
                typeface = Typeface.DEFAULT_BOLD
                gravity = Gravity.CENTER
                setTextSize(TypedValue.COMPLEX_UNIT_SP, if (h.key.length > 1) 10f else 13f)
                background = GradientDrawable().apply {
                    if (h.color != 0) shape = GradientDrawable.OVAL else cornerRadius = dp(10).toFloat()
                    setColor(if (h.color != 0) h.color else Color.rgb(58, 66, 84))
                }
                setPadding(if (h.key.length > 1) dp(8) else 0, 0, if (h.key.length > 1) dp(8) else 0, 0)
            }
            item.addView(key, LinearLayout.LayoutParams(if (h.key.length > 1) WRAP else dp(24), dp(24)))
            item.addView(TextView(context).apply {
                text = h.label
                setTextColor(Color.argb(230, 207, 203, 227))
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                setPadding(dp(8), 0, 0, 0)
            })
            hints.addView(item)
        }
    }

    private val battery = TextView(context)

    private fun statusBar(): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        addView(TextClock(context).apply {
            format24Hour = "HH:mm"
            format12Hour = "h:mm a"
            setTextColor(Color.argb(230, 217, 213, 238))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
        })
        battery.setTextColor(Color.argb(230, 217, 213, 238))
        battery.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
        addView(battery)
    }

    private fun updateBattery() {
        val bm = context.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
        val level = bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: -1
        val charging = bm?.isCharging == true
        battery.text = if (level in 0..100) "  ·  ${if (charging) "⚡" else "🔋"} $level%" else ""
    }

    /** Builds the screens; runs last in the constructor, once every view field exists. */
    private fun setup() {
        sounds.effectsOn = prefs.uiSounds
        sounds.musicOn = prefs.uiMusic
        background.animating = prefs.uiAnimatedBackground
        root.setBackgroundColor(Color.rgb(11, 10, 20))
        root.addView(background, FrameLayout.LayoutParams(MATCH, MATCH))
        root.addView(home, FrameLayout.LayoutParams(MATCH, MATCH))
        root.addView(list, FrameLayout.LayoutParams(MATCH, MATCH))
        hints.apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(20), 0, dp(20), 0)
            background = GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP,
                intArrayOf(Color.argb(170, 0, 0, 0), Color.TRANSPARENT))
        }
        root.addView(hints, FrameLayout.LayoutParams(MATCH, dp(56), Gravity.BOTTOM))
        root.addView(statusBar(), FrameLayout.LayoutParams(WRAP, WRAP, Gravity.TOP or Gravity.END).apply {
            topMargin = dp(14); rightMargin = dp(20)
        })
        root.fitsSystemWindows = true
        list.visibility = View.GONE
        buildHome()
    }

    init {
        setup()
    }

    companion object {
        fun artFor(key: String) = when (key) {
            "GB" -> R.drawable.photo_gb
            "GBA" -> R.drawable.photo_gba
            "NDS" -> R.drawable.photo_nds
            "N3DS" -> R.drawable.photo_n3ds
            "NES" -> R.drawable.photo_nes
            "SNES" -> R.drawable.photo_snes
            "MD" -> R.drawable.photo_md
            "SMS" -> R.drawable.photo_sms
            "GG" -> R.drawable.photo_gg
            "PS1" -> R.drawable.photo_ps1
            "recent" -> R.drawable.art_recent
            "fav" -> R.drawable.art_fav
            "all" -> R.drawable.art_all
            else -> R.drawable.art_search
        }

        private const val MATCH = ViewGroup.LayoutParams.MATCH_PARENT
        private const val WRAP = ViewGroup.LayoutParams.WRAP_CONTENT
        private val HINT_A = Color.rgb(239, 68, 68)
        private val HINT_B = Color.rgb(245, 158, 11)
        private val HINT_Y = Color.rgb(34, 197, 94)
        private val HINT_X = Color.rgb(59, 130, 246)
    }
}
