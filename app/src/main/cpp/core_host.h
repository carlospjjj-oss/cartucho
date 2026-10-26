// Cartucho - libretro core host.
//
// Loads one libretro core (a shared library such as the VBA-M or Azahar core),
// answers its environment callbacks and exposes what a frontend needs: run a
// frame, read the last video frame, pull audio, feed input, saves and save
// states. Platform independent: the Android glue (JNI, GLES presentation,
// AAudio) lives elsewhere, and the same code runs in tools/host-test on Linux.
//
// Threading: everything except the input setters and PopAudio() must be called
// from one thread (on Android, the GL render thread, because hardware-rendered
// cores make GL calls from retro_load_game() and retro_run()). Input setters
// are atomic; PopAudio() is the consumer side of a single-producer ring buffer
// and may be called from the audio callback thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "libretro.h"

namespace cartucho {

// Lock-free single-producer / single-consumer ring of stereo int16 frames.
class AudioRing {
public:
    explicit AudioRing(size_t capacity_frames = 1 << 14);

    // Producer. Returns how many frames were stored; the rest are dropped
    // when the consumer falls behind (better a click than growing latency).
    size_t Push(const int16_t* stereo, size_t frames);
    // Consumer. Fills `frames` frames, padding with silence on underrun;
    // returns how many real frames were copied.
    size_t Pop(int16_t* stereo, size_t frames);
    size_t Available() const;
    void Clear();

private:
    std::vector<int16_t> buf_;
    const size_t cap_;
    std::atomic<size_t> head_{0};  // written by producer
    std::atomic<size_t> tail_{0};  // written by consumer
};

// What the GL side must provide for hardware-rendered cores.
struct HwHooks {
    std::function<uintptr_t()> current_framebuffer;
    std::function<retro_proc_address_t(const char*)> get_proc_address;
};

struct VideoFrame {
    // Software frames are converted to tightly packed RGBA8888 here.
    std::vector<uint8_t> rgba;
    unsigned width = 0;
    unsigned height = 0;
    bool hw = false;          // frame lives in the HW framebuffer instead
    uint64_t serial = 0;      // bumped on every new frame
};

class CoreHost {
public:
    CoreHost();
    ~CoreHost();
    CoreHost(const CoreHost&) = delete;
    CoreHost& operator=(const CoreHost&) = delete;

    // The single live instance (libretro callbacks carry no user data).
    static CoreHost* Current();

    void SetDirectories(const std::string& system_dir, const std::string& save_dir);
    // Value used instead of the core's default for `key`. May be called before
    // the core declares its options, and at any time while running (the core
    // sees the change through GET_VARIABLE_UPDATE).
    void SetOption(const std::string& key, const std::string& value);
    // Answer to GET_PREFERRED_HW_RENDER (default: OpenGL ES 3).
    void SetPreferredHwContext(retro_hw_context_type type) { preferred_hw_ = type; }
    void SetHwHooks(HwHooks hooks) { hw_hooks_ = std::move(hooks); }

    bool LoadCore(const std::string& path, std::string* error);
    // `save_path` is where battery saves (SRAM) live for this game, e.g.
    // ".../saves/Game.srm". Cores that manage their own saves ignore it.
    bool LoadGame(const std::string& rom_path, const std::string& save_path, std::string* error);
    void UnloadGame();

    bool game_loaded() const { return game_loaded_; }
    bool uses_hw_render() const { return hw_requested_; }
    const retro_hw_render_callback& hw_render() const { return hw_render_; }
    // Call once a GL context exists (and again after it was lost).
    void ContextReset();
    void ContextDestroy();

    const retro_system_av_info& av_info() const { return av_info_; }
    const retro_system_info& system_info() const { return system_info_; }
    double fps() const { return av_info_.timing.fps > 0 ? av_info_.timing.fps : 60.0; }
    double sample_rate() const {
        return av_info_.timing.sample_rate > 0 ? av_info_.timing.sample_rate : 44100.0;
    }
    // Display aspect ratio of the current frame.
    float aspect() const;

    void RunFrame();
    void Reset();
    const VideoFrame& frame() const { return frame_; }

    // Input (any thread). Buttons use RETRO_DEVICE_ID_JOYPAD_* bit positions.
    void SetButtons(uint32_t mask) { buttons_.store(mask, std::memory_order_relaxed); }
    void SetAnalog(int stick, int16_t x, int16_t y);
    // Pointer in libretro coordinates (-0x7fff..0x7fff over the game image).
    void SetPointer(int16_t x, int16_t y, bool pressed);

    // Audio (consumer side, any one thread).
    size_t PopAudio(int16_t* stereo, size_t frames) { return audio_.Pop(stereo, frames); }
    size_t QueuedAudioFrames() const { return audio_.Available(); }
    void SetAudioMuted(bool muted) { audio_muted_ = muted; }

    bool SaveState(const std::string& path, std::string* error);
    bool LoadState(const std::string& path, std::string* error);
    // Writes battery save RAM if it changed since the last flush.
    void FlushSaveRam();

    // Last on-screen message the core asked to show; empty once taken.
    std::string TakeMessage();
    // The core requested shutdown (RETRO_ENVIRONMENT_SHUTDOWN).
    bool shutdown_requested() const { return shutdown_requested_; }

    // Options as declared by the core (key -> "desc; a|b|c") and current values.
    std::map<std::string, std::string> DeclaredOptions();
    std::string OptionValue(const std::string& key);

private:
    bool Environment(unsigned cmd, void* data);
    void VideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch);
    size_t AudioBatch(const int16_t* data, size_t frames);
    int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id);
    void LoadSaveRam();

    static bool EnvTrampoline(unsigned cmd, void* data);
    static void VideoTrampoline(const void* data, unsigned w, unsigned h, size_t pitch);
    static void AudioSampleTrampoline(int16_t l, int16_t r);
    static size_t AudioBatchTrampoline(const int16_t* data, size_t frames);
    static void InputPollTrampoline();
    static int16_t InputStateTrampoline(unsigned port, unsigned device, unsigned index, unsigned id);
    static uintptr_t HwFramebufferTrampoline();
    static retro_proc_address_t HwProcTrampoline(const char* sym);
    static void LogTrampoline(enum retro_log_level level, const char* fmt, ...);

    struct Api;
    std::unique_ptr<Api> api_;
    void* handle_ = nullptr;

    std::string system_dir_ = ".";
    std::string save_dir_ = ".";
    std::string save_path_;
    std::vector<uint8_t> rom_data_;  // kept alive for cores without need_fullpath
    std::string rom_path_;

    retro_system_info system_info_{};
    retro_system_av_info av_info_{};
    retro_pixel_format pixel_format_ = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_hw_context_type preferred_hw_ = RETRO_HW_CONTEXT_OPENGLES3;
    retro_hw_render_callback hw_render_{};
    bool hw_requested_ = false;
    bool hw_context_alive_ = false;
    HwHooks hw_hooks_;
    bool game_loaded_ = false;
    bool shutdown_requested_ = false;

    VideoFrame frame_;
    AudioRing audio_;
    bool audio_muted_ = false;

    std::atomic<uint32_t> buttons_{0};
    std::atomic<int32_t> analog_[2]{{0}, {0}};  // packed x/y per stick
    std::atomic<int32_t> pointer_{0};
    std::atomic<bool> pointer_pressed_{false};

    std::mutex options_mutex_;
    std::map<std::string, std::string> declared_;   // key -> "desc; a|b|c"
    std::map<std::string, std::string> defaults_;
    std::map<std::string, std::string> overrides_;
    std::map<std::string, std::string> value_cache_;  // GET_VARIABLE answers live here
    bool options_dirty_ = false;

    std::vector<uint8_t> last_saved_sram_;

    std::mutex message_mutex_;
    std::string message_;
};

}  // namespace cartucho
