package com.cartucho.emu

import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import java.io.File

/**
 * The library's moving preview of a game: the last seconds played, kept by
 * recorder.cpp in ring mode, written as a short silent MP4 when leaving.
 */
object PreviewClip {
    fun file(context: Context, game: Game): File =
        File(context.filesDir, "previews/${game.system.name}/${game.saveName}.mp4")

    private fun align16(v: Int) = (v + 15) / 16 * 16

    /**
     * Encodes what the ring holds (call after NativeBridge.recStop(1)).
     * Blocking, about a second; returns true when a clip was written.
     */
    fun write(context: Context, game: Game): Boolean {
        val size = NativeBridge.recFirstSize(1)
        if (size[0] == 0 || !NativeBridge.recHasVideo(1)) return false
        // Whole-number scale to about 480 lines: crisp pixels, small file.
        val scale = maxOf(1, minOf(4, 480 / size[1]))
        val width = align16(size[0] * scale)
        val height = align16(size[1] * scale)
        val out = file(context, game)
        out.parentFile?.mkdirs()
        val tmp = File(out.path + ".tmp")
        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        var muxer: MediaMuxer? = null
        var frames = 0
        try {
            codec.configure(MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
                setInteger(MediaFormat.KEY_BIT_RATE, 1_500_000)
                setInteger(MediaFormat.KEY_FRAME_RATE, 10)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            }, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()
            val m = MediaMuxer(tmp.path, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            muxer = m
            var track = -1
            var base = -1L
            var last = -1L
            var inputDone = false
            val info = MediaCodec.BufferInfo()
            val deadline = System.currentTimeMillis() + 8000
            while (System.currentTimeMillis() < deadline) {
                if (!inputDone) {
                    val index = codec.dequeueInputBuffer(5000)
                    if (index >= 0) {
                        if (NativeBridge.recHasVideo(1)) {
                            val p = codec.getInputImage(index)!!.planes
                            val t = NativeBridge.recFillVideo(1, p[0].buffer, p[0].rowStride, p[1].buffer, p[2].buffer,
                                p[1].rowStride, p[1].pixelStride, width, height)
                            if (base < 0) base = t
                            val pts = maxOf(t - base, last + 1)
                            last = pts
                            codec.queueInputBuffer(index, 0, width * height * 3 / 2, pts, 0)
                            frames++
                        } else {
                            codec.queueInputBuffer(index, 0, 0, last + 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        }
                    }
                }
                val o = codec.dequeueOutputBuffer(info, 5000)
                if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    track = m.addTrack(codec.outputFormat)
                    m.start()
                } else if (o >= 0) {
                    val buf = codec.getOutputBuffer(o)!!
                    if (track >= 0 && info.size > 0 && info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
                        buf.position(info.offset)
                        buf.limit(info.offset + info.size)
                        m.writeSampleData(track, buf, info)
                    }
                    codec.releaseOutputBuffer(o, false)
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                }
            }
            if (track >= 0) m.stop()
        } catch (e: Exception) {
            frames = 0
        } finally {
            runCatching { codec.stop() }
            codec.release()
            runCatching { muxer?.release() }
        }
        if (frames < 5) {
            tmp.delete()
            return false
        }
        return tmp.renameTo(out)
    }
}
