package com.cartucho.emu

import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.graphics.Color
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.Process
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.io.File
import java.text.DateFormat
import java.util.Date
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Plays one game. Runs in its own process (see the manifest), which ends when
 * the player leaves the game.
 */
class GameActivity : AppCompatActivity() {

    private lateinit var game: Game
    private lateinit var prefs: Prefs
    private lateinit var glView: GLSurfaceView
    private lateinit var overlay: ControllerOverlay
    private lateinit var osd: TextView
    private lateinit var editPanel: LinearLayout
    private lateinit var gamepad: GamepadInput

    private lateinit var stateDir: File

    @Volatile private var loaded = false
    @Volatile private var resumed = false
    private var menuOpen = false
    private var fastForward = false
    private var overlayMask = 0
    private var overlayStickX = 0f
    private var overlayStickY = 0f
    private var frameCounter = 0
    private var messageUntil = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)
        game = Game(
            intent.getStringExtra(EXTRA_URI)!!,
            intent.getStringExtra(EXTRA_FILE)!!,
            GameSystem.fromName(intent.getStringExtra(EXTRA_SYSTEM))!!,
            intent.getLongExtra(EXTRA_SIZE, 0L),
        )
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        hideSystemBars()

        buildViews()
        gamepad = GamepadInput(onChange = ::pushInput, onMenu = ::showMenu)
        gamepad.stickAsDpad = game.system != GameSystem.N3DS

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (overlay.editMode) finishEditing() else showMenu()
            }
        })

        val error = prepare()
        if (error != null) {
            fail(error)
            return
        }
        glView.setRenderer(Renderer())
        glView.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        rendererSet = true
    }

    // GLSurfaceView crashes on onPause()/onResume() without a renderer, which
    // happens when the game failed to open.
    private var rendererSet = false

    // --- Setup ------------------------------------------------------------

    private fun buildViews() {
        val root = FrameLayout(this)
        root.setBackgroundColor(Color.BLACK)

        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            // 3DS games need depth and stencil in the core's own framebuffer,
            // not on screen; keep the window surface lean.
            setEGLConfigChooser(8, 8, 8, 0, 0, 0)
            preserveEGLContextOnPause = true
        }
        root.addView(glView, FrameLayout.LayoutParams(MATCH, MATCH))

        overlay = ControllerOverlay(this).apply {
            listener = OverlayListener()
            onSelectionChanged = { c -> editPanel.findViewWithTag<View>("size")?.isEnabled = c != null }
        }
        root.addView(overlay, FrameLayout.LayoutParams(MATCH, MATCH))

        osd = TextView(this).apply {
            setTextColor(Color.WHITE)
            setShadowLayer(4f, 0f, 1f, Color.BLACK)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }
        root.addView(osd, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.TOP or Gravity.CENTER_HORIZONTAL))

        editPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(0xE61B1A26.toInt())
            setPadding(dp(16), dp(12), dp(16), dp(12))
            visibility = View.GONE
            addView(TextView(context).apply {
                text = getString(R.string.edit_controls_hint)
                setTextColor(Color.WHITE)
                gravity = Gravity.CENTER
            })
            addView(LinearLayout(context).apply {
                gravity = Gravity.CENTER
                setPadding(0, dp(8), 0, 0)
                addView(button("−") { this@GameActivity.overlay.scaleSelected(1f / 1.1f) }.apply { tag = "size" })
                addView(button("+") { this@GameActivity.overlay.scaleSelected(1.1f) })
                addView(button(getString(R.string.reset_layout)) { this@GameActivity.overlay.resetLayout() })
                addView(button(getString(R.string.done)) { finishEditing() })
            })
        }
        root.addView(editPanel, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER))

        setContentView(root)
        applyOverlaySettings()
    }

    private fun button(label: String, onClick: () -> Unit) = MaterialButton(this).apply {
        text = label
        setOnClickListener { onClick() }
        layoutParams = LinearLayout.LayoutParams(WRAP, WRAP).apply { marginStart = dp(4); marginEnd = dp(4) }
    }

    /** Opens the ROM, loads the core and passes everything native needs. */
    private fun prepare(): String? {
        val romPath = if (game.system == GameSystem.N3DS) {
            Storage.directPath(this, game)
                ?: return "Não foi possível abrir ${game.fileName} direto da pasta. " +
                    "Libere \"Acesso a todos os arquivos\" para o Cartucho e deixe o jogo na " +
                    "memória interna ou no cartão SD."
        } else {
            // Small: copy to the cache, named like the game (the core looks at
            // the extension).
            val romDir = File(cacheDir, "rom").apply { deleteRecursively(); mkdirs() }
            val copy = File(romDir, game.fileName.replace('/', '_'))
            try {
                contentResolver.openInputStream(Uri.parse(game.uri))?.use { input ->
                    copy.outputStream().use { input.copyTo(it) }
                } ?: return "Não foi possível abrir ${game.fileName}."
            } catch (e: Exception) {
                return "Não foi possível abrir ${game.fileName}. A pasta ainda existe?"
            }
            copy.path
        }

        val systemDir = File(filesDir, "system").apply { mkdirs() }
        val saveDir = File(filesDir, "saves/${game.system.name}").apply { mkdirs() }
        stateDir = File(filesDir, "states/${game.system.name}").apply { mkdirs() }
        val core = File(applicationInfo.nativeLibraryDir, game.system.coreLibrary)
        if (!core.exists()) return "O emulador de ${game.system.fullName} não está neste APK."

        NativeBridge.open(core.path, systemDir.path, saveDir.path)?.let { return it }
        applyCoreOptions()
        NativeBridge.setGame(romPath, File(saveDir, game.saveName + ".srm").path)
        return null
    }

    private val landscape get() = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    private fun applyCoreOptions() {
        if (game.system != GameSystem.N3DS) {
            // Real BIOS when the player imported one, built-in otherwise.
            NativeBridge.setOption("vbam_usebios", if (Bios.anyFor(this, game.system)) "enabled" else "disabled")
            return
        }
        NativeBridge.setOption("citra_graphics_api", "OpenGL")
        NativeBridge.setOption("citra_use_cpu_jit", "enabled")
        NativeBridge.setOption("citra_enable_touch_touchscreen", "enabled")
        NativeBridge.setOption("citra_enable_mouse_touchscreen", "disabled")
        NativeBridge.setOption("citra_analog_function", "c_stick")
        NativeBridge.setOption("citra_resolution_factor", prefs.n3dsResolution)
        NativeBridge.setOption("citra_is_new_3ds", if (prefs.n3dsNew3ds) "New 3DS" else "Old 3DS")
        NativeBridge.setOption("citra_swap_screen", if (prefs.n3dsSwapScreens) "Bottom" else "Top")
        val layout = prefs.n3dsLayout.let {
            if (it == "auto") (if (landscape) "large_screen" else "default") else it
        }
        NativeBridge.setOption("citra_layout_option", layout)
    }

    private fun applyDisplay() {
        val portrait = !landscape
        val reserve = if (portrait) (if (game.system == GameSystem.N3DS) 0.3f else 0.38f) else 0f
        val filter = prefs.filter
        val integer = prefs.integerScale
        glView.queueEvent { NativeBridge.setDisplay(filter, integer, if (portrait) 1 else 0, reserve) }
    }

    private fun applyOverlaySettings() {
        overlay.opacity = prefs.overlayOpacity
        overlay.globalScale = prefs.overlayScale
        overlay.haptics = prefs.haptics
        overlay.configure(game.system, landscape)
    }

    private fun hideSystemBars() {
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // Dialogs and rotation can bring the bars back.
        if (hasFocus) hideSystemBars()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        hideSystemBars()
        overlay.configure(game.system, landscape)
        if (loaded) {
            applyCoreOptions()
            applyDisplay()
        }
    }

    // --- Lifecycle ----------------------------------------------------------

    override fun onResume() {
        super.onResume()
        resumed = true
        if (!rendererSet) return
        glView.onResume()
        if (!menuOpen) NativeBridge.setPaused(false)
        if (loaded) NativeBridge.startAudio()
    }

    override fun onPause() {
        resumed = false
        if (rendererSet) {
            NativeBridge.setPaused(true)
            NativeBridge.stopAudio()
            if (loaded) glView.queueEvent { NativeBridge.flushSave() }
            glView.onPause()
        }
        super.onPause()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) {
            // Everything native was closed by quit(); end the game process
            // so the next game starts from a clean slate.
            Process.killProcess(Process.myPid())
        }
    }

    private fun quit() {
        if (!loaded) {
            finish()
            return
        }
        NativeBridge.setPaused(true)
        NativeBridge.stopAudio()
        glView.queueEvent {
            NativeBridge.close()
            runOnUiThread { finish() }
        }
    }

    private fun fail(message: String) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.error_title)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton(R.string.ok) { _, _ -> finish() }
            .show()
    }

    // --- Rendering ------------------------------------------------------------

    private inner class Renderer : GLSurfaceView.Renderer {
        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            val error = NativeBridge.surfaceCreated()
            if (error != null) {
                runOnUiThread { fail(error) }
                return
            }
            if (!loaded) {
                loaded = true
                runOnUiThread {
                    applyDisplay()
                    if (resumed) NativeBridge.startAudio()
                    showMessage(game.title)
                }
            }
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            NativeBridge.surfaceChanged(width, height)
        }

        override fun onDrawFrame(gl: GL10?) {
            if (!loaded) return
            NativeBridge.drawFrame()
            if (++frameCounter % 20 == 0) {
                val message = NativeBridge.takeMessage()
                val fps = NativeBridge.measuredFps()
                val shutdown = NativeBridge.shutdownRequested()
                val vp = NativeBridge.viewport()
                runOnUiThread {
                    overlay.setGameRect(android.graphics.Rect(vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3]))
                    if (shutdown) quit()
                    if (message != null) showMessage(message)
                    updateOsd(fps)
                }
            }
        }
    }

    private fun showMessage(text: String) {
        osd.text = text
        messageUntil = System.currentTimeMillis() + 2500
    }

    private fun updateOsd(fps: Float) {
        if (System.currentTimeMillis() < messageUntil) return
        val parts = mutableListOf<String>()
        if (prefs.showFps) parts += "%.0f FPS".format(fps)
        if (fastForward) parts += "» %.0fx".format(prefs.fastForwardSpeed)
        osd.text = parts.joinToString("   ")
    }

    // --- Input ------------------------------------------------------------------

    private fun pushInput() {
        NativeBridge.setButtons(overlayMask or gamepad.buttons)
        val padActive = gamepad.leftX != 0f || gamepad.leftY != 0f
        NativeBridge.setAnalog(0, if (padActive) gamepad.leftX else overlayStickX,
                               if (padActive) gamepad.leftY else overlayStickY)
        NativeBridge.setAnalog(1, gamepad.rightX, gamepad.rightY)
        if (gamepad.gamepadSeen && prefs.hideWithGamepad && overlay.visibility == View.VISIBLE && !overlay.editMode)
            overlay.visibility = View.INVISIBLE
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (!menuOpen && !overlay.editMode && gamepad.onKey(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (!menuOpen && gamepad.onMotion(event)) return true
        return super.dispatchGenericMotionEvent(event)
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        // Touching the screen brings the hidden controls back.
        if (overlay.visibility != View.VISIBLE && event.actionMasked == MotionEvent.ACTION_DOWN) {
            overlay.visibility = View.VISIBLE
        }
        return super.dispatchTouchEvent(event)
    }

    private inner class OverlayListener : ControllerOverlay.Listener {
        override fun onButtons(mask: Int) {
            overlayMask = mask
            pushInput()
        }

        override fun onAnalog(stick: Int, x: Float, y: Float) {
            overlayStickX = x
            overlayStickY = y
            pushInput()
        }

        override fun onScreenTouch(x: Float, y: Float, pressed: Boolean) {
            NativeBridge.touch(x, y, pressed)
        }

        override fun onMenu() = showMenu()

        override fun onFastForward() = toggleFastForward()
    }

    private fun toggleFastForward() {
        fastForward = !fastForward
        NativeBridge.setSpeed(if (fastForward) prefs.fastForwardSpeed else 1f)
        overlay.fastForwardOn = fastForward
        updateOsd(0f)
    }

    // --- Menu ---------------------------------------------------------------------

    private fun showMenu() {
        if (menuOpen || !loaded) return
        menuOpen = true
        NativeBridge.setPaused(true)
        val items = mutableListOf(
            getString(R.string.menu_resume) to { },
            getString(R.string.menu_save_state) to { pickSlot(save = true) },
            getString(R.string.menu_load_state) to { pickSlot(save = false) },
            (if (fastForward) getString(R.string.speed_normal)
             else getString(R.string.speed_x, prefs.fastForwardSpeed.toInt())) to { toggleFastForward() },
            getString(R.string.menu_filter) to { pickFilter() },
        )
        if (game.system == GameSystem.N3DS) items += getString(R.string.menu_screen_layout) to { pickLayout() }
        items += getString(R.string.menu_edit_controls) to { startEditing() }
        items += (if (overlay.visibility == View.VISIBLE) getString(R.string.menu_hide_controls)
                  else getString(R.string.menu_show_controls)) to {
            overlay.visibility = if (overlay.visibility == View.VISIBLE) View.INVISIBLE else View.VISIBLE
        }
        items += getString(R.string.menu_reset) to { glView.queueEvent { NativeBridge.reset() } }
        items += getString(R.string.menu_quit) to { quit() }

        MaterialAlertDialogBuilder(this)
            .setTitle(game.title)
            .setItems(items.map { it.first }.toTypedArray()) { _, which ->
                items[which].second()
            }
            .setOnDismissListener { closeMenu() }
            .show()
    }

    private fun closeMenu() {
        // A submenu may still be open; it closes the menu itself.
        window.decorView.post {
            if (!subDialogOpen) {
                menuOpen = false
                if (resumed && !overlay.editMode) NativeBridge.setPaused(false)
            }
        }
    }

    private var subDialogOpen = false

    private fun subDialog(builder: MaterialAlertDialogBuilder) {
        subDialogOpen = true
        builder.setOnDismissListener {
            subDialogOpen = false
            closeMenu()
        }.show()
    }

    private fun stateFile(slot: Int) = File(stateDir, "${game.saveName}.state$slot")

    private fun pickSlot(save: Boolean) {
        val fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
        val labels = (1..6).map { slot ->
            val f = stateFile(slot)
            if (f.exists()) getString(R.string.slot_used, slot, fmt.format(Date(f.lastModified())))
            else getString(R.string.slot_empty, slot)
        }.toTypedArray()
        subDialog(MaterialAlertDialogBuilder(this)
            .setTitle(if (save) R.string.menu_save_state else R.string.menu_load_state)
            .setItems(labels) { _, which ->
                val slot = which + 1
                val path = stateFile(slot).path
                glView.queueEvent {
                    val error = if (save) NativeBridge.saveState(path) else NativeBridge.loadState(path)
                    runOnUiThread {
                        showMessage(error ?: getString(if (save) R.string.state_saved else R.string.state_loaded, slot))
                    }
                }
            })
    }

    private fun pickFilter() {
        val names = resources.getStringArray(R.array.filter_names)
        subDialog(MaterialAlertDialogBuilder(this)
            .setTitle(R.string.menu_filter)
            .setSingleChoiceItems(names, prefs.filter) { d, which ->
                prefs.setFilter(which)
                applyDisplay()
                d.dismiss()
            })
    }

    private fun pickLayout() {
        val names = resources.getStringArray(R.array.layout_names) + getString(R.string.swap_screens)
        val values = resources.getStringArray(R.array.layout_values)
        val current = values.indexOf(prefs.n3dsLayout).coerceAtLeast(0)
        subDialog(MaterialAlertDialogBuilder(this)
            .setTitle(R.string.menu_screen_layout)
            .setSingleChoiceItems(names, current) { d, which ->
                if (which < values.size) prefs.setN3dsLayout(values[which])
                else prefs.setN3dsSwap(!prefs.n3dsSwapScreens)
                applyCoreOptions()
                d.dismiss()
            })
    }

    private fun startEditing() {
        overlay.visibility = View.VISIBLE
        overlay.setEditMode(true)
        editPanel.visibility = View.VISIBLE
        NativeBridge.setPaused(true)
    }

    private fun finishEditing() {
        overlay.setEditMode(false)
        editPanel.visibility = View.GONE
        menuOpen = false
        if (resumed) NativeBridge.setPaused(false)
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    companion object {
        private const val MATCH = ViewGroup.LayoutParams.MATCH_PARENT
        private const val WRAP = ViewGroup.LayoutParams.WRAP_CONTENT
        private const val EXTRA_URI = "uri"
        private const val EXTRA_FILE = "file"
        private const val EXTRA_SYSTEM = "system"
        private const val EXTRA_SIZE = "size"

        fun intent(context: Context, game: Game) = Intent(context, GameActivity::class.java)
            .putExtra(EXTRA_URI, game.uri)
            .putExtra(EXTRA_FILE, game.fileName)
            .putExtra(EXTRA_SYSTEM, game.system.name)
            .putExtra(EXTRA_SIZE, game.size)
    }
}
