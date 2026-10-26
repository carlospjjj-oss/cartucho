package com.cartucho.emu

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice
import android.view.Surface
import android.view.WindowManager

/**
 * The phone's motion sensors, vibration and microphone, offered to the
 * emulators only when a game asks for them:
 *
 *  - sensors: listened to only while the core has them switched on
 *    (Kirby Tilt 'n' Tumble, WarioWare Twisted, Yoshi Topsy-Turvy, 3DS gyro
 *    aiming, Boktai's light sensor...), with axes turned to match the screen
 *    whatever way the phone is held;
 *  - vibration: the phone, or the gamepad in use when it has a motor
 *    (Pokémon Pinball, DS Rumble Pak games, 3DS...);
 *  - microphone: recorded only while the core listens (DS: while the MIC
 *    button is held; 3DS: when the game asks), after the player allowed it.
 *
 * update() runs on the UI thread a few times a second with what the core
 * wants; onRumble() whenever the requested strength changes.
 */
class Devices(
    private val context: Context,
    private val prefs: Prefs,
    private val askMicPermission: () -> Unit,
) : SensorEventListener {

    private val sensors = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val accel = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
    private val gyro = sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    private val light = sensors.getDefaultSensor(Sensor.TYPE_LIGHT)
    private var listening = 0  // bits currently registered

    private val a = FloatArray(3)
    private val g = FloatArray(3)

    /** Gamepad last used, to vibrate it instead of the phone. */
    var gamepadDeviceId = -1

    fun update(wanted: Int, micRate: Int) {
        updateSensors(if (prefs.motion) wanted else 0)
        updateMic(if (prefs.microphone) micRate else 0)
    }

    fun stop() {
        updateSensors(0)
        updateMic(0)
        onRumble(0)
    }

    // --- Sensors -------------------------------------------------------------

    private fun updateSensors(wanted: Int) {
        if (wanted == listening) return
        sensors.unregisterListener(this)
        listening = wanted
        val rate = SensorManager.SENSOR_DELAY_GAME
        if (wanted and 1 != 0 && accel != null) sensors.registerListener(this, accel, rate)
        if (wanted and 2 != 0 && gyro != null) sensors.registerListener(this, gyro, rate)
        if (wanted and 4 != 0 && light != null) sensors.registerListener(this, light, rate)
    }

    @Suppress("DEPRECATION")
    private fun rotation(): Int = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
        context.display?.rotation ?: Surface.ROTATION_0
    else (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.rotation

    /** Device axes -> screen axes for the current rotation (z unchanged). */
    private fun toScreen(v: FloatArray, out: FloatArray) {
        val x = v[0]
        val y = v[1]
        when (rotation()) {
            Surface.ROTATION_90 -> { out[0] = -y; out[1] = x }
            Surface.ROTATION_180 -> { out[0] = -x; out[1] = -y }
            Surface.ROTATION_270 -> { out[0] = y; out[1] = -x }
            else -> { out[0] = x; out[1] = y }
        }
        out[2] = v[2]
    }

    override fun onSensorChanged(e: SensorEvent) {
        when (e.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> toScreen(e.values, a)
            Sensor.TYPE_GYROSCOPE -> toScreen(e.values, g)
            Sensor.TYPE_LIGHT -> {
                NativeBridge.setLight(e.values[0])
                return
            }
            else -> return
        }
        NativeBridge.setMotion(a[0], a[1], a[2], g[0], g[1], g[2])
    }

    override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {}

    // --- Vibration -------------------------------------------------------------

    private var vibrating = false

    /** packed = strong << 16 | weak, 0..65535 each. */
    fun onRumble(packed: Int) {
        val strength = maxOf(packed ushr 16, packed and 0xffff)
        val vibrator = vibrator() ?: return
        if (strength == 0 || !prefs.vibration) {
            if (vibrating) vibrator.cancel()
            vibrating = false
            return
        }
        // A long one-shot at the requested strength; replaced on every
        // change and cancelled when the game stops rumbling.
        val amplitude = (strength / 257).coerceIn(1, 255)
        val effect = if (vibrator.hasAmplitudeControl())
            VibrationEffect.createOneShot(10_000, amplitude)
        else VibrationEffect.createOneShot(10_000, VibrationEffect.DEFAULT_AMPLITUDE)
        vibrator.vibrate(effect)
        vibrating = true
    }

    private fun vibrator(): Vibrator? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && gamepadDeviceId >= 0) {
            val pad = InputDevice.getDevice(gamepadDeviceId)?.vibratorManager?.defaultVibrator
            if (pad != null && pad.hasVibrator()) return pad
        }
        val phone = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as VibratorManager).defaultVibrator
        else @Suppress("DEPRECATION") (context.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator)
        return phone.takeIf { it.hasVibrator() }
    }

    // --- Microphone ---------------------------------------------------------------

    @Volatile private var recording = false
    private var recorder: Thread? = null
    private var askedPermission = false

    private fun updateMic(rate: Int) {
        if (rate > 0 && !recording) {
            if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                if (!askedPermission) {
                    askedPermission = true
                    askMicPermission()
                }
                return
            }
            startRecording()
        } else if (rate == 0 && recording) {
            recording = false
            recorder?.join(500)
            recorder = null
        }
    }

    @SuppressLint("MissingPermission")
    private fun startRecording() {
        recording = true
        recorder = Thread {
            // 48 kHz mono is supported everywhere; the native side resamples.
            val rate = 48000
            val min = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            val rec = try {
                AudioRecord(MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, maxOf(min, rate / 10 * 2))
            } catch (e: Exception) {
                null
            }
            if (rec == null || rec.state != AudioRecord.STATE_INITIALIZED) {
                rec?.release()
                recording = false
                return@Thread
            }
            val buf = ShortArray(rate / 100)  // 10 ms
            try {
                rec.startRecording()
                while (recording) {
                    val n = rec.read(buf, 0, buf.size)
                    if (n > 0) NativeBridge.pushMic(buf, n, rate)
                }
            } catch (e: Exception) {
            } finally {
                try { rec.stop() } catch (e: Exception) {}
                rec.release()
            }
        }.apply { name = "cartucho-mic"; start() }
    }

    /** The player answered the microphone permission prompt. */
    fun micPermissionAnswered() {
        askedPermission = true  // don't ask again this session either way
    }
}
