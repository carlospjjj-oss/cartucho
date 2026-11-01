// Cartucho PC - the console look's sounds: short clicks when moving,
// opening and going back, and a calm background tune, all synthesized
// here like on Android (no recorded audio ships); or the player's own
// music file (.ogg / .wav).
#pragma once

#include <SDL.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace cartucho {

class Sounds {
public:
    enum Sfx { kMove, kSelect, kBack, kLaunch, kCount };
    bool Open();
    void Close();
    void Play(Sfx s);
    void SetEffects(bool on) { effects_ = on; }
    void SetMusic(bool on);
    void LoadMusic(const std::string& file);   // "" = the built-in tune
    void Pause(bool paused);

private:
    static void Callback(void* user, Uint8* out, int len);
    SDL_AudioDeviceID dev_ = 0;
    int rate_ = 44100;
    std::vector<int16_t> sfx_[kCount];
    std::vector<int16_t> music_;     // stereo
    std::mutex mutex_;
    struct Voice { const std::vector<int16_t>* pcm; size_t at; float vol; };
    std::vector<Voice> voices_;
    size_t music_at_ = 0;
    std::atomic<bool> effects_{true}, music_on_{true};
};

}  // namespace cartucho
