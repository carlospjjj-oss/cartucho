package com.cartucho.emu

/**
 * The native frontend (app/src/main/cpp). One game per process; see
 * GameActivity. Functions marked "GL thread" must run on the GLSurfaceView
 * render thread (queueEvent or the renderer callbacks).
 */
object NativeBridge {
    init {
        System.loadLibrary("cartucho")
    }

    /** Loads a core library. Returns null on success, else the error. */
    external fun open(corePath: String, systemDir: String, saveDir: String): String?
    /** fd: an open descriptor holding the game (-1 to open romPath instead). */
    external fun setGame(romPath: String, savePath: String, fd: Int)
    external fun setOption(key: String, value: String)
    /** Alternating key, "Description; a|b|c", current value. */
    external fun options(): Array<String>

    /** GL thread. Loads the game on first call. Null on success. */
    external fun surfaceCreated(): String?
    external fun surfaceChanged(width: Int, height: Int)
    external fun drawFrame()

    external fun startAudio()
    external fun stopAudio()
    external fun setPaused(paused: Boolean)
    external fun setSpeed(speed: Float)

    /** Bits are RetroPad ids (Retro.B, Retro.A ...). */
    external fun setButtons(mask: Int)
    external fun setAnalog(stick: Int, x: Float, y: Float)
    external fun touch(x: Float, y: Float, pressed: Boolean)

    /** GL thread. Null on success. */
    external fun saveState(path: String): String?
    /** GL thread. Null on success. */
    external fun loadState(path: String): String?
    external fun reset()
    external fun flushSave()
    external fun close()

    external fun setDisplay(filter: Int, integerScale: Boolean, verticalAlign: Int, bottomReserve: Float)
    /** [x, y, w, h] of the picture on the surface. */
    external fun viewport(): IntArray
    external fun measuredFps(): Float
    external fun takeMessage(): String?
    external fun shutdownRequested(): Boolean
    external fun usesHardwareRender(): Boolean

    /** Native crashes dump the recent log into this file (opened now). */
    external fun setCrashFile(path: String)
    /** What the frontend and the core logged recently. */
    external fun recentLog(): String

    // Rewind, cheats, screenshots.
    external fun rewindAvailable(): Boolean
    external fun setRewindEnabled(on: Boolean)
    /** Hold to go back in time (any thread). */
    external fun setRewinding(on: Boolean)
    /** GL thread. */
    external fun cheatReset()
    /** GL thread. */
    external fun cheatSet(index: Int, enabled: Boolean, code: String)
    /** GL thread. [width, height, ARGB pixels...] or null. */
    external fun screenshot(): IntArray?

    // Motion sensors, rumble, microphone.
    /** Bits the core switched on: 1 accelerometer, 2 gyroscope, 4 light. */
    external fun sensorsWanted(): Int
    external fun setMotion(ax: Float, ay: Float, az: Float, gx: Float, gy: Float, gz: Float)
    external fun setLight(lux: Float)
    external fun setAccelInG(on: Boolean)
    /** strong << 16 | weak. */
    external fun rumble(): Int
    /** Rate the core listens at, 0 when the microphone is off. */
    external fun micRate(): Int
    external fun pushMic(samples: ShortArray, count: Int, rate: Int)

    // Network link (GBA / GB). linkStart and linkStop: GL thread.
    external fun linkSupported(): Boolean
    /** mode: 0 GBA cable, 1 GBA wireless adapter, 2 Game Boy cable. */
    external fun linkStart(mode: Int, server: Boolean, players: Int, host: String, port: Int)
    external fun linkStop()
    /** -1 off, 0 connected, 1 failed, 2 connecting. */
    external fun linkState(): Int
    external fun linkMessage(): String?
}

/** RetroPad button ids (bit positions in NativeBridge.setButtons). */
object Retro {
    const val B = 0
    const val Y = 1
    const val SELECT = 2
    const val START = 3
    const val UP = 4
    const val DOWN = 5
    const val LEFT = 6
    const val RIGHT = 7
    const val A = 8
    const val X = 9
    const val L = 10
    const val R = 11
    const val L2 = 12
    const val R2 = 13
    const val L3 = 14
    const val R3 = 15
}
