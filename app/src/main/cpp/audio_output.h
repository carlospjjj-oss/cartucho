// Cartucho - audio output on Android (AAudio), resampling the core's rate
// (32768 Hz for GBA, 32728 Hz for 3DS...) to whatever the device runs at.
#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>

#include "core_host.h"
#include "resampler.h"

namespace cartucho {

class AudioOutput {
public:
    ~AudioOutput();
    // Opens the device stream; the host is pulled from the audio thread.
    bool Start(CoreHost* host);
    void Stop();
    // Reopens the stream after the device went away (call from the render
    // loop). Returns true if it restarted.
    bool RestartIfNeeded();
    bool running() const { return stream_ != nullptr; }
    int32_t device_rate() const { return device_rate_; }
    // How many input (core-rate) frames the output wants buffered, as a
    // target for the emulation pacing.
    size_t target_queue_frames() const { return target_queue_; }

private:
    static aaudio_data_callback_result_t DataCallback(AAudioStream* stream, void* user,
                                                      void* audio_data, int32_t num_frames);
    static void ErrorCallback(AAudioStream* stream, void* user, aaudio_result_t error);

    AAudioStream* stream_ = nullptr;
    CoreHost* host_ = nullptr;
    Resampler resampler_;
    int32_t device_rate_ = 48000;
    size_t target_queue_ = 2048;
    std::atomic<bool> restart_needed_{false};
};

}  // namespace cartucho
