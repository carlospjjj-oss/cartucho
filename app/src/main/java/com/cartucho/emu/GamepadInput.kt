package com.cartucho.emu

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs

/**
 * Physical controllers and keyboards. Buttons are mapped by position, the way
 * Nintendo games expect: the bottom face button (Xbox A / PlayStation cross)
 * is B, the right one is A.
 */
class GamepadInput(
    private val onChange: () -> Unit,
    private val onMenu: () -> Unit,
    /** The player's own mapping (PadMap): key code -> Retro.* or a PadMap action. */
    custom: Map<Int, Int> = emptyMap(),
    /** A PadMap action pressed (true) or released (false). */
    private val onAction: (Int, Boolean) -> Unit = { _, _ -> },
) {
    // Defaults, minus buttons the player moved to another key, plus theirs.
    private val map: Map<Int, Int> = KEY_MAP.filterValues { it !in custom.values } + custom

    var buttons = 0
        private set
    var leftX = 0f
        private set
    var leftY = 0f
        private set
    var rightX = 0f
        private set
    var rightY = 0f
        private set
    /** True after any input from a real controller (to hide the overlay). */
    var gamepadSeen = false
        private set

    /** The controller last used, for its vibration motor. */
    var lastDeviceId = -1
        private set

    private var hatMask = 0
    private var axisDpadMask = 0
    private var keyMask = 0
    private var triggerMask = 0

    fun onKey(event: KeyEvent): Boolean {
        val code = event.keyCode
        val mapped = map[code]
        if (mapped != null && mapped < 0) {
            if (isGamepad(event.source)) {
                gamepadSeen = true
                lastDeviceId = event.deviceId
            }
            if (event.repeatCount == 0) {
                if (mapped == PadMap.MENU) { if (event.action == KeyEvent.ACTION_UP) onMenu() }
                else onAction(mapped, event.action == KeyEvent.ACTION_DOWN)
            }
            return true
        }
        if (mapped == null && (code == KeyEvent.KEYCODE_BUTTON_MODE || (code == KeyEvent.KEYCODE_BACK && isGamepad(event.source)))) {
            if (event.action == KeyEvent.ACTION_UP) onMenu()
            return true
        }
        val bit = mapped ?: return false
        if (isGamepad(event.source)) {
            gamepadSeen = true
            lastDeviceId = event.deviceId
        }
        val before = keyMask
        keyMask = if (event.action == KeyEvent.ACTION_DOWN) keyMask or (1 shl bit)
                  else keyMask and (1 shl bit).inv()
        if (keyMask != before) update()
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (event.action != MotionEvent.ACTION_MOVE || !isJoystick(event.source)) return false
        gamepadSeen = true
        lastDeviceId = event.deviceId
        leftX = dead(event.getAxisValue(MotionEvent.AXIS_X))
        leftY = dead(event.getAxisValue(MotionEvent.AXIS_Y))
        rightX = dead(event.getAxisValue(MotionEvent.AXIS_Z))
        rightY = dead(event.getAxisValue(MotionEvent.AXIS_RZ))

        val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        hatMask = dirMask(hx, hy, 0.5f)
        // The left stick also works as a D-pad (GBA has no stick).
        axisDpadMask = dirMask(leftX, leftY, 0.5f)

        val lt = maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
        val rt = maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
        triggerMask = (if (lt > 0.5f) 1 shl Retro.L2 else 0) or (if (rt > 0.5f) 1 shl Retro.R2 else 0)
        update()
        return true
    }

    /** The left stick also drives the D-pad, except on consoles with a stick. */
    var stickAsDpad = true

    private fun update() {
        buttons = keyMask or hatMask or triggerMask or (if (stickAsDpad) axisDpadMask else 0)
        onChange()
    }

    private fun dirMask(x: Float, y: Float, t: Float): Int {
        var m = 0
        if (x > t) m = m or (1 shl Retro.RIGHT)
        if (x < -t) m = m or (1 shl Retro.LEFT)
        if (y > t) m = m or (1 shl Retro.DOWN)
        if (y < -t) m = m or (1 shl Retro.UP)
        return m
    }

    private fun dead(v: Float) = if (abs(v) < 0.12f) 0f else v

    companion object {
        fun isGamepad(source: Int) =
            (source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

        fun isJoystick(source: Int) = (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

        private val KEY_MAP: Map<Int, Int> = mapOf(
            // Controllers (positional).
            KeyEvent.KEYCODE_BUTTON_A to Retro.B,
            KeyEvent.KEYCODE_BUTTON_B to Retro.A,
            KeyEvent.KEYCODE_BUTTON_X to Retro.Y,
            KeyEvent.KEYCODE_BUTTON_Y to Retro.X,
            KeyEvent.KEYCODE_BUTTON_L1 to Retro.L,
            KeyEvent.KEYCODE_BUTTON_R1 to Retro.R,
            KeyEvent.KEYCODE_BUTTON_L2 to Retro.L2,
            KeyEvent.KEYCODE_BUTTON_R2 to Retro.R2,
            KeyEvent.KEYCODE_BUTTON_THUMBL to Retro.L3,
            KeyEvent.KEYCODE_BUTTON_THUMBR to Retro.R3,
            KeyEvent.KEYCODE_BUTTON_START to Retro.START,
            KeyEvent.KEYCODE_BUTTON_SELECT to Retro.SELECT,
            KeyEvent.KEYCODE_DPAD_UP to Retro.UP,
            KeyEvent.KEYCODE_DPAD_DOWN to Retro.DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT to Retro.LEFT,
            KeyEvent.KEYCODE_DPAD_RIGHT to Retro.RIGHT,
            // Keyboard (Chromebooks, tablets with keyboards).
            KeyEvent.KEYCODE_X to Retro.A,
            KeyEvent.KEYCODE_Z to Retro.B,
            KeyEvent.KEYCODE_S to Retro.X,
            KeyEvent.KEYCODE_A to Retro.Y,
            KeyEvent.KEYCODE_Q to Retro.L,
            KeyEvent.KEYCODE_W to Retro.R,
            KeyEvent.KEYCODE_ENTER to Retro.START,
            KeyEvent.KEYCODE_SHIFT_RIGHT to Retro.SELECT,
            KeyEvent.KEYCODE_DEL to Retro.SELECT,
        )
    }
}
