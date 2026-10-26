package com.cartucho.emu

import android.app.Presentation
import android.content.Context
import android.hardware.display.DisplayManager
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Display
import android.view.WindowManager
import javax.microedition.khronos.egl.EGL10
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.egl.EGLContext
import javax.microedition.khronos.egl.EGLDisplay
import javax.microedition.khronos.opengles.GL10

/**
 * The GL context of the phone's game view, remembered so the TV's context can
 * share its objects (textures, fences) and draw the same picture without
 * copying it.
 */
object SharedGl {
    @Volatile var context: EGLContext? = null
        private set
    /** Bumped every time the phone gets a new context (TV must follow). */
    @Volatile var generation = 0
        private set

    private const val EGL_CONTEXT_CLIENT_VERSION = 0x3098

    /** For the phone's GLSurfaceView: a GLES 3 context, remembered. */
    val mainFactory = object : GLSurfaceView.EGLContextFactory {
        override fun createContext(egl: EGL10, display: EGLDisplay, config: EGLConfig): EGLContext {
            val ctx = egl.eglCreateContext(display, config, EGL10.EGL_NO_CONTEXT,
                intArrayOf(EGL_CONTEXT_CLIENT_VERSION, 3, EGL10.EGL_NONE))
            context = ctx
            generation++
            return ctx
        }

        override fun destroyContext(egl: EGL10, display: EGLDisplay, ctx: EGLContext) {
            if (context == ctx) context = null
            egl.eglDestroyContext(display, ctx)
        }
    }

    /** For the TV's GLSurfaceView: a context sharing the phone's. */
    val tvFactory = object : GLSurfaceView.EGLContextFactory {
        override fun createContext(egl: EGL10, display: EGLDisplay, config: EGLConfig): EGLContext =
            egl.eglCreateContext(display, config, context ?: EGL10.EGL_NO_CONTEXT,
                intArrayOf(EGL_CONTEXT_CLIENT_VERSION, 3, EGL10.EGL_NONE))

        override fun destroyContext(egl: EGL10, display: EGLDisplay, ctx: EGLContext) {
            egl.eglDestroyContext(display, ctx)
        }
    }
}

/** The game on the TV: a full-screen GL view drawing the shared picture. */
class TvPresentation(context: Context, display: Display) : Presentation(context, display) {
    private lateinit var view: GLSurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        view = GLSurfaceView(context).apply {
            setEGLContextClientVersion(3)
            setEGLConfigChooser(8, 8, 8, 0, 0, 0)
            setEGLContextFactory(SharedGl.tvFactory)
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) = NativeBridge.tvSurfaceCreated()
                override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) =
                    NativeBridge.tvSurfaceChanged(width, height)
                override fun onDrawFrame(gl: GL10?) = NativeBridge.tvDrawFrame()
            })
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }
        setContentView(view)
    }

    fun pause() = view.onPause()
    fun resume() = view.onResume()
}

/**
 * Watches for a TV (HDMI, Chromecast / Smart View screen casting, Miracast)
 * and moves the game there while one is connected.
 */
class TvMode(
    private val context: Context,
    private val enabled: () -> Boolean,
    /** Called on the UI thread with true when the TV takes the game, false when it leaves. */
    private val onChange: (Boolean) -> Unit,
) : DisplayManager.DisplayListener {

    private val displays = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val main = Handler(Looper.getMainLooper())
    private var presentation: TvPresentation? = null
    private var shownGeneration = -1
    val active get() = presentation != null

    fun start() {
        displays.registerDisplayListener(this, main)
        refresh()
    }

    fun stop() {
        displays.unregisterDisplayListener(this)
        hide()
    }

    fun pause() = presentation?.pause()
    fun resume() = presentation?.resume()

    /** Show or hide according to the displays and the setting. */
    fun refresh() {
        val tv = if (enabled()) displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION).firstOrNull() else null
        val current = presentation
        when {
            tv == null || SharedGl.context == null -> hide()
            current != null && current.display.displayId == tv.displayId &&
                shownGeneration == SharedGl.generation -> {}
            else -> {
                hide()
                val p = TvPresentation(context, tv)
                try {
                    p.show()
                } catch (e: WindowManager.InvalidDisplayException) {
                    return
                }
                presentation = p
                shownGeneration = SharedGl.generation
                p.setOnDismissListener {
                    if (presentation === p) {
                        presentation = null
                        onChange(false)
                    }
                }
                onChange(true)
            }
        }
    }

    private fun hide() {
        val p = presentation ?: return
        presentation = null
        p.dismiss()
        onChange(false)
    }

    override fun onDisplayAdded(displayId: Int) = refresh()
    override fun onDisplayRemoved(displayId: Int) = refresh()
    override fun onDisplayChanged(displayId: Int) = refresh()
}
