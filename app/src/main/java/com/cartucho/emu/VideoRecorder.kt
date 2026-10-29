package com.cartucho.emu

import android.content.ContentValues
import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.net.Uri
import android.os.Build
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Turns what recorder.cpp collects into an MP4 (H.264 + AAC) in
 * Movies/Cartucho, with the phone's hardware encoders. One instance per video.
 */
class VideoRecorder(private val context: Context, private val title: String) {
    @Volatile private var stopRequested = false
    @Volatile var running = false
        private set
    private var thread: Thread? = null

    /** Starts the encoder thread; call after NativeBridge.recStart(). */
    fun start(onDone: (String) -> Unit) {
        running = true
        thread = Thread({
            val message = try {
                encode()
            } catch (e: Exception) {
                "Não deu para gravar o vídeo (${e.message ?: e.javaClass.simpleName})"
            } finally {
                running = false
            }
            onDone(message)
        }, "video").apply { start() }
    }

    fun stop() {
        stopRequested = true
        NativeBridge.recStop(0)
    }

    /** Waits for the file to be finished (at most [ms]). */
    fun join(ms: Long) {
        thread?.join(ms)
    }

    private fun align16(v: Int) = (v + 15) / 16 * 16

    private fun encode(): String {
        // Size of the game's picture, once the first one arrives.
        var size = NativeBridge.recFirstSize(0)
        val waitUntil = System.currentTimeMillis() + 3000
        while (size[0] == 0 && !stopRequested && System.currentTimeMillis() < waitUntil) {
            Thread.sleep(20)
            size = NativeBridge.recFirstSize(0)
        }
        if (size[0] == 0) return "Nada para gravar"
        // Whole-number scale up to about 720 lines (crisp pixels), at most 4x.
        val scale = maxOf(1, minOf(4, 720 / size[1]))
        val width = align16(size[0] * scale)
        val height = align16(size[1] * scale)

        val video = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        video.configure(MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
            setInteger(MediaFormat.KEY_BIT_RATE, (width * height * 6).coerceIn(2_000_000, 16_000_000))
            setInteger(MediaFormat.KEY_FRAME_RATE, 60)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        }, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val audio = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
        audio.configure(MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, 48000, 2).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, 160_000)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384)
        }, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)

        val (uri, pfd) = createFile()
        val muxer = MediaMuxer(pfd.fileDescriptor, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        video.start()
        audio.start()

        var videoTrack = -1
        var audioTrack = -1
        var muxing = false
        val pending = ArrayList<Triple<Boolean, ByteBuffer, MediaCodec.BufferInfo>>()
        var videoEos = false
        var audioEos = false
        var videoEosSent = false
        var audioEosSent = false
        var lastVideoPts = -1L
        var lastAudioPts = 0L
        var frames = 0
        val pts = LongArray(1)
        val info = MediaCodec.BufferInfo()
        val scratch = ByteBuffer.allocateDirect(16384).order(ByteOrder.nativeOrder())

        fun drain(codec: MediaCodec, isVideo: Boolean): Boolean {
            while (true) {
                val index = codec.dequeueOutputBuffer(info, 0)
                when {
                    index == MediaCodec.INFO_TRY_AGAIN_LATER -> return false
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        if (isVideo) videoTrack = muxer.addTrack(codec.outputFormat)
                        else audioTrack = muxer.addTrack(codec.outputFormat)
                        if (videoTrack >= 0 && audioTrack >= 0 && !muxing) {
                            muxer.start()
                            muxing = true
                            for ((v, buf, i) in pending) muxer.writeSampleData(if (v) videoTrack else audioTrack, buf, i)
                            pending.clear()
                        }
                    }
                    index >= 0 -> {
                        val out = codec.getOutputBuffer(index)!!
                        val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                        if (info.size > 0 && info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
                            out.position(info.offset)
                            out.limit(info.offset + info.size)
                            if (muxing) {
                                muxer.writeSampleData(if (isVideo) videoTrack else audioTrack, out, info)
                            } else {
                                val copy = ByteBuffer.allocate(info.size)
                                copy.put(out)
                                copy.flip()
                                val i = MediaCodec.BufferInfo()
                                i.set(0, info.size, info.presentationTimeUs, info.flags)
                                pending += Triple(isVideo, copy, i)
                            }
                        }
                        codec.releaseOutputBuffer(index, false)
                        if (eos) return true
                    }
                }
            }
        }

        while (!(videoEos && audioEos)) {
            val stopping = stopRequested
            // Pictures. This thread is the only one taking them, so one seen
            // waiting is still there to fill.
            if (!videoEosSent && (NativeBridge.recHasVideo(0) || stopping)) {
                val index = video.dequeueInputBuffer(2000)
                if (index >= 0) {
                    val t = if (NativeBridge.recHasVideo(0)) {
                        val p = video.getInputImage(index)!!.planes
                        NativeBridge.recFillVideo(0, p[0].buffer, p[0].rowStride, p[1].buffer, p[2].buffer,
                            p[1].rowStride, p[1].pixelStride, width, height)
                    } else -1L
                    if (t >= 0) {
                        video.queueInputBuffer(index, 0, width * height * 3 / 2, maxOf(t, lastVideoPts + 1), 0)
                        lastVideoPts = maxOf(t, lastVideoPts + 1)
                        frames++
                    } else {
                        video.queueInputBuffer(index, 0, 0, lastVideoPts + 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                        videoEosSent = true
                    }
                }
            }
            // Sound.
            if (!audioEosSent) {
                val available = NativeBridge.recAudioAvailable()
                if (available >= 1024 || stopping) {
                    val index = audio.dequeueInputBuffer(2000)
                    if (index >= 0) {
                        val buffer = audio.getInputBuffer(index)!!
                        val maxFrames = minOf(buffer.capacity() / 4, 4096)
                        scratch.clear()
                        val n = if (available > 0) NativeBridge.recFillAudio(scratch, minOf(maxFrames, scratch.capacity() / 4), pts) else 0
                        if (n > 0) {
                            scratch.limit(n * 4)
                            buffer.clear()
                            buffer.put(scratch)
                            audio.queueInputBuffer(index, 0, n * 4, pts[0], 0)
                            lastAudioPts = pts[0]
                        } else {
                            audio.queueInputBuffer(index, 0, 0, lastAudioPts + 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            audioEosSent = true
                        }
                    }
                }
            }
            if (!videoEos) videoEos = drain(video, true)
            if (!audioEos) audioEos = drain(audio, false)
            if (!stopping && !NativeBridge.recHasVideo(0) && NativeBridge.recAudioAvailable() < 1024) Thread.sleep(5)
        }

        video.stop(); video.release()
        audio.stop(); audio.release()
        val ok = muxing && frames > 0
        if (muxing) muxer.stop()
        muxer.release()
        pfd.close()
        return if (ok) {
            finishFile(uri)
            "Vídeo salvo em Filmes/Cartucho"
        } else {
            context.contentResolver.delete(uri, null, null)
            "Vídeo muito curto: não foi salvo"
        }
    }

    private fun createFile(): Pair<Uri, ParcelFileDescriptor> {
        val name = title.replace(Regex("[\\\\/:*?\"<>|]"), "_").take(60) + "-" +
            SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date()) + ".mp4"
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, name)
            put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/Cartucho")
                put(MediaStore.Video.Media.IS_PENDING, 1)
            }
        }
        val collection = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        else MediaStore.Video.Media.EXTERNAL_CONTENT_URI
        val uri = context.contentResolver.insert(collection, values) ?: throw IllegalStateException("sem espaço")
        val pfd = context.contentResolver.openFileDescriptor(uri, "rw") ?: throw IllegalStateException("arquivo")
        return uri to pfd
    }

    private fun finishFile(uri: Uri) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            context.contentResolver.update(uri, ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }, null, null)
        }
    }
}
