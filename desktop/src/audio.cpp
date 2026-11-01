#include "audio.h"

#include <cstring>

namespace cartucho {

namespace {
size_t Pull(void* user, int16_t* stereo, size_t frames) {
    return static_cast<CoreHost*>(user)->PopAudio(stereo, frames);
}
}  // namespace

bool Audio::Start(CoreHost* host) {
    Stop();
    host_ = host;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = &Audio::Callback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!dev_)
        return false;
    resampler_.Reset();
    resampler_.SetRates(host_->sample_rate(), have.freq);
    // Two frames of game sound plus two device buffers, at the core's rate.
    const double per_frame = host_->sample_rate() / host_->fps();
    target_ = static_cast<size_t>(per_frame * 2 + have.samples * 2.0 * host_->sample_rate() / have.freq);
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void Audio::Stop() {
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
}

void Audio::Pause(bool paused) {
    if (dev_)
        SDL_PauseAudioDevice(dev_, paused ? 1 : 0);
}

void Audio::Callback(void* user, Uint8* stream, int len) {
    auto* self = static_cast<Audio*>(user);
    const int frames = len / 4;
    auto* out = reinterpret_cast<int16_t*>(stream);
    self->resampler_.Process(&Pull, self->host_, out, frames);
    const float v = self->volume_.load();
    if (v < 0.999f)
        for (int i = 0; i < frames * 2; i++)
            out[i] = static_cast<int16_t>(out[i] * v);
}

}  // namespace cartucho
