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

    private var romCrc = 0L
    // Kept open for the whole game: the core reads the ROM through it.
    private var romFile: android.os.ParcelFileDescriptor? = null
    private lateinit var linkUi: LinkUi
    private lateinit var cheats: Cheats
    @Volatile private var loaded = false
    @Volatile private var resumed = false
    private var fastForward = false
    private var overlayMask = 0
    private var overlayStickX = 0f
    private var overlayStickY = 0f
    private var frameCounter = 0
    private var messageUntil = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ErrorReport.install(this)
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

        val extracted = game.entry?.let { Archives.extractedFile(this, game) }
        if (extracted != null && !(extracted.exists() && extracted.length() > 0)) extractThenStart()
        else startGame()
    }

    private fun startGame() {
        val error = prepare()
        if (error != null) {
            fail(error)
            return
        }
        ErrorReport.gameStarting(this, game)
        cheats = Cheats(this, game)
        linkUi = LinkUi(this, glView, game, romCrc, modal)
        // The surface view joins the layout only now: GLSurfaceView crashes
        // if its surface appears before it has a renderer.
        root.addView(glView, 0, FrameLayout.LayoutParams(MATCH, MATCH))
        glView.setRenderer(Renderer())
        glView.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        rendererSet = true
        if (resumed) glView.onResume()
    }

    /** Zipped game played for the first time: extract it, with progress. */
    private fun extractThenStart() {
        val bar = com.google.android.material.progressindicator.LinearProgressIndicator(this).apply {
            max = 1000
            isIndeterminate = false
        }
        val box = FrameLayout(this).apply {
            setPadding(dp(24), dp(16), dp(24), dp(8))
            addView(bar)
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Extraindo ${game.title}…")
            .setView(box)
            .setCancelable(false)
            .show()
        Thread {
            try {
                var last = -1
                Archives.extract(this, game) { p ->
                    val v = (p * 1000).toInt()
                    if (v != last) {
                        last = v
                        runOnUiThread { bar.setProgressCompat(v, false) }
                    }
                }
                runOnUiThread {
                    dialog.dismiss()
                    if (!isFinishing) startGame()
                }
            } catch (e: Exception) {
                runOnUiThread {
                    dialog.dismiss()
                    fail("Não foi possível extrair ${game.archive}: ${e.message ?: e.javaClass.simpleName}")
                }
            }
        }.start()
    }

    // GLSurfaceView crashes on onPause()/onResume() without a renderer, which
    // happens when the game failed to open.
    private var rendererSet = false

    // --- Setup ------------------------------------------------------------

    private lateinit var root: FrameLayout

    private fun buildViews() {
        root = FrameLayout(this)
        root.setBackgroundColor(Color.BLACK)

        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            // 3DS games need depth and stencil in the core's own framebuffer,
            // not on screen; keep the window surface lean.
            setEGLConfigChooser(8, 8, 8, 0, 0, 0)
            preserveEGLContextOnPause = true
        }

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
        var romFd = -1
        val extracted = game.entry?.let { Archives.extractedFile(this, game) }
        val romPath = if (extracted != null) {
            // Zipped: already extracted to app storage, readable by path.
            if (game.system == GameSystem.GBA || game.system == GameSystem.GB)
                romCrc = java.util.zip.CRC32().apply { update(extracted.readBytes()) }.value
            extracted.path
        } else if (game.system == GameSystem.N3DS) {
            // The 3DS core opens the game itself, by path, and games are
            // gigabytes: open it where it is (needs all-files access).
            Storage.directPath(this, game)
                ?: return "Não foi possível abrir ${game.fileName} direto da pasta. " +
                    "Libere \"Acesso a todos os arquivos\" para o Cartucho e deixe o jogo na " +
                    "memória interna ou no cartão SD."
        } else {
            // The GB/GBA and DS cores take the game from memory: read it
            // straight from the picked file. The path only carries the name
            // (the cores look at the extension); it does not need to exist.
            val pfd = try {
                contentResolver.openFileDescriptor(Uri.parse(game.uri), "r")
            } catch (e: Exception) {
                null
            } ?: return "Não foi possível abrir ${game.fileName}. A pasta ainda existe?"
            romFile = pfd
            romFd = pfd.fd
            if (game.system != GameSystem.NDS && game.size in 1..64L * 1024 * 1024) {
                // Shown to link partners so they can tell it is the same game.
                romCrc = try {
                    contentResolver.openInputStream(Uri.parse(game.uri))?.use { input ->
                        val crc = java.util.zip.CRC32()
                        val buf = ByteArray(1 shl 16)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            crc.update(buf, 0, n)
                        }
                        crc.value
                    } ?: 0L
                } catch (e: Exception) {
                    0L
                }
            }
            File(cacheDir, "rom/" + game.fileName.replace('/', '_')).path
        }

        val systemDir = File(filesDir, "system").apply { mkdirs() }
        val saveDir = File(filesDir, "saves/${game.system.name}").apply { mkdirs() }
        stateDir = File(filesDir, "states/${game.system.name}").apply { mkdirs() }
        val core = File(applicationInfo.nativeLibraryDir, game.system.coreLibrary)
        if (!core.exists()) return "O emulador de ${game.system.fullName} não está neste APK."

        NativeBridge.open(core.path, systemDir.path, saveDir.path)?.let { return it }
        applyCoreOptions()
        NativeBridge.setGame(romPath, File(saveDir, game.saveName + ".srm").path, romFd)
        return null
    }

    private val landscape get() = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    private fun applyCoreOptions() {
        if (game.system == GameSystem.NDS) {
            NativeBridge.setOption("melonds_render_mode", "software")
            NativeBridge.setOption("melonds_jit_enable", "enabled")
            NativeBridge.setOption("melonds_sysfile_mode", "builtin")
            NativeBridge.setOption("melonds_console_mode", "ds")
            NativeBridge.setOption("melonds_touch_mode", "touch")
            NativeBridge.setOption("melonds_show_cursor", "disabled")
            NativeBridge.setOption("melonds_show_bios_warnings", "disabled")
            NativeBridge.setOption("melonds_number_of_screen_layouts", "1")
            val layout = prefs.ndsLayout.let {
                if (it == "auto") (if (landscape) "left-right" else "top-bottom") else it
            }
            NativeBridge.setOption("melonds_screen_layout1", layout)
            return
        }
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
        // Shaders compiled once are kept on disk, so a game stutters only the
        // first time it shows each effect.
        NativeBridge.setOption("citra_use_hw_shader", "enabled")
        NativeBridge.setOption("citra_use_disk_shader_cache", "enabled")
        NativeBridge.setOption("citra_shaders_accurate_mul", if (prefs.n3dsFastShaders) "disabled" else "enabled")
        NativeBridge.setOption("citra_texture_filter", prefs.n3dsTextureFilter)
        NativeBridge.setOption("citra_is_new_3ds", if (prefs.n3dsNew3ds) "New 3DS" else "Old 3DS")
        NativeBridge.setOption("citra_swap_screen", if (prefs.n3dsSwapScreens) "Bottom" else "Top")
        val layout = prefs.n3dsLayout.let {
            if (it == "auto") (if (landscape) "large_screen" else "default") else it
        }
        NativeBridge.setOption("citra_layout_option", layout)
    }

    private fun applyDisplay() {
        val portrait = !landscape
        val reserve = if (!portrait) 0f else when (game.system) {
            GameSystem.N3DS, GameSystem.NDS -> 0.3f
            else -> 0.38f
        }
        val filter = prefs.filter
        val integer = prefs.integerScale
        glView.queueEvent { NativeBridge.setDisplay(filter, integer, if (portrait) 1 else 0, reserve) }
    }

    private fun applyOverlaySettings() {
        overlay.opacity = prefs.overlayOpacity
        overlay.globalScale = prefs.overlayScale
        overlay.haptics = prefs.haptics
        overlay.quickButtons = prefs.quickButtons
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
        if (modalCount == 0 && !overlay.editMode) NativeBridge.setPaused(false)
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
            ErrorReport.gameEnded(this)
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
        if (::linkUi.isInitialized) linkUi.release()
        glView.queueEvent {
            NativeBridge.linkStop()
            NativeBridge.close()
            runOnUiThread { finish() }
        }
    }

    private fun fail(message: String) {
        ErrorReport.gameError(this, message)
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
                // Still on the GL thread: the core is ready for its codes.
                NativeBridge.setRewindEnabled(prefs.rewind)
                if (game.system != GameSystem.N3DS) cheats.apply()
                val canRewind = prefs.rewind && NativeBridge.rewindAvailable()
                runOnUiThread {
                    overlay.rewindAvailable = canRewind
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
        if (modalCount == 0 && !overlay.editMode && gamepad.onKey(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (modalCount == 0 && gamepad.onMotion(event)) return true
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

        override fun onRewind(held: Boolean) {
            if (held && ::linkUi.isInitialized && linkUi.connected) {
                showMessage("Voltar no tempo fica desligado durante o link")
                return
            }
            NativeBridge.setRewinding(held)
            if (held) showMessage("« Voltando no tempo")
        }

        override fun onQuickSave() = quickState(save = true)

        override fun onQuickLoad() = quickState(save = false)
    }

    /** Slot 0 is the quick slot (the on-screen S / L buttons). */
    private fun quickState(save: Boolean) {
        if (!loaded) return
        val path = stateFile(0).path
        glView.queueEvent {
            val error = if (save) NativeBridge.saveState(path) else NativeBridge.loadState(path)
            runOnUiThread {
                showMessage(error ?: if (save) "Salvo rápido" else "Carregado o salvo rápido")
            }
        }
    }

    private fun takeScreenshot() {
        glView.queueEvent {
            val data = NativeBridge.screenshot()
            Thread {
                val message = try {
                    if (data == null || data.size < 2) throw IllegalStateException("sem imagem")
                    val w = data[0]
                    val h = data[1]
                    val bmp = android.graphics.Bitmap.createBitmap(data, 2, w, w, h, android.graphics.Bitmap.Config.ARGB_8888)
                    saveScreenshot(bmp)
                } catch (e: Exception) {
                    "Não foi possível salvar a captura"
                }
                runOnUiThread { showMessage(message) }
            }.start()
        }
    }

    /** Saves to Pictures/Cartucho; returns the message to show. */
    private fun saveScreenshot(bmp: android.graphics.Bitmap): String {
        val name = game.saveName.take(60) + "-" +
            java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(Date()) + ".png"
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
            val values = android.content.ContentValues().apply {
                put(android.provider.MediaStore.Images.Media.DISPLAY_NAME, name)
                put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/png")
                put(android.provider.MediaStore.Images.Media.RELATIVE_PATH, "Pictures/Cartucho")
            }
            val uri = contentResolver.insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                ?: throw IllegalStateException("no uri")
            contentResolver.openOutputStream(uri)!!.use { bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
            return "Captura salva em Imagens/Cartucho"
        }
        val dir = File(getExternalFilesDir(android.os.Environment.DIRECTORY_PICTURES), "Cartucho").apply { mkdirs() }
        File(dir, name).outputStream().use { bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
        return "Captura salva em ${dir.path}"
    }

    private fun toggleFastForward() {
        // Both sides of a link must run at the same speed.
        if (!fastForward && ::linkUi.isInitialized && linkUi.connected) {
            showMessage("Acelerar fica desligado durante o link")
            return
        }
        fastForward = !fastForward
        NativeBridge.setSpeed(if (fastForward) prefs.fastForwardSpeed else 1f)
        overlay.fastForwardOn = fastForward
        updateOsd(0f)
    }

    // --- Menu ---------------------------------------------------------------------

    private fun showMenu() {
        if (modalCount > 0 || !loaded) return
        val items = mutableListOf(
            getString(R.string.menu_resume) to { },
            getString(R.string.menu_save_state) to { pickSlot(save = true) },
            getString(R.string.menu_load_state) to { pickSlot(save = false) },
            (if (fastForward) getString(R.string.speed_normal)
             else getString(R.string.speed_x, prefs.fastForwardSpeed.toInt())) to { toggleFastForward() },
            getString(R.string.menu_filter) to { pickFilter() },
        )
        if (game.system == GameSystem.N3DS) items += getString(R.string.menu_screen_layout) to { pickLayout() }
        if (game.system == GameSystem.NDS) items += getString(R.string.menu_screen_layout) to { pickNdsLayout() }
        if ((game.system == GameSystem.GBA || game.system == GameSystem.GB) && NativeBridge.linkSupported())
            items += (if (linkUi.connected) "Link cable: conectado" else "Link cable (rede)") to { linkUi.show() }
        if (game.system != GameSystem.N3DS) items += "Trapaças" + (if (cheats.list.any { it.enabled }) " (ativas)" else "") to {
            cheats.showDialog(this, { b -> modal.show(b) }) { glView.queueEvent { cheats.apply() } }
        }
        items += "Captura de tela" to { takeScreenshot() }
        items += getString(R.string.menu_edit_controls) to { startEditing() }
        items += (if (overlay.visibility == View.VISIBLE) getString(R.string.menu_hide_controls)
                  else getString(R.string.menu_show_controls)) to {
            overlay.visibility = if (overlay.visibility == View.VISIBLE) View.INVISIBLE else View.VISIBLE
        }
        items += getString(R.string.menu_reset) to { glView.queueEvent { NativeBridge.reset() } }
        items += getString(R.string.menu_quit) to { quit() }

        modal.show(MaterialAlertDialogBuilder(this)
            .setTitle(game.title)
            .setItems(items.map { it.first }.toTypedArray()) { _, which ->
                items[which].second()
            })
    }

    /**
     * Dialogs over the game keep it paused while any is open. A dialog that
     * opens another from its click handler hands over without a gap: the new
     * one is counted before the old one's dismissal.
     */
    private var modalCount = 0
    private val modal = object : LinkUi.Modal {
        override fun show(builder: MaterialAlertDialogBuilder, onDismiss: (() -> Unit)?): androidx.appcompat.app.AlertDialog {
            modalCount++
            NativeBridge.setPaused(true)
            builder.setOnDismissListener {
                onDismiss?.invoke()
                modalCount--
                if (modalCount == 0 && resumed && !overlay.editMode) NativeBridge.setPaused(false)
            }
            return builder.show()
        }
    }

    private fun subDialog(builder: MaterialAlertDialogBuilder) {
        modal.show(builder)
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

    private fun pickNdsLayout() {
        val names = resources.getStringArray(R.array.nds_layout_names)
        val values = resources.getStringArray(R.array.nds_layout_values)
        val current = values.indexOf(prefs.ndsLayout).coerceAtLeast(0)
        subDialog(MaterialAlertDialogBuilder(this)
            .setTitle(R.string.menu_screen_layout)
            .setSingleChoiceItems(names, current) { d, which ->
                prefs.setNdsLayout(values[which])
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
        if (resumed && modalCount == 0) NativeBridge.setPaused(false)
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
