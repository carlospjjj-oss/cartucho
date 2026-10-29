// Cartucho - records the game's own picture and sound for a video.
//
// The emulation thread pushes every emulated frame's picture and the core's
// audio; the app's encoder thread (MediaCodec, in Kotlin) pulls them out
// converted to what the encoders take: YUV 4:2:0 scaled up to a fixed canvas,
// and 48 kHz stereo PCM. Timestamps come from the emulated time, so the video
// plays at the game's speed whatever the phone managed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>


namespace cartucho {

class Recorder {
public:
    static constexpr int kAudioRate = 48000;

    // Emulation thread.
    void Start(double fps, double sample_rate);
    void Stop();
    bool active() const { return active_; }
    // One emulated frame's picture (RGBA, top row first). `frames` is how many
    // emulated frames it stands for (1 normally).
    void PushVideo(const uint8_t* rgba, unsigned width, unsigned height, int frames = 1);
    // Core audio (stereo int16 at the core's rate); any thread.
    void PushAudio(const int16_t* stereo, size_t frames);

    // Encoder thread.
    // Size of the first picture (0 until one arrived).
    void FirstSize(int* width, int* height);
    bool HasVideo();
    // Converts the oldest picture into the encoder's planes (canvas out_w x
    // out_h, the picture scaled with nearest neighbour, centred). Returns its
    // time in microseconds, or -1 if there was none.
    int64_t FillVideo(uint8_t* y, int y_stride, uint8_t* u, uint8_t* v, int uv_stride, int uv_pixel_stride,
                      int out_w, int out_h);
    // 48 kHz stereo frames ready.
    size_t AudioAvailable();
    // Up to `max_frames` of them into `out`; their time in *pts_us.
    size_t FillAudio(int16_t* out, size_t max_frames, int64_t* pts_us);

private:
    struct Picture {
        std::vector<uint8_t> rgba;
        unsigned width, height;
        int64_t pts_us;
    };

    std::mutex mutex_;
    bool active_ = false;
    double fps_ = 60.0;
    double in_rate_ = 48000.0;
    int64_t frames_ = 0;              // emulated frames so far
    std::deque<Picture> pictures_;
    std::vector<Picture> spare_;      // reused buffers
    int first_w_ = 0, first_h_ = 0;

    std::deque<int16_t> raw_;         // core audio waiting to be resampled
    std::vector<int16_t> out_;        // resampled, waiting for the encoder
    size_t out_read_ = 0;
    double resample_pos_ = 0.0;
    int16_t last_[2] = {0, 0};
    int64_t audio_frames_out_ = 0;    // 48 kHz frames handed over so far
    void ResampleLocked();
};

}  // namespace cartucho
