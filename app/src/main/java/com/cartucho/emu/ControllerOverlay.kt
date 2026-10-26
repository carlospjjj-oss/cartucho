package com.cartucho.emu

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import org.json.JSONObject
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * The on-screen controller drawn over the game.
 *
 * - Multi-touch: hold A while steering with the D-pad, slide a finger from B
 *   to A, etc. Touches that land on no control go to the game as a touchscreen
 *   press (the 3DS bottom screen).
 * - Per console and per orientation layouts, which the player can rearrange
 *   and resize (edit mode); changes are saved.
 * - Opacity, size and haptic feedback come from the settings.
 */
class ControllerOverlay(context: Context) : View(context) {

    enum class Kind { BUTTON, DPAD, STICK, MENU, FAST_FORWARD }

    /** One control. Position is the centre, as a fraction of the view size. */
    data class Control(
        val id: String,
        val kind: Kind,
        val label: String,
        val bit: Int,
        var x: Float,
        var y: Float,
        val widthDp: Float,
        val heightDp: Float = widthDp,
        var scale: Float = 1f,
    ) {
        val round get() = widthDp == heightDp
    }

    interface Listener {
        fun onButtons(mask: Int)
        fun onAnalog(stick: Int, x: Float, y: Float)
        fun onScreenTouch(x: Float, y: Float, pressed: Boolean)
        fun onMenu()
        fun onFastForward()
    }

    var listener: Listener? = null
    var opacity = 0.6f
        set(value) { field = value; invalidate() }
    var globalScale = 1f
        set(value) { field = value; invalidate() }
    var haptics = true
    /** Highlights the fast-forward button while active. */
    var fastForwardOn = false
        set(value) { field = value; invalidate() }

    var editMode = false
        private set
    private var selected: Control? = null
    var onSelectionChanged: ((Control?) -> Unit)? = null

    private var system = GameSystem.GBA
    private var landscape = true
    private var controls: List<Control> = emptyList()

    private val density = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * resources.displayMetrics.density
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }
    private val rect = RectF()

    // Touch state.
    private val pointerControl = HashMap<Int, Control>()
    private var screenPointer = -1
    private var dpadMask = 0
    private var stickX = 0f
    private var stickY = 0f
    private var lastMask = 0
    private var dragOffsetX = 0f
    private var dragOffsetY = 0f

    fun configure(system: GameSystem, landscape: Boolean) {
        this.system = system
        this.landscape = landscape
        controls = defaults(system, landscape)
        loadLayout()
        releaseAll()
        invalidate()
    }

    // --- Layout persistence -----------------------------------------------

    private fun layoutKey() = "layout_${system.name}_${if (landscape) "land" else "port"}"

    private fun loadLayout() {
        val json = Prefs.raw(context).getString(layoutKey(), null) ?: return
        runCatching {
            val o = JSONObject(json)
            for (c in controls) {
                val v = o.optJSONArray(c.id) ?: continue
                c.x = v.getDouble(0).toFloat()
                c.y = v.getDouble(1).toFloat()
                c.scale = v.getDouble(2).toFloat()
            }
        }
    }

    private fun saveLayout() {
        val o = JSONObject()
        for (c in controls)
            o.put(c.id, org.json.JSONArray(listOf(c.x.toDouble(), c.y.toDouble(), c.scale.toDouble())))
        Prefs.raw(context).edit().putString(layoutKey(), o.toString()).apply()
    }

    fun setEditMode(on: Boolean) {
        editMode = on
        releaseAll()
        if (!on) {
            selected = null
            saveLayout()
        }
        onSelectionChanged?.invoke(selected)
        invalidate()
    }

    fun scaleSelected(factor: Float) {
        val c = selected ?: return
        c.scale = (c.scale * factor).coerceIn(0.5f, 2.5f)
        invalidate()
    }

    fun resetLayout() {
        Prefs.raw(context).edit().remove(layoutKey()).apply()
        controls = defaults(system, landscape)
        selected = null
        onSelectionChanged?.invoke(null)
        invalidate()
    }

    // --- Geometry ---------------------------------------------------------

    private fun halfW(c: Control) = c.widthDp * density * globalScale * c.scale / 2f
    private fun halfH(c: Control) = c.heightDp * density * globalScale * c.scale / 2f
    private fun cx(c: Control) = c.x * width
    private fun cy(c: Control) = c.y * height

    private fun hit(c: Control, x: Float, y: Float, slack: Float = 1.15f): Boolean {
        val dx = x - cx(c)
        val dy = y - cy(c)
        return if (c.round || c.kind == Kind.DPAD) {
            hypot(dx, dy) <= max(halfW(c), halfH(c)) * slack
        } else {
            kotlin.math.abs(dx) <= halfW(c) * slack && kotlin.math.abs(dy) <= halfH(c) * slack
        }
    }

    private fun controlAt(x: Float, y: Float, kinds: Set<Kind>? = null): Control? {
        // Exact hits first, then the slack zone, smallest control winning
        // (so the gap between A and B still reaches the nearer one).
        val candidates = controls.filter { kinds == null || it.kind in kinds }
        candidates.firstOrNull { hit(it, x, y, 1f) }?.let { return it }
        return candidates.filter { hit(it, x, y) }
            .minByOrNull { hypot(x - cx(it), y - cy(it)) / max(halfW(it), halfH(it)) }
    }

    // --- Drawing ------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        for (c in controls) drawControl(canvas, c)
    }

    private fun pressed(c: Control): Boolean = when (c.kind) {
        Kind.BUTTON -> pointerControl.values.any { it === c }
        Kind.DPAD -> dpadMask != 0
        Kind.STICK -> pointerControl.values.any { it === c }
        Kind.MENU -> false
        Kind.FAST_FORWARD -> fastForwardOn
    }

    private fun drawControl(canvas: Canvas, c: Control) {
        val a = (opacity * 255).toInt().coerceIn(20, 255)
        val isPressed = pressed(c)
        val isSelected = editMode && c === selected
        fill.color = if (isPressed) Color.argb((a * 0.85f).toInt(), 107, 78, 255)
                     else Color.argb((a * 0.35f).toInt(), 255, 255, 255)
        stroke.color = if (isSelected) Color.argb(255, 255, 181, 71) else Color.argb((a * 0.9f).toInt(), 255, 255, 255)
        text.color = Color.argb(a.coerceAtLeast(120), 255, 255, 255)
        val x = cx(c)
        val y = cy(c)
        val hw = halfW(c)
        val hh = halfH(c)
        when (c.kind) {
            Kind.DPAD -> drawDpad(canvas, x, y, hw, a)
            Kind.STICK -> {
                fill.color = Color.argb((a * 0.25f).toInt(), 255, 255, 255)
                canvas.drawCircle(x, y, hw, fill)
                canvas.drawCircle(x, y, hw, stroke)
                fill.color = if (isPressed) Color.argb((a * 0.9f).toInt(), 107, 78, 255)
                             else Color.argb((a * 0.6f).toInt(), 255, 255, 255)
                canvas.drawCircle(x + stickX * hw * 0.55f, y + stickY * hw * 0.55f, hw * 0.45f, fill)
            }
            else -> {
                if (c.round) {
                    canvas.drawCircle(x, y, hw, fill)
                    canvas.drawCircle(x, y, hw, stroke)
                } else {
                    rect.set(x - hw, y - hh, x + hw, y + hh)
                    canvas.drawRoundRect(rect, hh, hh, fill)
                    canvas.drawRoundRect(rect, hh, hh, stroke)
                }
                text.textSize = min(hw, hh) * if (c.label.length > 2) 0.62f else 0.9f
                canvas.drawText(c.label, x, y - (text.descent() + text.ascent()) / 2f, text)
            }
        }
        if (isSelected) {
            rect.set(x - hw - 6 * density, y - hh - 6 * density, x + hw + 6 * density, y + hh + 6 * density)
            canvas.drawRoundRect(rect, 12 * density, 12 * density, stroke)
        }
    }

    private fun drawDpad(canvas: Canvas, x: Float, y: Float, half: Float, a: Int) {
        val arm = half * 0.36f
        val arms = listOf(
            Retro.UP to RectF(x - arm, y - half, x + arm, y - arm),
            Retro.DOWN to RectF(x - arm, y + arm, x + arm, y + half),
            Retro.LEFT to RectF(x - half, y - arm, x - arm, y + arm),
            Retro.RIGHT to RectF(x + arm, y - arm, x + half, y + arm),
        )
        fill.color = Color.argb((a * 0.35f).toInt(), 255, 255, 255)
        canvas.drawRect(x - arm, y - arm, x + arm, y + arm, fill)
        for ((bit, r) in arms) {
            fill.color = if ((dpadMask and (1 shl bit)) != 0) Color.argb((a * 0.85f).toInt(), 107, 78, 255)
                         else Color.argb((a * 0.35f).toInt(), 255, 255, 255)
            canvas.drawRoundRect(r, arm * 0.35f, arm * 0.35f, fill)
        }
        // Outline of the cross.
        canvas.drawRoundRect(RectF(x - arm, y - half, x + arm, y + half), arm * 0.35f, arm * 0.35f, stroke)
        canvas.drawRoundRect(RectF(x - half, y - arm, x + half, y + arm), arm * 0.35f, arm * 0.35f, stroke)
    }

    // --- Touch ----------------------------------------------------------------

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (editMode) return onEditTouch(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = e.actionIndex
                pointerDown(e.getPointerId(i), e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until e.pointerCount) pointerMove(e.getPointerId(i), e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val i = e.actionIndex
                pointerUp(e.getPointerId(i), e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        publish()
        return true
    }

    private fun pointerDown(id: Int, x: Float, y: Float) {
        val c = controlAt(x, y)
        when {
            c == null -> {
                if (screenPointer == -1) {
                    screenPointer = id
                    listener?.onScreenTouch(x, y, true)
                }
            }
            c.kind == Kind.MENU -> { haptic(); listener?.onMenu() }
            c.kind == Kind.FAST_FORWARD -> { haptic(); listener?.onFastForward() }
            else -> {
                pointerControl[id] = c
                if (c.kind == Kind.DPAD) updateDpad(c, x, y)
                if (c.kind == Kind.STICK) updateStick(c, x, y)
                haptic()
            }
        }
    }

    private fun pointerMove(id: Int, x: Float, y: Float) {
        if (id == screenPointer) {
            listener?.onScreenTouch(x, y, true)
            return
        }
        val c = pointerControl[id] ?: return
        when (c.kind) {
            Kind.DPAD -> updateDpad(c, x, y)
            Kind.STICK -> updateStick(c, x, y)
            Kind.BUTTON -> {
                // Sliding between face buttons is a common technique; follow
                // the finger, but only between buttons.
                if (!hit(c, x, y)) {
                    val other = controlAt(x, y, setOf(Kind.BUTTON))
                    if (other != null) {
                        if (other !== c) haptic()
                        pointerControl[id] = other
                    } else {
                        pointerControl.remove(id)
                    }
                }
            }
            else -> {}
        }
    }

    private fun pointerUp(id: Int, x: Float, y: Float) {
        if (id == screenPointer) {
            screenPointer = -1
            listener?.onScreenTouch(x, y, false)
            return
        }
        val c = pointerControl.remove(id) ?: return
        if (c.kind == Kind.DPAD) dpadMask = 0
        if (c.kind == Kind.STICK) {
            stickX = 0f; stickY = 0f
            listener?.onAnalog(0, 0f, 0f)
        }
    }

    private fun releaseAll() {
        pointerControl.clear()
        dpadMask = 0
        if (stickX != 0f || stickY != 0f) listener?.onAnalog(0, 0f, 0f)
        stickX = 0f; stickY = 0f
        if (screenPointer != -1) listener?.onScreenTouch(0f, 0f, false)
        screenPointer = -1
        publish()
    }

    private fun updateDpad(c: Control, x: Float, y: Float) {
        val half = halfW(c)
        val dx = (x - cx(c)) / half
        val dy = (y - cy(c)) / half
        var m = 0
        // A dead zone in the middle, and diagonals when the finger is far
        // enough along both axes (about 22 degrees either side of 45).
        val t = 0.28f
        if (hypot(dx, dy) > t) {
            if (dx > t && dx > kotlin.math.abs(dy) * 0.4f) m = m or (1 shl Retro.RIGHT)
            if (dx < -t && -dx > kotlin.math.abs(dy) * 0.4f) m = m or (1 shl Retro.LEFT)
            if (dy > t && dy > kotlin.math.abs(dx) * 0.4f) m = m or (1 shl Retro.DOWN)
            if (dy < -t && -dy > kotlin.math.abs(dx) * 0.4f) m = m or (1 shl Retro.UP)
        }
        if (m != dpadMask && m != 0 && (m and dpadMask) != m) haptic()
        dpadMask = m
    }

    private fun updateStick(c: Control, x: Float, y: Float) {
        val r = halfW(c) * 0.75f
        var dx = (x - cx(c)) / r
        var dy = (y - cy(c)) / r
        val len = hypot(dx, dy)
        if (len > 1f) { dx /= len; dy /= len }
        stickX = dx; stickY = dy
        listener?.onAnalog(0, dx, dy)
        invalidate()
    }

    private fun publish() {
        var mask = dpadMask
        for (c in pointerControl.values) if (c.kind == Kind.BUTTON && c.bit >= 0) mask = mask or (1 shl c.bit)
        if (mask != lastMask) {
            lastMask = mask
            listener?.onButtons(mask)
        }
        invalidate()
    }

    private fun haptic() {
        if (haptics) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
    }

    // --- Edit mode ------------------------------------------------------------

    private fun onEditTouch(e: MotionEvent): Boolean {
        val x = e.x
        val y = e.y
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                selected = controlAt(x, y)
                selected?.let { dragOffsetX = x - cx(it); dragOffsetY = y - cy(it) }
                onSelectionChanged?.invoke(selected)
            }
            MotionEvent.ACTION_MOVE -> selected?.let {
                it.x = ((x - dragOffsetX) / width).coerceIn(0.02f, 0.98f)
                it.y = ((y - dragOffsetY) / height).coerceIn(0.02f, 0.98f)
            }
        }
        invalidate()
        return true
    }

    companion object {
        private fun b(id: String, label: String, bit: Int, x: Float, y: Float, size: Float = 64f) =
            Control(id, Kind.BUTTON, label, bit, x, y, size)

        private fun pill(id: String, label: String, bit: Int, x: Float, y: Float, w: Float = 96f, h: Float = 40f) =
            Control(id, Kind.BUTTON, label, bit, x, y, w, h)

        private fun dpad(x: Float, y: Float, size: Float) = Control("dpad", Kind.DPAD, "", -1, x, y, size)
        private fun menu(x: Float, y: Float) = Control("menu", Kind.MENU, "☰", -1, x, y, 40f)
        private fun ff(x: Float, y: Float) = Control("ff", Kind.FAST_FORWARD, "»", -1, x, y, 40f)

        fun defaults(system: GameSystem, landscape: Boolean): List<Control> = when (system) {
            GameSystem.N3DS -> if (landscape) listOf(
                Control("stick", Kind.STICK, "", -1, 0.09f, 0.42f, 124f),
                dpad(0.11f, 0.78f, 132f),
                b("a", "A", Retro.A, 0.945f, 0.62f, 56f),
                b("b", "B", Retro.B, 0.88f, 0.76f, 56f),
                b("x", "X", Retro.X, 0.88f, 0.48f, 56f),
                b("y", "Y", Retro.Y, 0.815f, 0.62f, 56f),
                pill("l", "L", Retro.L, 0.06f, 0.09f, 84f),
                pill("zl", "ZL", Retro.L2, 0.17f, 0.09f, 72f),
                pill("r", "R", Retro.R, 0.94f, 0.09f, 84f),
                pill("zr", "ZR", Retro.R2, 0.83f, 0.09f, 72f),
                pill("select", "SELECT", Retro.SELECT, 0.42f, 0.95f, 76f, 30f),
                pill("start", "START", Retro.START, 0.58f, 0.95f, 76f, 30f),
                menu(0.5f, 0.05f),
                ff(0.43f, 0.05f),
            ) else listOf(
                pill("l", "L", Retro.L, 0.09f, 0.6f, 84f),
                pill("zl", "ZL", Retro.L2, 0.27f, 0.6f, 72f),
                pill("r", "R", Retro.R, 0.91f, 0.6f, 84f),
                pill("zr", "ZR", Retro.R2, 0.73f, 0.6f, 72f),
                Control("stick", Kind.STICK, "", -1, 0.19f, 0.71f, 116f),
                dpad(0.21f, 0.865f, 128f),
                b("a", "A", Retro.A, 0.91f, 0.79f, 58f),
                b("b", "B", Retro.B, 0.8f, 0.86f, 58f),
                b("x", "X", Retro.X, 0.8f, 0.72f, 58f),
                b("y", "Y", Retro.Y, 0.69f, 0.79f, 58f),
                pill("select", "SELECT", Retro.SELECT, 0.4f, 0.955f, 76f, 30f),
                pill("start", "START", Retro.START, 0.6f, 0.955f, 76f, 30f),
                menu(0.93f, 0.025f),
                ff(0.07f, 0.025f),
            )
            else -> {
                val shoulders = system == GameSystem.GBA
                if (landscape) listOfNotNull(
                    dpad(0.12f, 0.68f, 160f),
                    b("b", "B", Retro.B, 0.8f, 0.76f, 72f),
                    b("a", "A", Retro.A, 0.91f, 0.62f, 72f),
                    if (shoulders) pill("l", "L", Retro.L, 0.08f, 0.12f, 104f, 46f) else null,
                    if (shoulders) pill("r", "R", Retro.R, 0.92f, 0.12f, 104f, 46f) else null,
                    pill("select", "SELECT", Retro.SELECT, 0.42f, 0.94f, 80f, 32f),
                    pill("start", "START", Retro.START, 0.58f, 0.94f, 80f, 32f),
                    menu(0.5f, 0.06f),
                    ff(0.42f, 0.06f),
                ) else listOfNotNull(
                    if (shoulders) pill("l", "L", Retro.L, 0.13f, 0.5f, 104f, 46f) else null,
                    if (shoulders) pill("r", "R", Retro.R, 0.87f, 0.5f, 104f, 46f) else null,
                    dpad(0.24f, 0.7f, 172f),
                    b("b", "B", Retro.B, 0.7f, 0.74f, 76f),
                    b("a", "A", Retro.A, 0.86f, 0.66f, 76f),
                    pill("select", "SELECT", Retro.SELECT, 0.38f, 0.9f, 84f, 34f),
                    pill("start", "START", Retro.START, 0.62f, 0.9f, 84f, 34f),
                    menu(0.93f, 0.025f),
                    ff(0.07f, 0.025f),
                )
            }
        }
    }
}
