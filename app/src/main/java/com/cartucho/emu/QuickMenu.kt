package com.cartucho.emu

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.core.content.ContextCompat

/**
 * The in-game menu as a console shows it: a panel sliding in from the left
 * over the paused game, and save slots as a grid of pictures. Works with
 * touch, a controller or a TV remote.
 */
@SuppressLint("SetTextI18n", "ViewConstructor")
class QuickMenu(private val context: Context, private val onClose: () -> Unit) {
    data class Item(val icon: String, val label: String, val value: String = "", val action: () -> Unit)
    data class Slot(val number: Int, val label: String, val picture: Bitmap?, val used: Boolean)

    private val density = context.resources.displayMetrics.density
    private fun dp(v: Int) = (v * density).toInt()
    private val accent = ContextCompat.getColor(context, R.color.accent)

    val root = FrameLayout(context).apply {
        setBackgroundColor(Color.argb(140, 5, 5, 12))
        visibility = View.GONE
        isClickable = true
        setOnClickListener { close() }
    }
    private val panel = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(22), dp(24), dp(22), dp(16))
        background = GradientDrawable().apply { setColor(Color.argb(244, 18, 16, 30)) }
        elevation = dp(16).toFloat()
        isClickable = true  // taps on the panel do not close the menu
    }
    private val title = TextView(context).apply {
        setTextColor(Color.WHITE)
        typeface = Typeface.DEFAULT_BOLD
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
        maxLines = 2
    }
    private val subtitle = TextView(context).apply {
        setTextColor(Color.argb(220, 169, 166, 188))
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
        setPadding(0, dp(2), 0, dp(14))
    }
    private val rows = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private val scroll = ScrollView(context).apply { isVerticalScrollBarEnabled = false; addView(rows) }
    private val slotsBox = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        visibility = View.GONE
        isClickable = true
    }

    private var items: List<Item> = emptyList()
    private var index = 0
    private var slots: List<Slot> = emptyList()
    private var slotIndex = 0
    private var onSlot: ((Int) -> Unit)? = null

    val visible get() = root.visibility == View.VISIBLE

    init {
        panel.addView(title)
        panel.addView(subtitle)
        panel.addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val landscape = context.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
        val width = if (landscape) dp(360) else (context.resources.displayMetrics.widthPixels * 0.86f).toInt()
        root.addView(panel, FrameLayout.LayoutParams(width, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.START))
        root.addView(slotsBox, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            if (landscape) Gravity.CENTER_VERTICAL else Gravity.BOTTOM).apply {
            leftMargin = if (landscape) width + dp(24) else dp(12)
            rightMargin = dp(24)
            bottomMargin = if (landscape) 0 else dp(24)
        })
    }

    fun show(gameTitle: String, info: String, list: List<Item>) {
        title.text = gameTitle
        subtitle.text = info
        items = list
        index = 0
        hideSlots()
        rows.removeAllViews()
        list.forEachIndexed { i, item ->
            rows.addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(12), 0, dp(12), 0)
                addView(TextView(context).apply {
                    text = item.icon
                    gravity = Gravity.CENTER
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f)
                }, LinearLayout.LayoutParams(dp(30), ViewGroup.LayoutParams.WRAP_CONTENT))
                addView(TextView(context).apply {
                    text = item.label
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
                    setPadding(dp(10), 0, 0, 0)
                    maxLines = 1
                }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                addView(TextView(context).apply {
                    text = item.value
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                })
                setOnClickListener { index = i; highlight(); activate() }
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(46)).apply { bottomMargin = dp(2) })
        }
        highlight()
        root.visibility = View.VISIBLE
        panel.translationX = -dp(380).toFloat()
        panel.animate().translationX(0f).setDuration(160).start()
    }

    private fun highlight() {
        for (i in 0 until rows.childCount) {
            val row = rows.getChildAt(i) as LinearLayout
            val on = i == index
            row.background = if (on) GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                intArrayOf(Color.rgb(124, 92, 255), Color.rgb(90, 63, 224))).apply { cornerRadius = dp(12).toFloat() } else null
            val label = row.getChildAt(1) as TextView
            label.setTextColor(if (on) Color.WHITE else Color.argb(230, 217, 213, 238))
            label.typeface = if (on) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
            (row.getChildAt(2) as TextView).setTextColor(if (on) Color.argb(230, 232, 227, 255) else Color.argb(200, 169, 166, 188))
        }
        rows.getChildAt(index)?.let { r -> scroll.post { scroll.smoothScrollTo(0, (r.top - scroll.height / 2 + r.height / 2).coerceAtLeast(0)) } }
    }

    private fun activate() {
        val item = items.getOrNull(index) ?: return
        item.action()
    }

    /** Shows the six slots as pictures; `pick` gets the slot number chosen. */
    fun showSlots(heading: String, list: List<Slot>, pick: (Int) -> Unit) {
        slots = list
        onSlot = pick
        slotIndex = list.indexOfFirst { it.used }.coerceAtLeast(0)
        slotsBox.removeAllViews()
        slotsBox.addView(TextView(context).apply {
            text = heading
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            setPadding(0, 0, 0, dp(12))
        })
        for (rowStart in list.indices step 3) {
            val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            for (i in rowStart until minOf(rowStart + 3, list.size)) {
                val s = list[i]
                val card = LinearLayout(context).apply {
                    orientation = LinearLayout.VERTICAL
                    setPadding(dp(8), dp(8), dp(8), dp(8))
                    setOnClickListener { slotIndex = i; chooseSlot() }
                }
                val pic = ImageView(context).apply {
                    scaleType = ImageView.ScaleType.FIT_CENTER
                    background = GradientDrawable().apply { cornerRadius = dp(8).toFloat(); setColor(Color.rgb(7, 6, 12)) }
                    s.picture?.let { setImageDrawable(BitmapDrawable(context.resources, it).apply { isFilterBitmap = it.width > 300 }) }
                }
                card.addView(pic, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(96)))
                card.addView(TextView(context).apply {
                    text = s.label
                    setTextColor(Color.argb(230, 207, 203, 227))
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                    setPadding(0, dp(6), 0, 0)
                    maxLines = 1
                })
                row.addView(card, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                    rightMargin = dp(10); bottomMargin = dp(10)
                })
            }
            slotsBox.addView(row)
        }
        slotsBox.visibility = View.VISIBLE
        highlightSlot()
    }

    private fun cards(): List<View> = (1 until slotsBox.childCount).flatMap { r ->
        val row = slotsBox.getChildAt(r) as LinearLayout
        (0 until row.childCount).map { row.getChildAt(it) }
    }

    private fun highlightSlot() {
        cards().forEachIndexed { i, v ->
            v.background = GradientDrawable().apply {
                cornerRadius = dp(14).toFloat()
                setColor(Color.argb(235, 27, 26, 38))
                if (i == slotIndex) setStroke(dp(3), accent)
            }
        }
    }

    private fun chooseSlot() {
        val s = slots.getOrNull(slotIndex) ?: return
        val pick = onSlot ?: return
        hideSlots()
        close()
        pick(s.number)
    }

    private fun hideSlots() {
        slotsBox.visibility = View.GONE
        onSlot = null
    }

    fun close() {
        if (!visible) return
        hideSlots()
        root.visibility = View.GONE
        onClose()
    }

    /** Controller / remote while the menu is open: everything goes here. */
    fun onKey(e: KeyEvent): Boolean {
        if (e.action != KeyEvent.ACTION_DOWN) return true
        val inSlots = slotsBox.visibility == View.VISIBLE
        when (e.keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> if (inSlots) moveSlot(-3) else move(-1)
            KeyEvent.KEYCODE_DPAD_DOWN -> if (inSlots) moveSlot(3) else move(1)
            KeyEvent.KEYCODE_DPAD_LEFT -> if (inSlots) moveSlot(-1)
            KeyEvent.KEYCODE_DPAD_RIGHT -> if (inSlots) moveSlot(1)
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_BUTTON_A ->
                if (e.repeatCount == 0) { if (inSlots) chooseSlot() else activate() }
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_ESCAPE ->
                if (inSlots) { hideSlots(); highlight() } else close()
            KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> close()
            KeyEvent.KEYCODE_MOVE_END -> { index = items.size - 1; highlight() }
            KeyEvent.KEYCODE_MOVE_HOME -> { index = 0; highlight() }
        }
        return true
    }

    private fun move(step: Int) {
        if (items.isEmpty()) return
        index = (index + step + items.size) % items.size  // wraps around
        highlight()
    }

    private fun moveSlot(step: Int) {
        if (slots.isEmpty()) return
        slotIndex = (slotIndex + step).coerceIn(0, slots.size - 1)
        highlightSlot()
    }
}
