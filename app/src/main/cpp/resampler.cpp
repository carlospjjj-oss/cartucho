#include "resampler.h"

#include <cmath>

namespace cartucho {

void Resampler::SetRates(double in_rate, double out_rate) {
    step_ = (in_rate > 0 && out_rate > 0) ? in_rate / out_rate : 1.0;
}

void Resampler::Reset() {
    pos_ = 0.0;
    primed_ = false;
    prev_[0] = prev_[1] = next_[0] = next_[1] = 0;
}

void Resampler::Process(PullFn pull, void* user, int16_t* out, int32_t frames) {
    // Input frames this call consumes, exactly: before output i the window
    // has slid floor(pos_ + step_ * i) times, so the last output needs
    // floor(pos_ + step_ * (frames - 1)) slides. Popping more would throw
    // samples away.
    if (frames <= 0)
        return;
    const double last = pos_ + step_ * (frames - 1);
    const size_t needed = static_cast<size_t>(std::floor(last)) + (primed_ ? 0 : 2);
    scratch_.resize((needed + 1) * 2);
    size_t got = needed ? pull(user, scratch_.data(), needed) : 0;
    // On underrun Pop() pads with silence; hold the last real sample instead
    // so a short gap does not click.
    for (size_t i = got; i < needed; i++) {
        const int16_t* last = got ? &scratch_[(got - 1) * 2] : (primed_ ? next_ : prev_);
        scratch_[i * 2] = last[0];
        scratch_[i * 2 + 1] = last[1];
    }
    size_t in = 0;
    if (!primed_) {
        // The first two input frames open the interpolation window.
        prev_[0] = scratch_[0];
        prev_[1] = scratch_[1];
        next_[0] = scratch_[2];
        next_[1] = scratch_[3];
        in = 2;
        primed_ = true;
    }
    for (int32_t i = 0; i < frames; i++) {
        while (pos_ >= 1.0) {
            prev_[0] = next_[0];
            prev_[1] = next_[1];
            if (in < needed) {
                next_[0] = scratch_[in * 2];
                next_[1] = scratch_[in * 2 + 1];
                in++;
            }
            pos_ -= 1.0;
        }
        const double t = pos_;
        out[i * 2] = static_cast<int16_t>(prev_[0] + (next_[0] - prev_[0]) * t);
        out[i * 2 + 1] = static_cast<int16_t>(prev_[1] + (next_[1] - prev_[1]) * t);
        pos_ += step_;
    }
}

}  // namespace cartucho
