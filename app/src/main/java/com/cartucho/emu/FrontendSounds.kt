package com.cartucho.emu

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.media.SoundPool
import android.net.Uri
import java.io.DataOutputStream
import java.io.File
import java.io.FileOutputStream
import kotlin.math.PI
import kotlin.math.exp
import kotlin.math.sin

/**
 * Sounds of the console-style screens: short clicks when moving, opening and
 * going back, and a soft background tune. Everything is synthesized here the
 * first time (no recorded audio ships with the app); the player can pick
 * their own music file instead.
 */
class FrontendSounds(private val context: Context) {
    enum class Sfx { MOVE, SELECT, BACK, LAUNCH }

    private val dir = File(context.cacheDir, "ui_sounds").apply { mkdirs() }
    private val pool = SoundPool.Builder()
        .setMaxStreams(4)
        .setAudioAttributes(AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_GAME)
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
            .build())
        .build()
    private val ids = HashMap<Sfx, Int>()
    private var music: MediaPlayer? = null
    @Volatile private var released = false

    var effectsOn = true
    var musicOn = true

    init {
        Thread {
            for (s in Sfx.entries) {
                val f = File(dir, "${s.name.lowercase()}_v1.wav")
                if (!f.exists()) writeWav(f, 44100, synthEffect(s, 44100))
                if (released) return@Thread
                ids[s] = pool.load(f.path, 1)
            }
        }.start()
    }

    fun play(s: Sfx) {
        if (!effectsOn) return
        val id = ids[s] ?: return
        val volume = if (s == Sfx.MOVE) 0.35f else 0.55f
        pool.play(id, volume, volume, 1, 0, 1f)
    }

    /** Starts (or resumes) the background music. */
    fun startMusic() {
        if (!musicOn || released) return
        music?.let { if (!it.isPlaying) it.start(); return }
        Thread {
            val custom = Prefs.raw(context).getString("ui_music_uri", null)
            val player = MediaPlayer()
            try {
                if (custom != null) player.setDataSource(context, Uri.parse(custom))
                else {
                    val f = File(dir, "music_v1.wav")
                    if (!f.exists()) writeWav(f, 22050, synthMusic(22050))
                    player.setDataSource(f.path)
                }
                player.setAudioAttributes(AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                player.isLooping = true
                player.prepare()
                player.setVolume(0.35f, 0.35f)
                synchronized(this) {
                    if (released || !musicOn) { player.release(); return@Thread }
                    music = player
                }
                player.start()
            } catch (e: Exception) {
                player.release()
            }
        }.start()
    }

    fun pauseMusic() {
        music?.takeIf { it.isPlaying }?.pause()
    }

    /** After the music choice changed. */
    fun restartMusic() {
        synchronized(this) {
            music?.release()
            music = null
        }
        startMusic()
    }

    fun release() {
        released = true
        synchronized(this) {
            music?.release()
            music = null
        }
        pool.release()
    }

    // --- Synthesis -------------------------------------------------------------

    private fun synthEffect(s: Sfx, rate: Int): ShortArray {
        fun tone(seconds: Double, f0: Double, f1: Double, square: Boolean, decay: Double): DoubleArray {
            val n = (seconds * rate).toInt()
            var phase = 0.0
            return DoubleArray(n) { i ->
                val t = i.toDouble() / n
                val f = f0 + (f1 - f0) * t
                phase += 2 * PI * f / rate
                val wave = if (square) (if (sin(phase) >= 0) 0.6 else -0.6) + 0.4 * sin(phase) else sin(phase)
                // Quick attack, exponential fade.
                wave * minOf(1.0, i / (rate * 0.003)) * exp(-t * decay)
            }
        }
        val parts: List<DoubleArray> = when (s) {
            Sfx.MOVE -> listOf(tone(0.035, 1320.0, 1320.0, true, 5.0))
            Sfx.SELECT -> listOf(tone(0.05, 880.0, 880.0, true, 2.0), tone(0.09, 1320.0, 1320.0, true, 4.0))
            Sfx.BACK -> listOf(tone(0.05, 990.0, 990.0, true, 2.0), tone(0.09, 660.0, 660.0, true, 4.0))
            Sfx.LAUNCH -> listOf(
                tone(0.06, 523.0, 523.0, true, 1.5), tone(0.06, 659.0, 659.0, true, 1.5),
                tone(0.06, 784.0, 784.0, true, 1.5), tone(0.25, 1047.0, 1047.0, true, 3.5))
        }
        val all = parts.flatMap { it.asList() }
        return ShortArray(all.size) { (all[it] * 0.5 * Short.MAX_VALUE).toInt().toShort() }
    }

    /**
     * A calm 4-chord loop (Am F C G), 8 bars at 84 bpm: soft pads, a plucked
     * arpeggio and a slow bass. Seamless when looped.
     */
    private fun synthMusic(rate: Int): ShortArray {
        val bpm = 84.0
        val beat = 60.0 / bpm
        val bars = 8
        val total = (bars * 4 * beat * rate).toInt()
        val out = DoubleArray(total)
        fun midi(n: Int) = 440.0 * Math.pow(2.0, (n - 69) / 12.0)
        val chords = listOf(listOf(57, 60, 64), listOf(53, 57, 60), listOf(48, 52, 55), listOf(55, 59, 62))
        // Pads: each chord held for a bar, gently swelling.
        for (bar in 0 until bars) {
            val chord = chords[bar % 4]
            val start = (bar * 4 * beat * rate).toInt()
            val len = (4 * beat * rate).toInt()
            for (note in chord) {
                val f = midi(note)
                for (i in 0 until len) {
                    val t = i.toDouble() / len
                    val env = sin(PI * t).let { it * it }
                    val x = 2 * PI * f * i / rate
                    out[(start + i) % total] += 0.05 * env * (sin(x) + 0.5 * sin(x * 1.003) + 0.25 * sin(x * 2.0))
                }
            }
            // Bass on beats 1 and 3.
            for (b in listOf(0, 2)) {
                val f = midi(chord[0] - 12)
                val s0 = start + (b * beat * rate).toInt()
                val n = (1.8 * beat * rate).toInt()
                for (i in 0 until n) {
                    val env = exp(-3.0 * i / n) * minOf(1.0, i / (rate * 0.01))
                    out[(s0 + i) % total] += 0.09 * env * sin(2 * PI * f * i / rate)
                }
            }
            // Arpeggio in eighths, an octave up.
            for (k in 0 until 8) {
                val note = chord[listOf(0, 1, 2, 1, 0, 2, 1, 2)[k]] + 12
                val f = midi(note)
                val s0 = start + (k * beat / 2 * rate).toInt()
                val n = (beat * 0.9 * rate).toInt()
                for (i in 0 until n) {
                    val env = exp(-6.0 * i / n) * minOf(1.0, i / (rate * 0.004))
                    val x = 2 * PI * f * i / rate
                    // Soft triangle-ish pluck.
                    out[(s0 + i) % total] += 0.045 * env * (sin(x) - sin(3 * x) / 9 + sin(5 * x) / 25)
                }
            }
        }
        val peak = out.maxOf { kotlin.math.abs(it) }.coerceAtLeast(1e-6)
        return ShortArray(total) { (out[it] / peak * 0.8 * Short.MAX_VALUE).toInt().toShort() }
    }

    private fun writeWav(f: File, rate: Int, pcm: ShortArray) {
        val tmp = File(f.path + ".tmp")
        DataOutputStream(FileOutputStream(tmp).buffered()).use { out ->
            fun le32(v: Int) { out.write(v and 0xff); out.write(v shr 8 and 0xff); out.write(v shr 16 and 0xff); out.write(v shr 24 and 0xff) }
            fun le16(v: Int) { out.write(v and 0xff); out.write(v shr 8 and 0xff) }
            val bytes = pcm.size * 2
            out.writeBytes("RIFF"); le32(36 + bytes); out.writeBytes("WAVE")
            out.writeBytes("fmt "); le32(16); le16(1); le16(1); le32(rate); le32(rate * 2); le16(2); le16(16)
            out.writeBytes("data"); le32(bytes)
            for (s in pcm) le16(s.toInt())
        }
        tmp.renameTo(f)
    }
}
