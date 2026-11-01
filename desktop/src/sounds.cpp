#include "sounds.h"

#include <cmath>

#include "paths.h"

#include <algorithm>
#include <cstring>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

namespace cartucho {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<double> Tone(int rate, double secs, double f0, double f1, double decay) {
    const int n = static_cast<int>(secs * rate);
    std::vector<double> out(static_cast<size_t>(n));
    double phase = 0;
    for (int i = 0; i < n; i++) {
        const double t = static_cast<double>(i) / n;
        phase += 2 * kPi * (f0 + (f1 - f0) * t) / rate;
        const double wave = (std::sin(phase) >= 0 ? 0.6 : -0.6) + 0.4 * std::sin(phase);
        out[static_cast<size_t>(i)] = wave * std::min(1.0, i / (rate * 0.003)) * std::exp(-t * decay);
    }
    return out;
}

std::vector<int16_t> Effect(int rate, int which) {
    std::vector<std::vector<double>> parts;
    switch (which) {
        case 0: parts = {Tone(rate, 0.035, 1320, 1320, 5)}; break;
        case 1: parts = {Tone(rate, 0.05, 880, 880, 2), Tone(rate, 0.09, 1320, 1320, 4)}; break;
        case 2: parts = {Tone(rate, 0.05, 990, 990, 2), Tone(rate, 0.09, 660, 660, 4)}; break;
        default: parts = {Tone(rate, 0.06, 523, 523, 1.5), Tone(rate, 0.06, 659, 659, 1.5),
                          Tone(rate, 0.06, 784, 784, 1.5), Tone(rate, 0.25, 1047, 1047, 3.5)}; break;
    }
    std::vector<int16_t> out;
    for (auto& p : parts)
        for (double v : p) {
            const int16_t s = static_cast<int16_t>(v * 0.5 * 32767);
            out.push_back(s);
            out.push_back(s);
        }
    return out;
}

// Am F C G, 8 bars at 84 bpm: pads, a plucked arpeggio and a slow bass.
std::vector<int16_t> Tune(int rate) {
    const double beat = 60.0 / 84;
    const int bars = 8;
    const size_t total = static_cast<size_t>(bars * 4 * beat * rate);
    std::vector<double> out(total, 0.0);
    auto midi = [](int n) { return 440.0 * std::pow(2.0, (n - 69) / 12.0); };
    const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
    const int arp[8] = {0, 1, 2, 1, 0, 2, 1, 2};
    for (int bar = 0; bar < bars; bar++) {
        const int* ch = chords[bar % 4];
        const size_t start = static_cast<size_t>(bar * 4 * beat * rate);
        const size_t len = static_cast<size_t>(4 * beat * rate);
        for (int k = 0; k < 3; k++) {
            const double f = midi(ch[k]);
            for (size_t i = 0; i < len; i++) {
                const double t = static_cast<double>(i) / len;
                const double env = std::pow(std::sin(kPi * t), 2);
                const double x = 2 * kPi * f * i / rate;
                out[(start + i) % total] += 0.05 * env * (std::sin(x) + 0.5 * std::sin(x * 1.003) + 0.25 * std::sin(x * 2));
            }
        }
        for (int b : {0, 2}) {
            const double f = midi(ch[0] - 12);
            const size_t s0 = start + static_cast<size_t>(b * beat * rate);
            const size_t n = static_cast<size_t>(1.8 * beat * rate);
            for (size_t i = 0; i < n; i++) {
                const double env = std::exp(-3.0 * i / n) * std::min(1.0, i / (rate * 0.01));
                out[(s0 + i) % total] += 0.09 * env * std::sin(2 * kPi * f * i / rate);
            }
        }
        for (int k = 0; k < 8; k++) {
            const double f = midi(ch[arp[k]] + 12);
            const size_t s0 = start + static_cast<size_t>(k * beat / 2 * rate);
            const size_t n = static_cast<size_t>(beat * 0.9 * rate);
            for (size_t i = 0; i < n; i++) {
                const double env = std::exp(-6.0 * i / n) * std::min(1.0, i / (rate * 0.004));
                const double x = 2 * kPi * f * i / rate;
                out[(s0 + i) % total] += 0.045 * env * (std::sin(x) - std::sin(3 * x) / 9 + std::sin(5 * x) / 25);
            }
        }
    }
    double peak = 1e-6;
    for (double v : out) peak = std::max(peak, std::fabs(v));
    std::vector<int16_t> pcm(total * 2);
    for (size_t i = 0; i < total; i++) {
        const int16_t s = static_cast<int16_t>(out[i] / peak * 0.8 * 32767);
        pcm[i * 2] = pcm[i * 2 + 1] = s;
    }
    return pcm;
}

}  // namespace

bool Sounds::Open() {
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &Sounds::Callback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) return false;
    rate_ = have.freq;
    for (int i = 0; i < kCount; i++) sfx_[i] = Effect(rate_, i);
    LoadMusic("");
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void Sounds::Close() {
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

void Sounds::Pause(bool paused) {
    if (dev_) SDL_PauseAudioDevice(dev_, paused ? 1 : 0);
}

void Sounds::SetMusic(bool on) { music_on_ = on; }

void Sounds::LoadMusic(const std::string& file) {
    std::vector<int16_t> pcm;
    if (!file.empty()) {
        const std::string ext = Lower(file.substr(file.find_last_of('.') + 1));
        if (ext == "ogg") {
            std::vector<uint8_t> bytes;
            if (ReadFileBytes(file, &bytes)) {
                int ch = 0, rate = 0;
                short* out = nullptr;
                const int n = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &ch, &rate, &out);
                if (n > 0 && out) {
                    SDL_AudioCVT cvt;
                    if (SDL_BuildAudioCVT(&cvt, AUDIO_S16SYS, static_cast<Uint8>(ch), rate, AUDIO_S16SYS, 2, rate_) >= 0) {
                        cvt.len = n * ch * 2;
                        std::vector<uint8_t> buf(static_cast<size_t>(cvt.len) * cvt.len_mult);
                        std::memcpy(buf.data(), out, static_cast<size_t>(cvt.len));
                        cvt.buf = buf.data();
                        if (SDL_ConvertAudio(&cvt) == 0)
                            pcm.assign(reinterpret_cast<int16_t*>(buf.data()), reinterpret_cast<int16_t*>(buf.data()) + cvt.len_cvt / 2);
                    }
                    free(out);
                }
            }
        } else {
            SDL_AudioSpec spec;
            Uint8* buf = nullptr;
            Uint32 len = 0;
            if (SDL_LoadWAV(file.c_str(), &spec, &buf, &len)) {
                SDL_AudioCVT cvt;
                if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, 2, rate_) >= 0) {
                    cvt.len = static_cast<int>(len);
                    std::vector<uint8_t> b(static_cast<size_t>(len) * cvt.len_mult);
                    std::memcpy(b.data(), buf, len);
                    cvt.buf = b.data();
                    if (SDL_ConvertAudio(&cvt) == 0)
                        pcm.assign(reinterpret_cast<int16_t*>(b.data()), reinterpret_cast<int16_t*>(b.data()) + cvt.len_cvt / 2);
                }
                SDL_FreeWAV(buf);
            }
        }
    }
    if (pcm.empty()) pcm = Tune(rate_);
    std::lock_guard<std::mutex> lock(mutex_);
    music_.swap(pcm);
    music_at_ = 0;
}

void Sounds::Play(Sfx s) {
    if (!effects_ || !dev_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (voices_.size() > 4) voices_.erase(voices_.begin());
    voices_.push_back({&sfx_[s], 0, s == kMove ? 0.35f : 0.55f});
}

void Sounds::Callback(void* user, Uint8* stream, int len) {
    auto* self = static_cast<Sounds*>(user);
    auto* out = reinterpret_cast<int16_t*>(stream);
    const size_t n = static_cast<size_t>(len) / 2;
    std::lock_guard<std::mutex> lock(self->mutex_);
    for (size_t i = 0; i < n; i++) {
        float v = 0;
        if (self->music_on_ && !self->music_.empty()) {
            v += self->music_[self->music_at_] * 0.35f;
            self->music_at_ = (self->music_at_ + 1) % self->music_.size();
        }
        for (auto& vo : self->voices_)
            if (vo.at < vo.pcm->size()) v += (*vo.pcm)[vo.at++] * vo.vol;
        out[i] = static_cast<int16_t>(std::max(-32768.f, std::min(32767.f, v)));
    }
    self->voices_.erase(std::remove_if(self->voices_.begin(), self->voices_.end(),
                                       [](const Voice& v) { return v.at >= v.pcm->size(); }), self->voices_.end());
}

}  // namespace cartucho
