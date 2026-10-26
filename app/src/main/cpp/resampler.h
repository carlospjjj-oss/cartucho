// Cartucho - converts the core's audio rate to the device's.
#pragma once

#include <cstdint>
#include <vector>

#include <cstddef>

namespace cartucho {

// Linear-interpolating stereo resampler pulling from the host's ring buffer.
class Resampler {
public:
    void SetRates(double in_rate, double out_rate);
    // Source of input frames: fills `frames` stereo frames (padding with
    // silence) and returns how many were real.
    using PullFn = size_t (*)(void* user, int16_t* stereo, size_t frames);

    // Produces `frames` output frames into `out` (stereo int16).
    void Process(PullFn pull, void* user, int16_t* out, int32_t frames);
    void Reset();

private:
    double step_ = 1.0;  // input frames per output frame
    double pos_ = 0.0;   // fractional position between prev_ and next_
    int16_t prev_[2] = {0, 0};
    int16_t next_[2] = {0, 0};
    bool primed_ = false;
    std::vector<int16_t> scratch_;
};

}  // namespace cartucho
