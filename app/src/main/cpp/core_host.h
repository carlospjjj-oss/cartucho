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

    // The loaded game's bytes (empty for cores that open it by path) and path.
    const std::vector<uint8_t>& rom_data() const { return rom_data_; }
    const std::string& rom_path() const { return rom_path_; }
    // The core's memory map, or null if it gave none.
    const retro_memory_map* memory_map() const { return mem_descs_.empty() ? nullptr : &mem_map_; }
    // retro_get_memory_data / _size (RETRO_MEMORY_SYSTEM_RAM...).
    uint8_t* MemoryData(unsigned id, size_t* size) const;
    // An already open file holding the game (Android hands out descriptors,
    // not paths). Used instead of opening rom_path when the core takes the
    // game from memory; rom_path then only names it (cores look at the
    // extension). Not closed here.
    void SetRomFd(int fd) { rom_fd_ = fd; }
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
    // Drops audio without telling the core it is fast-forwarding (run-ahead).
    void SetAudioSuppressed(bool on) { audio_suppressed_ = on; }
    // What GET_AUDIO_VIDEO_ENABLE answers, so cores can skip work.
    enum AvBits { kAvVideo = 1, kAvAudio = 2 };
    void SetAvFlags(int flags) { av_flags_ = flags; }

    // Save states in memory (rewind). Empty / false when unsupported.
    bool SerializeTo(std::vector<uint8_t>* out);
    bool UnserializeFrom(const std::vector<uint8_t>& data);
    size_t StateSize();

    // Cheat codes in the console's usual formats (GameShark, Action Replay,
    // CodeBreaker...); the core parses them.
    void CheatReset();
    void CheatSet(unsigned index, bool enabled, const std::string& code);

    bool SaveState(const std::string& path, std::string* error);
    bool LoadState(const std::string& path, std::string* error);
    // Writes battery save RAM if it changed since the last flush.
    void FlushSaveRam();

    // Shows a message as if the core had sent it (frontend notices).
    void PostMessage(const std::string& text) {
        std::lock_guard<std::mutex> lock(message_mutex_);
        message_ = text;
    }
    // Last on-screen message the core asked to show; empty once taken.
    std::string TakeMessage();
    // The core requested shutdown (RETRO_ENVIRONMENT_SHUTDOWN).
    bool shutdown_requested() const { return shutdown_requested_; }

    // A symbol exported by the loaded core (frontend extensions such as the
    // patched VBA-M core's cartucho_link_*), or null.
    void* Symbol(const char* name) const;

    // --- Motion sensors, rumble, microphone ----------------------------------
    // Bits of sensors the core switched on: 1 accelerometer, 2 gyroscope,
    // 4 light. The platform listens to those only.
    enum SensorBits { kAccel = 1, kGyro = 2, kLight = 4 };
    int sensors_wanted() const { return sensors_wanted_.load(std::memory_order_relaxed); }
    // Accelerometer in m/s^2 and gyroscope in rad/s, axes relative to the
    // screen as the player sees it (libretro convention). Any thread.
    void SetAccel(float x, float y, float z);
    void SetGyro(float x, float y, float z);
    void SetLight(float lux) { light_.store(lux, std::memory_order_relaxed); }
    // Some cores read the accelerometer in g instead of m/s^2 (Azahar).
    void set_accel_in_g(bool on) { accel_in_g_ = on; }

    // Rumble strength the core asks for, 0..65535, strong and weak motors.
    uint16_t rumble_strong() const { return rumble_[0].load(std::memory_order_relaxed); }
    uint16_t rumble_weak() const { return rumble_[1].load(std::memory_order_relaxed); }

    // Microphone: the rate the core wants while it is listening, else 0.
    unsigned mic_active_rate() const;
    // Feeds recorded mono samples (any rate; resampled to the core's).
    void PushMic(const int16_t* samples, size_t count, unsigned rate);

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
    // The core's memory map (SET_MEMORY_MAPS), copied: achievements read it.
    std::vector<retro_memory_descriptor> mem_descs_;
    std::vector<std::string> mem_names_;
    retro_memory_map mem_map_{};
    int rom_fd_ = -1;

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
    bool audio_suppressed_ = false;
    int av_flags_ = kAvVideo | kAvAudio;

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

    // Sensors / rumble / microphone.
    friend struct CoreHostCallbacks;
    std::atomic<int> sensors_wanted_{0};
    std::atomic<float> accel_[3]{{0.f}, {0.f}, {0.f}};
    std::atomic<float> gyro_[3]{{0.f}, {0.f}, {0.f}};
    std::atomic<float> light_{0.f};
    bool accel_in_g_ = false;
    std::atomic<uint16_t> rumble_[2]{{0}, {0}};
    struct Mic {
        unsigned rate = 0;
        bool active = false;
    };
    std::unique_ptr<Mic> mic_;
    mutable std::mutex mic_mutex_;
    std::vector<int16_t> mic_buf_;  // at the core's rate, oldest first
    double mic_pos_ = 0.0;          // resampling phase
    int16_t mic_last_ = 0;

    std::mutex message_mutex_;
    std::string message_;
};

}  // namespace cartucho
