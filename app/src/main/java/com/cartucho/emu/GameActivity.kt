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
    private lateinit var devices: Devices
    private lateinit var tvMode: TvMode
    private var onTv = false
    private lateinit var tvLabel: TextView
    private lateinit var recLabel: TextView
    private var recorder: VideoRecorder? = null
    private var lastRumble = 0
    private val micPermission = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.RequestPermission()) { granted ->
        devices.micPermissionAnswered()
        if (!granted) showMessage("Sem microfone: o jogo vai ouvir silêncio")
    }
    @Volatile private var loaded = false
    @Volatile private var resumed = false
    private var fastForward = false
    private var overlayMask = 0
    private var overlayStickX = 0f
    private var overlayStickY = 0f
    private var frameCounter = 0
    @Volatile private var achievementsOn = false
    @Volatile private var runAheadOk = false
    // Play time: counted while the game is on screen.
    private var playingSince = 0L
    private var messageUntil = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ErrorReport.install(this)
        prefs = Prefs(this)  // replaced once the game is known (per-game settings)
        game = Game(
            intent.getStringExtra(EXTRA_URI)!!,
            intent.getStringExtra(EXTRA_FILE)!!,
            GameSystem.fromName(intent.getStringExtra(EXTRA_SYSTEM))!!,
            intent.getLongExtra(EXTRA_SIZE, 0L),
        )
        prefs = Prefs(this, game)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        hideSystemBars()

        buildViews()
        gamepad = GamepadInput(onChange = ::pushInput, onMenu = ::showMenu, custom = PadMap.load(this)) { action, down ->
            if (!loaded) return@GamepadInput
            when (action) {
                PadMap.FAST_FORWARD -> if (down) toggleFastForward()
                PadMap.REWIND -> padListener.onRewind(down)
                PadMap.QUICK_SAVE -> if (down) quickState(save = true)
                PadMap.QUICK_LOAD -> if (down) quickState(save = false)
            }
        }
        gamepad.stickAsDpad = game.system != GameSystem.N3DS
        // TV (no touch screen): no on-screen buttons; the controller plays.
        if (!packageManager.hasSystemFeature(android.content.pm.PackageManager.FEATURE_TOUCHSCREEN))
            overlay.visibility = View.INVISIBLE

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
        tvMode = TvMode(this, { prefs.tvMode }) { on -> setOnTv(on) }
        devices = Devices(this, prefs) { micPermission.launch(android.Manifest.permission.RECORD_AUDIO) }
        // Azahar reads the accelerometer in g, the others in m/s².
        NativeBridge.setAccelInG(game.system == GameSystem.N3DS)
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
            // Remember our context: the TV's shares its textures.
            setEGLContextFactory(SharedGl.mainFactory)
            // 3DS games need depth and stencil in the core's own framebuffer,
            // not on screen; keep the window surface lean.
            setEGLConfigChooser(8, 8, 8, 0, 0, 0)
            preserveEGLContextOnPause = true
        }

        overlay = ControllerOverlay(this).apply {
            listener = padListener
            onSelectionChanged = { c -> editPanel.findViewWithTag<View>("size")?.isEnabled = c != null }
        }
        tvLabel = TextView(this).apply {
            text = "📺  Jogando na TV"
            setTextColor(0x99FFFFFF.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            visibility = View.GONE
        }
        root.addView(tvLabel, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER))
        recLabel = TextView(this).apply {
            text = "●  REC"
            setTextColor(0xFFFF3B30.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            setShadowLayer(4f, 0f, 1f, Color.BLACK)
            visibility = View.GONE
        }
        root.addView(recLabel, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
            topMargin = dp(10)
        })
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
        } else if (game.system.needsPath) {
            // The 3DS and PlayStation cores open the game themselves, by
            // path (gigabytes; .cue files naming their tracks): open it
            // where it is (needs all-files access).
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
        // PlayStation DualShock: RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1).
        if (game.system == GameSystem.PS1 && prefs.ps1DualShock) NativeBridge.setPortDevice((2 shl 8) or 5)
        applyCoreOptions()
        NativeBridge.setGame(romPath, File(saveDir, game.saveName + ".srm").path, romFd)
        return null
    }

    private val landscape get() = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    /** The TV took the game (true) or gave it back (false). */
    private fun setOnTv(on: Boolean) {
        onTv = on
        applyCoreOptions()
        // DS: screens stacked 256x384 (gap 0). 3DS default layout: 400x240 top,
        // 320x240 bottom centred below.
        val (phone, phonePicture, tv) = when (game.system) {
            GameSystem.NDS -> Triple(floatArrayOf(0f, 0.5f, 1f, 1f), true, floatArrayOf(0f, 0f, 1f, 0.5f))
            GameSystem.N3DS -> Triple(floatArrayOf(0.1f, 0.5f, 0.9f, 1f), true, floatArrayOf(0f, 0f, 1f, 0.5f))
            else -> Triple(null, false, null)
        }
        glView.queueEvent { NativeBridge.setTvMode(on, phone, phonePicture, tv) }
        tvLabel.visibility = if (on && !phonePicture) View.VISIBLE else View.GONE
        overlay.configure(game.system, landscape)
        applyDisplay()
        showMessage(if (on) "Jogo na TV — o celular é o controle" else "Jogo de volta no celular")
    }

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
            val layout = if (onTv) "top-bottom" else prefs.ndsLayout.let {
                if (it == "auto") (if (landscape) "left-right" else "top-bottom") else it
            }
            NativeBridge.setOption("melonds_screen_layout1", layout)
            if (onTv) NativeBridge.setOption("melonds_screen_gap", "0")
            return
        }
        when (game.system) {
            GameSystem.NES, GameSystem.SNES -> return
            GameSystem.MD, GameSystem.SMS, GameSystem.GG -> return
            GameSystem.PS1 -> {
                NativeBridge.setOption("pcsx_rearmed_drc", "enabled")
                NativeBridge.setOption("pcsx_rearmed_show_bios_bootlogo", "disabled")
                return
            }
            else -> {}
        }
        if (game.system != GameSystem.N3DS) {
            // Real BIOS when the player imported one, built-in otherwise.
            NativeBridge.setOption("vbam_usebios", if (Bios.anyFor(this, game.system)) "enabled" else "disabled")
            if (game.system == GameSystem.GB || game.system == GameSystem.GBA) {
                // Colours as the handheld's own screen showed them.
                val lcd = prefs.lcdColor
                NativeBridge.setOption("vbam_lcdfilter", if (lcd == "off") "disabled" else "enabled")
                if (lcd != "off") {
                    NativeBridge.setOption("vbam_lcdfilter_gba_panel", lcd)
                    NativeBridge.setOption("vbam_lcdfilter_gbc_panel", if (lcd == "NSO") "NSO" else "GBC")
                }
                NativeBridge.setOption("vbam_palettes", prefs.gbPalette)
                NativeBridge.setOption("vbam_interframeblending", prefs.frameBlend)
            }
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
        NativeBridge.setOption("citra_swap_screen", if (prefs.n3dsSwapScreens && !onTv) "Bottom" else "Top")
        val layout = if (onTv) "default" else prefs.n3dsLayout.let {
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
        overlay.style = prefs.buttonStyle
        overlay.globalScale = prefs.overlayScale
        overlay.haptics = prefs.haptics
        overlay.quickButtons = prefs.quickButtons
        overlay.micButton = prefs.microphone
        overlay.layoutScope = if (prefs.ownLayout) game.saveName else null
        overlay.analogStick = game.system == GameSystem.PS1 && prefs.ps1DualShock && prefs.ps1TouchStick
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
        if (::tvMode.isInitialized) tvMode.resume()
        if (modalCount == 0 && !overlay.editMode) NativeBridge.setPaused(false)
        if (loaded) NativeBridge.startAudio()
        playingSince = android.os.SystemClock.elapsedRealtime()
    }

    private fun countPlayTime() {
        if (playingSince == 0L) return
        val ms = android.os.SystemClock.elapsedRealtime() - playingSince
        playingSince = 0L
        if (loaded) Thread { PlayStats.addPlayTime(this, game, ms) }.start()
    }

    /**
     * On the GL thread: remember this exact moment, and its picture, for
     * "continue where you left off". Not during a link session (the other
     * side would not be there to continue with).
     */
    private fun saveResumePoint() {
        if (!prefs.resume || !PlayStats.canResume(game.system)) return
        if (::linkUi.isInitialized && linkUi.connected) return
        val f = PlayStats.resumeState(this, game)
        if (NativeBridge.saveState(f.path) == null)
            PlayStats.saveThumb(this, game, NativeBridge.screenshot())
    }

    override fun onPause() {
        resumed = false
        recorder?.takeIf { it.running }?.stop()
        countPlayTime()
        if (::devices.isInitialized) devices.stop()
        if (::tvMode.isInitialized) tvMode.pause()
        if (rendererSet) {
            NativeBridge.setPaused(true)
            NativeBridge.stopAudio()
            if (loaded) glView.queueEvent {
                NativeBridge.flushSave()
                // The app may be closed from the recent apps list while away.
                saveResumePoint()
            }
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
        // A video being recorded is finished first (the process ends after).
        val rec = recorder
        if (rec != null && rec.running) {
            rec.stop()
            showMessage("Salvando o vídeo…")
            Thread {
                rec.join(8000)
                runOnUiThread { quit() }
            }.start()
            return
        }
        NativeBridge.setPaused(true)
        NativeBridge.stopAudio()
        if (::linkUi.isInitialized) linkUi.release()
        if (::tvMode.isInitialized) tvMode.stop()
        countPlayTime()
        glView.queueEvent {
            NativeBridge.linkStop()
            saveResumePoint()
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
            if (loaded) {
                // A new phone context: the TV must share the new one.
                runOnUiThread { tvMode.refresh() }
            }
            if (!loaded) {
                loaded = true
                // Still on the GL thread: the core is ready for its codes.
                NativeBridge.setRewindEnabled(prefs.rewind)
                runAheadOk = NativeBridge.runAheadPossible()
                if (runAheadOk) NativeBridge.setRunAhead(prefs.runAhead)
                if (game.system != GameSystem.N3DS) cheats.apply()
                // RetroAchievements, when signed in.
                val raConsole = RetroAchievements.consoleId(game)
                if (raConsole != 0 && RetroAchievements.enabled(this@GameActivity)) {
                    NativeBridge.raStart(RetroAchievements.user(this@GameActivity),
                        RetroAchievements.token(this@GameActivity), raConsole)
                    achievementsOn = true
                    RetroAchievements.pump { !isFinishing && !isDestroyed }
                }
                // Continue where the player left off.
                val continued = prefs.resume && PlayStats.hasResume(this@GameActivity, game) &&
                    NativeBridge.loadState(PlayStats.resumeState(this@GameActivity, game).path) == null
                val canRewind = prefs.rewind && NativeBridge.rewindAvailable()
                runOnUiThread {
                    overlay.rewindAvailable = canRewind
                    applyDisplay()
                    if (resumed) NativeBridge.startAudio()
                    showMessage(if (continued) "Continuando de onde parou" else game.title)
                    tvMode.start()
                }
            }
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            NativeBridge.surfaceChanged(width, height)
        }

        override fun onDrawFrame(gl: GL10?) {
            if (!loaded) return
            NativeBridge.drawFrame()
            val rumble = NativeBridge.rumble()
            if (rumble != lastRumble) {
                lastRumble = rumble
                runOnUiThread { devices.onRumble(rumble) }
            }
            if (frameCounter % 10 == 0) {
                val wanted = NativeBridge.sensorsWanted()
                val mic = NativeBridge.micRate()
                runOnUiThread { if (resumed) devices.update(wanted, mic) }
            }
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
        if (::devices.isInitialized) devices.gamepadDeviceId = gamepad.lastDeviceId
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

    private val padListener by lazy { OverlayListener() }

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
            if (save && error == null) PlayStats.savePicture(File("$path.png"), NativeBridge.screenshot())
            runOnUiThread {
                showMessage(error ?: if (save) "Salvo rápido" else "Carregado o salvo rápido")
            }
        }
    }

    /** Settings that only this game uses; each falls back to the app's. */
    private fun gameSettings() {
        val own = PerGame.load(this, game)
        val relevant = PerGame.items.filter { item ->
            when (item.key) {
                "lcd_color", "frame_blend" -> game.system == GameSystem.GB || game.system == GameSystem.GBA
                "gb_palette" -> game.system == GameSystem.GB
                "run_ahead" -> runAheadOk
                else -> true
            }
        }
        fun label(item: PerGame.Item, value: String): String {
            val names = resources.getStringArray(item.names)
            val values = resources.getStringArray(item.values)
            return names.getOrNull(values.indexOf(value)) ?: value
        }
        val rows = relevant.map { item ->
            val v = own.optString(item.key)
            "${item.title}: " + if (v.isEmpty()) "padrão do app" else label(item, v)
        } + ("Botões na tela: " + (if (own.optBoolean("own_layout")) "layout só deste jogo" else "layout do console"))
        subDialog(MaterialAlertDialogBuilder(this)
            .setTitle("Configurações deste jogo")
            .setItems(rows.toTypedArray()) { _, which ->
                if (which == relevant.size) {
                    PerGame.set(this, game, "own_layout", if (own.optBoolean("own_layout")) null else true)
                    applyOverlaySettings()
                    showMessage(if (prefs.ownLayout) "Layout próprio: use Menu → Editar controles" else "Layout do console")
                    gameSettings()
                    return@setItems
                }
                val item = relevant[which]
                val names = arrayOf("Padrão do app") + resources.getStringArray(item.names)
                val values = resources.getStringArray(item.values)
                val current = values.indexOf(own.optString(item.key)) + 1
                subDialog(MaterialAlertDialogBuilder(this)
                    .setTitle(item.title)
                    .setSingleChoiceItems(names, current) { d, choice ->
                        d.dismiss()
                        PerGame.set(this, game, item.key, if (choice == 0) null else values[choice - 1])
                        applyGameSettings()
                        gameSettings()
                    })
            }
            .setPositiveButton(R.string.ok, null))
    }

    /** Puts changed settings into effect while playing. */
    private fun applyGameSettings() {
        applyDisplay()
        applyCoreOptions()
        overlay.style = prefs.buttonStyle
        if (fastForward) NativeBridge.setSpeed(prefs.fastForwardSpeed)
        val ahead = prefs.runAhead
        if (runAheadOk) glView.queueEvent { NativeBridge.setRunAhead(ahead) }
    }

    /** Multi-disc games (.m3u, .pbp with several discs). */
    private fun pickDisk() {
        glView.queueEvent {
            val disks = NativeBridge.disks()
            runOnUiThread {
                if (disks.isEmpty()) {
                    subDialog(MaterialAlertDialogBuilder(this)
                        .setTitle("Trocar disco")
                        .setMessage("Este jogo tem um disco só. Para jogos com vários CDs, crie um arquivo .m3u " +
                            "listando os discos (um nome de arquivo por linha) e abra o .m3u — ou use .pbp com todos os discos.")
                        .setPositiveButton(R.string.ok, null))
                    return@runOnUiThread
                }
                val current = disks.indexOfFirst { it.startsWith("*") }
                val labels = disks.map { it.removePrefix("*") }.toTypedArray()
                subDialog(MaterialAlertDialogBuilder(this)
                    .setTitle("Trocar disco")
                    .setSingleChoiceItems(labels, current) { d, which ->
                        d.dismiss()
                        glView.queueEvent {
                            val ok = NativeBridge.setDisk(which)
                            runOnUiThread { showMessage(if (ok) "💿 ${labels[which]}" else "Não deu para trocar o disco") }
                        }
                    })
            }
        }
    }

    /** Records the game's picture and sound to Movies/Cartucho. */
    private fun toggleRecording() {
        val current = recorder
        if (current != null && current.running) {
            current.stop()
            return
        }
        val rec = VideoRecorder(applicationContext, game.title)
        recorder = rec
        glView.queueEvent {
            NativeBridge.recStart()
            runOnUiThread {
                recLabel.visibility = View.VISIBLE
                showMessage("Gravando… Menu → Parar gravação")
                rec.start { message ->
                    runOnUiThread {
                        if (recorder === rec) recLabel.visibility = View.GONE
                        showMessage(message)
                    }
                }
            }
        }
    }

    private fun showAchievements() {
        val lines = NativeBridge.raList().map { it.split('|') }
        if (lines.isEmpty()) {
            subDialog(MaterialAlertDialogBuilder(this)
                .setTitle("Conquistas")
                .setMessage("Ainda carregando, ou este jogo não tem conquistas no RetroAchievements.")
                .setPositiveButton(R.string.ok, null))
            return
        }
        val earned = lines.count { it[0] == "1" }
        val items = lines.map { p ->
            val mark = if (p[0] == "1") "🏆" else "🔒"
            val progress = p.getOrNull(4)?.let { "  ($it)" } ?: ""
            "$mark ${p.getOrElse(1) { "" }} · ${p.getOrElse(3) { "0" }} pts$progress\n${p.getOrElse(2) { "" }}"
        }.toTypedArray()
        subDialog(MaterialAlertDialogBuilder(this)
            .setTitle("Conquistas: $earned de ${lines.size}")
            .setItems(items, null)
            .setPositiveButton(R.string.ok, null))
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
        if (game.system == GameSystem.N3DS && !onTv) items += getString(R.string.menu_screen_layout) to { pickLayout() }
        if (game.system == GameSystem.NDS && !onTv) items += getString(R.string.menu_screen_layout) to { pickNdsLayout() }
        if ((game.system == GameSystem.GBA || game.system == GameSystem.GB) && NativeBridge.linkSupported())
            items += (if (linkUi.connected) "Link cable: conectado" else "Link cable (rede)") to { linkUi.show() }
        if (game.system != GameSystem.N3DS) items += "Trapaças" + (if (cheats.list.any { it.enabled }) " (ativas)" else "") to {
            cheats.showDialog(this, { b -> modal.show(b) }) { glView.queueEvent { cheats.apply() } }
        }
        if (game.system == GameSystem.PS1) items += "💿 Trocar disco" to { pickDisk() }
        if (achievementsOn) items += "🏆 Conquistas" to { showAchievements() }
        items += (if (recorder?.running == true) "⏹ Parar gravação" else "🎬 Gravar vídeo") to { toggleRecording() }
        items += "Captura de tela" to { takeScreenshot() }
        items += "⚙️ Configurações deste jogo" to { gameSettings() }
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

    /** The six slots, each with the picture of its moment. */
    private fun pickSlot(save: Boolean) {
        val fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
        data class Slot(val n: Int, val label: String, val picture: android.graphics.Bitmap?)
        val slots = (1..6).map { slot ->
            val f = stateFile(slot)
            if (f.exists()) Slot(slot, getString(R.string.slot_used, slot, fmt.format(Date(f.lastModified()))),
                android.graphics.BitmapFactory.decodeFile(f.path + ".png"))
            else Slot(slot, getString(R.string.slot_empty, slot), null)
        }
        val adapter = object : android.widget.BaseAdapter() {
            override fun getCount() = slots.size
            override fun getItem(position: Int) = slots[position]
            override fun getItemId(position: Int) = position.toLong()
            override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
                val s = slots[position]
                val row = LinearLayout(this@GameActivity).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    setPadding(dp(20), dp(6), dp(20), dp(6))
                }
                val image = android.widget.ImageView(this@GameActivity).apply {
                    scaleType = android.widget.ImageView.ScaleType.FIT_CENTER
                    setBackgroundColor(0xFF07060C.toInt())
                    if (s.picture != null) setImageDrawable(android.graphics.drawable.BitmapDrawable(resources, s.picture)
                        .apply { isFilterBitmap = s.picture.width > 300 })
                }
                row.addView(image, LinearLayout.LayoutParams(dp(96), dp(72)))
                row.addView(TextView(this@GameActivity).apply {
                    text = s.label
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
                    setPadding(dp(14), 0, 0, 0)
                    setTextColor(androidx.core.content.ContextCompat.getColor(this@GameActivity, R.color.on_surface))
                })
                return row
            }
        }
        subDialog(MaterialAlertDialogBuilder(this)
            .setTitle(if (save) R.string.menu_save_state else R.string.menu_load_state)
            .setAdapter(adapter) { _, which ->
                val slot = which + 1
                val path = stateFile(slot).path
                if (!save && !File(path).exists()) return@setAdapter
                glView.queueEvent {
                    val error = if (save) NativeBridge.saveState(path) else NativeBridge.loadState(path)
                    if (save && error == null) PlayStats.savePicture(File("$path.png"), NativeBridge.screenshot())
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
