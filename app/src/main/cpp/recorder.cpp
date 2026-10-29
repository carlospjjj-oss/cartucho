#include "recorder.h"

#include <algorithm>
#include <cstring>

namespace cartucho {

namespace {

// Keep at most this many pictures waiting: if the encoder falls behind, the
// newest pictures are dropped (the video skips) rather than memory growing.
constexpr size_t kMaxPictures = 10;
// And at most ~2 s of audio.
constexpr size_t kMaxRawFrames = 96000;

inline uint8_t Clamp(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

}  // namespace

void Recorder::Start(double fps, double sample_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    fps_ = fps > 1.0 ? fps : 60.0;
    in_rate_ = sample_rate > 1000.0 ? sample_rate : 48000.0;
    frames_ = 0;
    pictures_.clear();
    first_w_ = first_h_ = 0;
    raw_.clear();
    out_.clear();
    out_read_ = 0;
    resample_pos_ = 0.0;
    last_[0] = last_[1] = 0;
    audio_frames_out_ = 0;
    active_ = true;
}

void Recorder::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
}

void Recorder::PushVideo(const uint8_t* rgba, unsigned width, unsigned height, int frames) {
    if (!active_ || !rgba || !width || !height)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_)
        return;
    const int64_t pts = static_cast<int64_t>(frames_ * 1000000.0 / fps_);
    frames_ += frames;
    if (!first_w_) {
        first_w_ = static_cast<int>(width);
        first_h_ = static_cast<int>(height);
    }
    if (pictures_.size() >= kMaxPictures)
        return;
    Picture p;
    if (!spare_.empty()) {
        p = std::move(spare_.back());
        spare_.pop_back();
    }
    p.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    p.width = width;
    p.height = height;
    p.pts_us = pts;
    pictures_.push_back(std::move(p));
}

void Recorder::PushAudio(const int16_t* stereo, size_t frames) {
    if (!active_)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || raw_.size() / 2 + frames > kMaxRawFrames)
        return;
    raw_.insert(raw_.end(), stereo, stereo + frames * 2);
}

void Recorder::FirstSize(int* width, int* height) {
    std::lock_guard<std::mutex> lock(mutex_);
    *width = first_w_;
    *height = first_h_;
}

bool Recorder::HasVideo() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pictures_.empty();
}

int64_t Recorder::FillVideo(uint8_t* y, int y_stride, uint8_t* u, uint8_t* v, int uv_stride,
                            int uv_pixel_stride, int out_w, int out_h) {
    Picture p;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pictures_.empty())
            return -1;
        p = std::move(pictures_.front());
        pictures_.pop_front();
    }
    // Fit the picture in the canvas: the biggest whole-number scale that fits
    // (crisp pixels), or a fractional one for pictures bigger than it.
    const double fit = std::min(static_cast<double>(out_w) / p.width, static_cast<double>(out_h) / p.height);
    const double scale = fit >= 1.0 ? std::max(1.0, static_cast<double>(static_cast<int>(fit))) : fit;
    const int dw = std::max(1, static_cast<int>(p.width * scale));
    const int dh = std::max(1, static_cast<int>(p.height * scale));
    const int ox = (out_w - dw) / 2;
    const int oy = (out_h - dh) / 2;

    std::vector<int> col(static_cast<size_t>(out_w), -1);
    for (int x = 0; x < out_w; x++) {
        const int sx = x - ox;
        if (sx >= 0 && sx < dw)
            col[static_cast<size_t>(x)] = std::min(static_cast<int>(p.width) - 1, static_cast<int>(sx / scale));
    }
    // BT.601 limited range.
    for (int row = 0; row < out_h; row++) {
        const int sy = row - oy;
        uint8_t* yrow = y + static_cast<size_t>(row) * y_stride;
        if (sy < 0 || sy >= dh) {
            std::memset(yrow, 16, static_cast<size_t>(out_w));
            continue;
        }
        const uint8_t* src = p.rgba.data() + static_cast<size_t>(std::min(static_cast<int>(p.height) - 1,
                                                                         static_cast<int>(sy / scale))) * p.width * 4;
        for (int x = 0; x < out_w; x++) {
            const int c = col[static_cast<size_t>(x)];
            if (c < 0) {
                yrow[x] = 16;
                continue;
            }
            const uint8_t* s = src + c * 4;
            yrow[x] = Clamp(((66 * s[0] + 129 * s[1] + 25 * s[2] + 128) >> 8) + 16);
        }
    }
    for (int row = 0; row < out_h / 2; row++) {
        uint8_t* urow = u + static_cast<size_t>(row) * uv_stride;
        uint8_t* vrow = v + static_cast<size_t>(row) * uv_stride;
        const int sy = row * 2 - oy;
        for (int x = 0; x < out_w / 2; x++) {
            uint8_t cu = 128, cv = 128;
            const int c = col[static_cast<size_t>(x * 2)];
            if (sy >= 0 && sy < dh && c >= 0) {
                const uint8_t* s = p.rgba.data() +
                    (static_cast<size_t>(std::min(static_cast<int>(p.height) - 1, static_cast<int>(sy / scale))) *
                         p.width + c) * 4;
                cu = Clamp(((-38 * s[0] - 74 * s[1] + 112 * s[2] + 128) >> 8) + 128);
                cv = Clamp(((112 * s[0] - 94 * s[1] - 18 * s[2] + 128) >> 8) + 128);
            }
            urow[x * uv_pixel_stride] = cu;
            vrow[x * uv_pixel_stride] = cv;
        }
    }
    const int64_t pts = p.pts_us;
    std::lock_guard<std::mutex> lock(mutex_);
    if (spare_.size() < kMaxPictures)
        spare_.push_back(std::move(p));
    return pts;
}

// Linear interpolation from the core's rate to 48 kHz, as far as the input goes.
void Recorder::ResampleLocked() {
    const double step = in_rate_ / kAudioRate;
    const size_t in_frames = raw_.size() / 2;
    if (in_frames < 2)
        return;
    while (resample_pos_ + 1.0 < static_cast<double>(in_frames)) {
        const size_t i = static_cast<size_t>(resample_pos_);
        const double t = resample_pos_ - i;
        for (int ch = 0; ch < 2; ch++) {
            const double a = raw_[i * 2 + ch];
            const double b = raw_[(i + 1) * 2 + ch];
            out_.push_back(static_cast<int16_t>(a + (b - a) * t));
        }
        resample_pos_ += step;
    }
    const size_t used = static_cast<size_t>(resample_pos_);
    raw_.erase(raw_.begin(), raw_.begin() + static_cast<std::ptrdiff_t>(used * 2));
    resample_pos_ -= used;
}

size_t Recorder::AudioAvailable() {
    std::lock_guard<std::mutex> lock(mutex_);
    ResampleLocked();
    return (out_.size() - out_read_) / 2;
}

size_t Recorder::FillAudio(int16_t* out, size_t max_frames, int64_t* pts_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResampleLocked();
    const size_t n = std::min(max_frames, (out_.size() - out_read_) / 2);
    std::memcpy(out, out_.data() + out_read_, n * 2 * sizeof(int16_t));
    out_read_ += n * 2;
    if (out_read_ > (1u << 16)) {
        out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(out_read_));
        out_read_ = 0;
    }
    *pts_us = audio_frames_out_ * 1000000 / kAudioRate;
    audio_frames_out_ += static_cast<int64_t>(n);
    return n;
}

}  // namespace cartucho
