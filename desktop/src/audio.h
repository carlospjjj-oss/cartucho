// Cartucho PC - sound: the core's audio resampled to the device's rate.
#pragma once

#include <SDL.h>

#include <atomic>

#include "core_host.h"
#include "resampler.h"

namespace cartucho {

class Audio {
public:
    ~Audio() { Stop(); }
    bool Start(CoreHost* host);
    void Stop();
    void Pause(bool paused);
    // How many core-rate frames should stay queued (pacing target).
    size_t target_queue_frames() const { return target_; }
    bool running() const { return dev_ != 0; }
    void set_volume(float v) { volume_ = v; }

private:
    static void Callback(void* user, Uint8* stream, int len);
    SDL_AudioDeviceID dev_ = 0;
    CoreHost* host_ = nullptr;
    Resampler resampler_;
    size_t target_ = 2048;
    std::atomic<float> volume_{1.0f};
};

}  // namespace cartucho
