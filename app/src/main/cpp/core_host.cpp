#include "core_host.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

#include "platform.h"

namespace cartucho {

// --- AudioRing ---------------------------------------------------------------

AudioRing::AudioRing(size_t capacity_frames) : buf_(capacity_frames * 2), cap_(capacity_frames) {}

size_t AudioRing::Available() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
}

size_t AudioRing::Push(const int16_t* stereo, size_t frames) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t space = cap_ - (head - tail);
    const size_t n = std::min(space, frames);
    for (size_t i = 0; i < n; i++) {
        const size_t slot = (head + i) % cap_;
        buf_[slot * 2] = stereo[i * 2];
        buf_[slot * 2 + 1] = stereo[i * 2 + 1];
    }
    head_.store(head + n, std::memory_order_release);
    return n;
}

size_t AudioRing::Pop(int16_t* stereo, size_t frames) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t n = std::min(head - tail, frames);
    for (size_t i = 0; i < n; i++) {
        const size_t slot = (tail + i) % cap_;
        stereo[i * 2] = buf_[slot * 2];
        stereo[i * 2 + 1] = buf_[slot * 2 + 1];
    }
    std::memset(stereo + n * 2, 0, (frames - n) * 2 * sizeof(int16_t));
    tail_.store(tail + n, std::memory_order_release);
    return n;
}

void AudioRing::Clear() {
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
}

// --- Core API ----------------------------------------------------------------

struct CoreHost::Api {
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)();
    void (*deinit)();
    unsigned (*api_version)();
    void (*get_system_info)(retro_system_info*);
    void (*get_system_av_info)(retro_system_av_info*);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)();
    void (*run)();
    size_t (*serialize_size)();
    bool (*serialize)(void*, size_t);
    bool (*unserialize)(const void*, size_t);
    bool (*load_game)(const retro_game_info*);
    void (*unload_game)();
    void (*cheat_reset)();
    void (*cheat_set)(unsigned, bool, const char*);
    void* (*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    bool initialized = false;
};

// libretro leaves the microphone handle to the frontend.
struct retro_microphone {
    int unused;
};

namespace {

CoreHost* g_current = nullptr;

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out->clear();
    uint8_t chunk[1 << 16];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        out->insert(out->end(), chunk, chunk + n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

bool ReadFd(int fd, std::vector<uint8_t>* out) {
    out->clear();
    uint8_t chunk[1 << 16];
    off_t offset = 0;
    for (;;) {
        const ssize_t n = pread(fd, chunk, sizeof(chunk), offset);
        if (n < 0)
            return false;
        if (n == 0)
            return true;
        out->insert(out->end(), chunk, chunk + n);
        offset += n;
    }
}

// Writes through a temporary file so a crash mid-write never leaves a
// truncated save behind.
bool WriteFileAtomic(const std::string& path, const void* data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data, 1, size, f) == size && std::fflush(f) == 0;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// "Description; first|second|third" -> "first" (the libretro default).
std::string DefaultFromDeclaration(const std::string& decl) {
    const size_t semi = decl.find(';');
    if (semi == std::string::npos)
        return {};
    size_t start = semi + 1;
    while (start < decl.size() && decl[start] == ' ')
        start++;
    const size_t bar = decl.find('|', start);
    return decl.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
}

uint32_t Expand5(uint32_t v) { return (v << 3) | (v >> 2); }
uint32_t Expand6(uint32_t v) { return (v << 2) | (v >> 4); }

}  // namespace

// --- CoreHost ----------------------------------------------------------------

CoreHost::CoreHost() : api_(std::make_unique<Api>()) {
    g_current = this;
}

CoreHost::~CoreHost() {
    if (game_loaded_)
        UnloadGame();
    if (api_->initialized)
        api_->deinit();
    // The library is deliberately not dlclose()d: several cores leave threads
    // or atexit handlers behind. The game runs in its own process, which ends
    // with the session.
    if (g_current == this)
        g_current = nullptr;
}

CoreHost* CoreHost::Current() {
    return g_current;
}

void CoreHost::SetDirectories(const std::string& system_dir, const std::string& save_dir) {
    system_dir_ = system_dir;
    save_dir_ = save_dir;
}

void CoreHost::SetOption(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(options_mutex_);
    auto it = overrides_.find(key);
    if (it != overrides_.end() && it->second == value)
        return;
    overrides_[key] = value;
    options_dirty_ = true;
}

std::map<std::string, std::string> CoreHost::DeclaredOptions() {
    std::lock_guard<std::mutex> lock(options_mutex_);
    return declared_;
}

std::string CoreHost::OptionValue(const std::string& key) {
    std::lock_guard<std::mutex> lock(options_mutex_);
    auto o = overrides_.find(key);
    if (o != overrides_.end())
        return o->second;
    auto d = defaults_.find(key);
    return d != defaults_.end() ? d->second : std::string();
}

bool CoreHost::LoadCore(const std::string& path, std::string* error) {
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* why = dlerror();
        *error = "Could not open core " + path + ": " + (why ? why : "unknown error");
        return false;
    }
    bool missing = false;
    auto sym = [&](const char* name) -> void* {
        void* p = dlsym(handle_, name);
        if (!p) {
            missing = true;
            *error = std::string("Core is missing ") + name;
        }
        return p;
    };
#define CARTUCHO_SYM(field, name) \
    api_->field = reinterpret_cast<decltype(api_->field)>(sym(name))
    CARTUCHO_SYM(set_environment, "retro_set_environment");
    CARTUCHO_SYM(set_video_refresh, "retro_set_video_refresh");
    CARTUCHO_SYM(set_audio_sample, "retro_set_audio_sample");
    CARTUCHO_SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    CARTUCHO_SYM(set_input_poll, "retro_set_input_poll");
    CARTUCHO_SYM(set_input_state, "retro_set_input_state");
    CARTUCHO_SYM(init, "retro_init");
    CARTUCHO_SYM(deinit, "retro_deinit");
    CARTUCHO_SYM(api_version, "retro_api_version");
    CARTUCHO_SYM(get_system_info, "retro_get_system_info");
    CARTUCHO_SYM(get_system_av_info, "retro_get_system_av_info");
    CARTUCHO_SYM(set_controller_port_device, "retro_set_controller_port_device");
    CARTUCHO_SYM(reset, "retro_reset");
    CARTUCHO_SYM(run, "retro_run");
    CARTUCHO_SYM(serialize_size, "retro_serialize_size");
    CARTUCHO_SYM(serialize, "retro_serialize");
    CARTUCHO_SYM(unserialize, "retro_unserialize");
    CARTUCHO_SYM(load_game, "retro_load_game");
    CARTUCHO_SYM(unload_game, "retro_unload_game");
    CARTUCHO_SYM(cheat_reset, "retro_cheat_reset");
    CARTUCHO_SYM(cheat_set, "retro_cheat_set");
    CARTUCHO_SYM(get_memory_data, "retro_get_memory_data");
    CARTUCHO_SYM(get_memory_size, "retro_get_memory_size");
#undef CARTUCHO_SYM
    if (missing)
        return false;
    if (api_->api_version() != RETRO_API_VERSION) {
        *error = "Core uses an incompatible libretro API version";
        return false;
    }

    api_->set_environment(&CoreHost::EnvTrampoline);
    api_->set_video_refresh(&CoreHost::VideoTrampoline);
    api_->set_audio_sample(&CoreHost::AudioSampleTrampoline);
    api_->set_audio_sample_batch(&CoreHost::AudioBatchTrampoline);
    api_->set_input_poll(&CoreHost::InputPollTrampoline);
    api_->set_input_state(&CoreHost::InputStateTrampoline);
    api_->init();
    api_->initialized = true;
    api_->get_system_info(&system_info_);
    PlatformLog(RETRO_LOG_INFO, (std::string("Loaded core ") +
                                 (system_info_.library_name ? system_info_.library_name : "?") +
                                 " " +
                                 (system_info_.library_version ? system_info_.library_version : ""))
                                    .c_str());
    return true;
}

bool CoreHost::LoadGame(const std::string& rom_path, const std::string& save_path,
                        std::string* error) {
    rom_path_ = rom_path;
    save_path_ = save_path;

    retro_game_info info{};
    info.path = rom_path_.c_str();
    if (!system_info_.need_fullpath) {
        const bool ok = rom_fd_ >= 0 ? ReadFd(rom_fd_, &rom_data_) : ReadFile(rom_path_, &rom_data_);
        if (!ok || rom_data_.empty()) {
            *error = "Could not read " + rom_path_;
            return false;
        }
        info.data = rom_data_.data();
        info.size = rom_data_.size();
    }
    if (!api_->load_game(&info)) {
        *error = "The core could not load this game";
        return false;
    }
    game_loaded_ = true;
    api_->get_system_av_info(&av_info_);
    api_->set_controller_port_device(0, port_device_);
    LoadSaveRam();
    return true;
}

void CoreHost::UnloadGame() {
    if (!game_loaded_)
        return;
    FlushSaveRam();
    api_->unload_game();
    game_loaded_ = false;
}

void CoreHost::ContextReset() {
    if (!hw_requested_ || !hw_render_.context_reset)
        return;
    hw_context_alive_ = true;
    hw_render_.context_reset();
}

void CoreHost::ContextDestroy() {
    if (!hw_requested_ || !hw_context_alive_)
        return;
    hw_context_alive_ = false;
    if (hw_render_.context_destroy)
        hw_render_.context_destroy();
}

float CoreHost::aspect() const {
    if (av_info_.geometry.aspect_ratio > 0.0f)
        return av_info_.geometry.aspect_ratio;
    if (frame_.width && frame_.height)
        return static_cast<float>(frame_.width) / static_cast<float>(frame_.height);
    if (av_info_.geometry.base_width && av_info_.geometry.base_height)
        return static_cast<float>(av_info_.geometry.base_width) /
               static_cast<float>(av_info_.geometry.base_height);
    return 4.0f / 3.0f;
}

void CoreHost::RunFrame() {
    if (game_loaded_)
        api_->run();
}

void CoreHost::Reset() {
    if (game_loaded_)
        api_->reset();
}

void CoreHost::SetAnalog(int stick, int16_t x, int16_t y) {
    if (stick < 0 || stick > 1)
        return;
    const int32_t packed = static_cast<int32_t>(static_cast<uint16_t>(x)) |
                           (static_cast<int32_t>(static_cast<uint16_t>(y)) << 16);
    analog_[stick].store(packed, std::memory_order_relaxed);
}

void CoreHost::SetPointer(int16_t x, int16_t y, bool pressed) {
    const int32_t packed = static_cast<int32_t>(static_cast<uint16_t>(x)) |
                           (static_cast<int32_t>(static_cast<uint16_t>(y)) << 16);
    pointer_.store(packed, std::memory_order_relaxed);
    pointer_pressed_.store(pressed, std::memory_order_relaxed);
}

size_t CoreHost::StateSize() {
    return game_loaded_ ? api_->serialize_size() : 0;
}

void CoreHost::SetPortDevice(unsigned device) {
    port_device_ = device;
    if (game_loaded_)
        api_->set_controller_port_device(0, device);
}

unsigned CoreHost::DiskCount() const {
    return (game_loaded_ && disk_.get_num_images) ? disk_.get_num_images() : 0;
}

int CoreHost::DiskIndex() const {
    return (game_loaded_ && disk_.get_image_index) ? static_cast<int>(disk_.get_image_index()) : -1;
}

std::string CoreHost::DiskLabel(unsigned index) const {
    char label[256] = {};
    if (disk_.get_image_label && disk_.get_image_label(index, label, sizeof(label)) && label[0])
        return label;
    if (disk_.get_image_path && disk_.get_image_path(index, label, sizeof(label)) && label[0]) {
        std::string path = label;
        return path.substr(path.find_last_of('/') + 1);
    }
    return "Disco " + std::to_string(index + 1);
}

bool CoreHost::SetDisk(unsigned index) {
    if (!game_loaded_ || !disk_.set_eject_state || !disk_.set_image_index || index >= DiskCount())
        return false;
    disk_.set_eject_state(true);
    const bool ok = disk_.set_image_index(index);
    disk_.set_eject_state(false);
    return ok;
}

uint8_t* CoreHost::MemoryData(unsigned id, size_t* size) const {
    *size = 0;
    if (!game_loaded_ || !api_->get_memory_data)
        return nullptr;
    *size = api_->get_memory_size(id);
    return static_cast<uint8_t*>(api_->get_memory_data(id));
}

bool CoreHost::SerializeTo(std::vector<uint8_t>* out) {
    const size_t size = StateSize();
    if (size == 0)
        return false;
    out->resize(size);
    return api_->serialize(out->data(), size);
}

bool CoreHost::UnserializeFrom(const std::vector<uint8_t>& data) {
    return game_loaded_ && !data.empty() && api_->unserialize(data.data(), data.size());
}

void CoreHost::CheatReset() {
    if (game_loaded_)
        api_->cheat_reset();
}

void CoreHost::CheatSet(unsigned index, bool enabled, const std::string& code) {
    if (game_loaded_)
        api_->cheat_set(index, enabled, code.c_str());
}

bool CoreHost::SaveState(const std::string& path, std::string* error) {
    if (!game_loaded_) {
        *error = "No game is running";
        return false;
    }
    const size_t size = api_->serialize_size();
    if (size == 0) {
        *error = "This core does not support save states";
        return false;
    }
    std::vector<uint8_t> data(size);
    if (!api_->serialize(data.data(), data.size())) {
        *error = "The core could not create the save state";
        return false;
    }
    if (!WriteFileAtomic(path, data.data(), data.size())) {
        *error = "Could not write " + path;
        return false;
    }
    return true;
}

bool CoreHost::LoadState(const std::string& path, std::string* error) {
    if (!game_loaded_) {
        *error = "No game is running";
        return false;
    }
    std::vector<uint8_t> data;
    if (!ReadFile(path, &data) || data.empty()) {
        *error = "No save state in this slot";
        return false;
    }
    if (!api_->unserialize(data.data(), data.size())) {
        *error = "The save state is from another version or game";
        return false;
    }
    audio_.Clear();
    return true;
}

void CoreHost::LoadSaveRam() {
    if (save_path_.empty())
        return;
    void* mem = api_->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t size = api_->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || size == 0)
        return;
    std::vector<uint8_t> data;
    if (ReadFile(save_path_, &data) && !data.empty()) {
        std::memcpy(mem, data.data(), std::min(size, data.size()));
        last_saved_sram_.assign(static_cast<uint8_t*>(mem), static_cast<uint8_t*>(mem) + size);
    }
}

void CoreHost::FlushSaveRam() {
    if (!game_loaded_ || save_path_.empty())
        return;
    void* mem = api_->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t size = api_->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || size == 0)
        return;
    const uint8_t* bytes = static_cast<const uint8_t*>(mem);
    if (last_saved_sram_.size() == size && std::memcmp(last_saved_sram_.data(), bytes, size) == 0)
        return;
    // A save area that was never written holds only 0xFF/0x00; don't create
    // a file for games that have not saved yet.
    if (last_saved_sram_.empty() &&
        std::all_of(bytes, bytes + size, [&](uint8_t b) { return b == bytes[0]; }))
        return;
    if (WriteFileAtomic(save_path_, bytes, size))
        last_saved_sram_.assign(bytes, bytes + size);
    else
        PlatformLog(RETRO_LOG_ERROR, ("Could not write save " + save_path_).c_str());
}

void* CoreHost::Symbol(const char* name) const {
    return handle_ ? dlsym(handle_, name) : nullptr;
}

std::string CoreHost::TakeMessage() {
    std::lock_guard<std::mutex> lock(message_mutex_);
    std::string m;
    m.swap(message_);
    return m;
}

// --- Sensors, rumble, microphone ---------------------------------------------------

void CoreHost::SetAccel(float x, float y, float z) {
    accel_[0].store(x, std::memory_order_relaxed);
    accel_[1].store(y, std::memory_order_relaxed);
    accel_[2].store(z, std::memory_order_relaxed);
}

void CoreHost::SetGyro(float x, float y, float z) {
    gyro_[0].store(x, std::memory_order_relaxed);
    gyro_[1].store(y, std::memory_order_relaxed);
    gyro_[2].store(z, std::memory_order_relaxed);
}

unsigned CoreHost::mic_active_rate() const {
    std::lock_guard<std::mutex> lock(mic_mutex_);
    return (mic_ && mic_->active) ? mic_->rate : 0;
}

void CoreHost::PushMic(const int16_t* samples, size_t count, unsigned rate) {
    std::lock_guard<std::mutex> lock(mic_mutex_);
    if (!mic_ || !mic_->active || rate == 0 || mic_->rate == 0)
        return;
    // Linear resampling from the recording rate to the core's.
    const double step = static_cast<double>(rate) / mic_->rate;
    double pos = mic_pos_;
    int16_t prev = mic_last_;
    for (size_t i = 0; i < count; i++) {
        const int16_t next = samples[i];
        while (pos < 1.0) {
            mic_buf_.push_back(static_cast<int16_t>(prev + (next - prev) * pos));
            pos += step;
        }
        pos -= 1.0;
        prev = next;
    }
    mic_pos_ = pos;
    mic_last_ = prev;
    // Keep at most half a second: latency matters more than gaps.
    const size_t cap = mic_->rate / 2;
    if (mic_buf_.size() > cap)
        mic_buf_.erase(mic_buf_.begin(), mic_buf_.end() - static_cast<ptrdiff_t>(cap));
}

// Static callbacks handed to the cores; they reach the live host.
struct CoreHostCallbacks {
    static bool RETRO_CALLCONV SetSensorState(unsigned port, enum retro_sensor_action action,
                                              unsigned rate) {
        CoreHost* h = g_current;
        if (!h || port != 0)
            return false;
        int bit = 0;
        bool on = false;
        switch (action) {
            case RETRO_SENSOR_ACCELEROMETER_ENABLE: bit = CoreHost::kAccel; on = true; break;
            case RETRO_SENSOR_ACCELEROMETER_DISABLE: bit = CoreHost::kAccel; break;
            case RETRO_SENSOR_GYROSCOPE_ENABLE: bit = CoreHost::kGyro; on = true; break;
            case RETRO_SENSOR_GYROSCOPE_DISABLE: bit = CoreHost::kGyro; break;
            case RETRO_SENSOR_ILLUMINANCE_ENABLE: bit = CoreHost::kLight; on = true; break;
            case RETRO_SENSOR_ILLUMINANCE_DISABLE: bit = CoreHost::kLight; break;
            default: return false;
        }
        if (on)
            h->sensors_wanted_.fetch_or(bit);
        else
            h->sensors_wanted_.fetch_and(~bit);
        return true;
    }

    static float RETRO_CALLCONV GetSensorInput(unsigned port, unsigned id) {
        CoreHost* h = g_current;
        if (!h || port != 0)
            return 0.f;
        const float accel_scale = h->accel_in_g_ ? 1.0f / 9.80665f : 1.0f;
        switch (id) {
            case RETRO_SENSOR_ACCELEROMETER_X: return h->accel_[0].load() * accel_scale;
            case RETRO_SENSOR_ACCELEROMETER_Y: return h->accel_[1].load() * accel_scale;
            case RETRO_SENSOR_ACCELEROMETER_Z: return h->accel_[2].load() * accel_scale;
            case RETRO_SENSOR_GYROSCOPE_X: return h->gyro_[0].load();
            case RETRO_SENSOR_GYROSCOPE_Y: return h->gyro_[1].load();
            case RETRO_SENSOR_GYROSCOPE_Z: return h->gyro_[2].load();
            case RETRO_SENSOR_ILLUMINANCE: return h->light_.load();
            default: return 0.f;
        }
    }

    static bool RETRO_CALLCONV SetRumble(unsigned port, enum retro_rumble_effect effect,
                                         uint16_t strength) {
        CoreHost* h = g_current;
        if (!h || port != 0 || effect > RETRO_RUMBLE_WEAK)
            return false;
        h->rumble_[effect].store(strength, std::memory_order_relaxed);
        return true;
    }

    static retro_microphone_t* RETRO_CALLCONV OpenMic(const retro_microphone_params_t* params) {
        CoreHost* h = g_current;
        if (!h)
            return nullptr;
        std::lock_guard<std::mutex> lock(h->mic_mutex_);
        // One microphone at a time; a core that opens a new one (melonDS DS
        // does while loading) gets it, and the old handle stops counting.
        h->mic_ = std::make_unique<CoreHost::Mic>();
        h->mic_->rate = (params && params->rate) ? params->rate : 44100;
        h->mic_buf_.clear();
        return reinterpret_cast<retro_microphone_t*>(h->mic_.get());
    }

    static void RETRO_CALLCONV CloseMic(retro_microphone_t* mic) {
        CoreHost* h = g_current;
        if (!h || !mic)
            return;
        std::lock_guard<std::mutex> lock(h->mic_mutex_);
        if (reinterpret_cast<CoreHost::Mic*>(mic) == h->mic_.get())
            h->mic_.reset();
    }

    static bool RETRO_CALLCONV GetMicParams(const retro_microphone_t* mic,
                                            retro_microphone_params_t* params) {
        CoreHost* h = g_current;
        if (!h || !mic || !params)
            return false;
        std::lock_guard<std::mutex> lock(h->mic_mutex_);
        params->rate = reinterpret_cast<const CoreHost::Mic*>(mic)->rate;
        return true;
    }

    static bool RETRO_CALLCONV SetMicState(retro_microphone_t* mic, bool state) {
        CoreHost* h = g_current;
        if (!h || !mic)
            return false;
        std::lock_guard<std::mutex> lock(h->mic_mutex_);
        auto* m = reinterpret_cast<CoreHost::Mic*>(mic);
        if (m->active != state)
            h->mic_buf_.clear();
        m->active = state;
        return true;
    }

    static bool RETRO_CALLCONV GetMicState(const retro_microphone_t* mic) {
        CoreHost* h = g_current;
        if (!h || !mic)
            return false;
        std::lock_guard<std::mutex> lock(h->mic_mutex_);
        return reinterpret_cast<const CoreHost::Mic*>(mic)->active;
    }

    static int RETRO_CALLCONV ReadMic(retro_microphone_t* mic, int16_t* samples, size_t count) {
        CoreHost* h = g_current;
        if (!h || !mic)
            return -1;
        std::lock_guard<std::mutex> lock(h->mic_mutex_);
        if (!reinterpret_cast<CoreHost::Mic*>(mic)->active)
            return -1;
        // Always hand back what was asked for, padding with silence: cores
        // poll every frame and do not cope well with short reads.
        const size_t n = std::min(count, h->mic_buf_.size());
        std::memcpy(samples, h->mic_buf_.data(), n * sizeof(int16_t));
        h->mic_buf_.erase(h->mic_buf_.begin(), h->mic_buf_.begin() + static_cast<ptrdiff_t>(n));
        std::memset(samples + n, 0, (count - n) * sizeof(int16_t));
        return static_cast<int>(count);
    }
};

// --- Environment ---------------------------------------------------------------

bool CoreHost::Environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;
            return true;

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const auto fmt = *static_cast<const retro_pixel_format*>(data);
            if (fmt != RETRO_PIXEL_FORMAT_0RGB1555 && fmt != RETRO_PIXEL_FORMAT_XRGB8888 &&
                fmt != RETRO_PIXEL_FORMAT_RGB565)
                return false;
            pixel_format_ = fmt;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *static_cast<const char**>(data) = system_dir_.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char**>(data) = save_dir_.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *static_cast<const char**>(data) = system_dir_.c_str();
            return true;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            static_cast<retro_log_callback*>(data)->log = &CoreHost::LogTrampoline;
            return true;

        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
            *static_cast<unsigned*>(data) = preferred_hw_;
            return true;

        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            auto* cb = static_cast<retro_hw_render_callback*>(data);
            const bool gles = cb->context_type == RETRO_HW_CONTEXT_OPENGLES2 ||
                              cb->context_type == RETRO_HW_CONTEXT_OPENGLES3 ||
                              cb->context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION;
            if (!gles || !hw_hooks_.current_framebuffer) {
                PlatformLog(RETRO_LOG_WARN, "Core asked for a hardware context we do not offer");
                return false;
            }
            cb->get_current_framebuffer = &CoreHost::HwFramebufferTrampoline;
            cb->get_proc_address = &CoreHost::HwProcTrampoline;
            hw_render_ = *cb;
            hw_requested_ = true;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
            return true;

        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            // Version 0: cores fall back to SET_VARIABLES, whose
            // "Description; default|other|..." strings carry everything the
            // frontend needs.
            *static_cast<unsigned*>(data) = 0;
            return true;

        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            std::lock_guard<std::mutex> lock(options_mutex_);
            for (auto* v = static_cast<const retro_variable*>(data); v && v->key; v++) {
                if (!v->value)
                    continue;
                declared_[v->key] = v->value;
                defaults_[v->key] = DefaultFromDeclaration(v->value);
            }
            options_dirty_ = true;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* var = static_cast<retro_variable*>(data);
            if (!var || !var->key)
                return false;
            std::lock_guard<std::mutex> lock(options_mutex_);
            std::string value;
            if (auto o = overrides_.find(var->key); o != overrides_.end())
                value = o->second;
            else if (auto d = defaults_.find(var->key); d != defaults_.end())
                value = d->second;
            else {
                var->value = nullptr;
                return false;
            }
            // The pointer handed out must stay valid until the next call for
            // the same key; std::map nodes never move.
            std::string& slot = value_cache_[var->key];
            slot = value;
            var->value = slot.c_str();
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            std::lock_guard<std::mutex> lock(options_mutex_);
            *static_cast<bool*>(data) = options_dirty_;
            options_dirty_ = false;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
            return true;

        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
            if (data)
                *static_cast<unsigned*>(data) = 1;
            return true;
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
            disk_ = {};
            if (data)
                std::memcpy(&disk_, data, sizeof(retro_disk_control_callback));
            return true;
        }
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: {
            if (data)
                disk_ = *static_cast<const retro_disk_control_ext_callback*>(data);
            return true;
        }

        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            const auto* map = static_cast<const retro_memory_map*>(data);
            mem_descs_.clear();
            mem_names_.clear();
            if (map && map->descriptors) {
                mem_descs_.assign(map->descriptors, map->descriptors + map->num_descriptors);
                mem_names_.reserve(mem_descs_.size());
                for (auto& d : mem_descs_) {
                    mem_names_.push_back(d.addrspace ? d.addrspace : "");
                    d.addrspace = d.addrspace ? mem_names_.back().c_str() : nullptr;
                }
            }
            mem_map_.descriptors = mem_descs_.data();
            mem_map_.num_descriptors = static_cast<unsigned>(mem_descs_.size());
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
            return true;

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;

        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *static_cast<unsigned*>(data) = RETRO_LANGUAGE_PORTUGUESE_BRAZIL;
            return true;

        case RETRO_ENVIRONMENT_GET_USERNAME:
            *static_cast<const char**>(data) = "Cartucho";
            return true;

        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            av_info_.geometry = *static_cast<const retro_game_geometry*>(data);
            return true;

        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            av_info_ = *static_cast<const retro_system_av_info*>(data);
            return true;

        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const auto* msg = static_cast<const retro_message*>(data);
            if (msg && msg->msg) {
                std::lock_guard<std::mutex> lock(message_mutex_);
                message_ = msg->msg;
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
            const auto* msg = static_cast<const retro_message_ext*>(data);
            if (msg && msg->msg && msg->target != RETRO_MESSAGE_TARGET_LOG) {
                std::lock_guard<std::mutex> lock(message_mutex_);
                message_ = msg->msg;
            }
            if (msg && msg->msg)
                PlatformLog(RETRO_LOG_INFO, msg->msg);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
            *static_cast<unsigned*>(data) = 1;
            return true;

        case RETRO_ENVIRONMENT_SHUTDOWN:
            shutdown_requested_ = true;
            return true;

        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE: {
            auto* s = static_cast<retro_sensor_interface*>(data);
            s->set_sensor_state = &CoreHostCallbacks::SetSensorState;
            s->get_sensor_input = &CoreHostCallbacks::GetSensorInput;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            static_cast<retro_rumble_interface*>(data)->set_rumble_state = &CoreHostCallbacks::SetRumble;
            return true;

        case RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE: {
            auto* m = static_cast<retro_microphone_interface*>(data);
            if (!m || m->interface_version != RETRO_MICROPHONE_INTERFACE_VERSION)
                return false;
            m->open_mic = &CoreHostCallbacks::OpenMic;
            m->close_mic = &CoreHostCallbacks::CloseMic;
            m->get_params = &CoreHostCallbacks::GetMicParams;
            m->set_mic_state = &CoreHostCallbacks::SetMicState;
            m->get_mic_state = &CoreHostCallbacks::GetMicState;
            m->read_mic = &CoreHostCallbacks::ReadMic;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
            *static_cast<bool*>(data) = true;
            return true;

        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            *static_cast<int*>(data) = av_flags_;
            return true;

        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            *static_cast<bool*>(data) = audio_muted_;
            return true;

        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
            *static_cast<float*>(data) = 60.0f;
            return true;

        default: {
            // Unknown or unsupported: answer "no" so the core uses its
            // fallback. Log each command once to help with new cores.
            static std::set<unsigned> logged;
            if (logged.insert(cmd).second) {
                char text[96];
                std::snprintf(text, sizeof(text), "Unhandled environment command %u",
                              cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL);
                PlatformLog(RETRO_LOG_DEBUG, text);
            }
            return false;
        }
    }
}

void CoreHost::VideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (!data)
        return;  // duplicate frame: keep showing the last one
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        frame_.hw = true;
        frame_.width = width;
        frame_.height = height;
        frame_.serial++;
        return;
    }
    frame_.hw = false;
    frame_.width = width;
    frame_.height = height;
    frame_.rgba.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* dst = frame_.rgba.data();
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (unsigned y = 0; y < height; y++, src += pitch) {
        switch (pixel_format_) {
            case RETRO_PIXEL_FORMAT_XRGB8888: {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(src);
                for (unsigned x = 0; x < width; x++) {
                    const uint32_t p = row[x];
                    *dst++ = static_cast<uint8_t>(p >> 16);
                    *dst++ = static_cast<uint8_t>(p >> 8);
                    *dst++ = static_cast<uint8_t>(p);
                    *dst++ = 0xff;
                }
                break;
            }
            case RETRO_PIXEL_FORMAT_RGB565: {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(src);
                for (unsigned x = 0; x < width; x++) {
                    const uint32_t p = row[x];
                    *dst++ = static_cast<uint8_t>(Expand5(p >> 11));
                    *dst++ = static_cast<uint8_t>(Expand6((p >> 5) & 0x3f));
                    *dst++ = static_cast<uint8_t>(Expand5(p & 0x1f));
                    *dst++ = 0xff;
                }
                break;
            }
            default: {  // 0RGB1555
                const uint16_t* row = reinterpret_cast<const uint16_t*>(src);
                for (unsigned x = 0; x < width; x++) {
                    const uint32_t p = row[x];
                    *dst++ = static_cast<uint8_t>(Expand5((p >> 10) & 0x1f));
                    *dst++ = static_cast<uint8_t>(Expand5((p >> 5) & 0x1f));
                    *dst++ = static_cast<uint8_t>(Expand5(p & 0x1f));
                    *dst++ = 0xff;
                }
                break;
            }
        }
    }
    frame_.serial++;
}

size_t CoreHost::AudioBatch(const int16_t* data, size_t frames) {
    if (!audio_muted_ && !audio_suppressed_)
        audio_.Push(data, frames);
    if (audio_tap_ && !audio_suppressed_)
        audio_tap_(audio_tap_user_, data, frames);
    return frames;
}

int16_t CoreHost::InputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0)
        return 0;
    switch (device) {
        case RETRO_DEVICE_JOYPAD: {
            const uint32_t mask = buttons_.load(std::memory_order_relaxed);
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
                return static_cast<int16_t>(mask & 0xffff);
            return (id < 16 && (mask & (1u << id))) ? 1 : 0;
        }
        case RETRO_DEVICE_ANALOG: {
            if (index > RETRO_DEVICE_INDEX_ANALOG_RIGHT)
                return 0;
            const int32_t packed = analog_[index].load(std::memory_order_relaxed);
            return static_cast<int16_t>(id == RETRO_DEVICE_ID_ANALOG_X ? (packed & 0xffff)
                                                                       : ((packed >> 16) & 0xffff));
        }
        case RETRO_DEVICE_POINTER: {
            if (index != 0)
                return 0;
            const int32_t packed = pointer_.load(std::memory_order_relaxed);
            switch (id) {
                case RETRO_DEVICE_ID_POINTER_X:
                    return static_cast<int16_t>(packed & 0xffff);
                case RETRO_DEVICE_ID_POINTER_Y:
                    return static_cast<int16_t>((packed >> 16) & 0xffff);
                case RETRO_DEVICE_ID_POINTER_PRESSED:
                    return pointer_pressed_.load(std::memory_order_relaxed) ? 1 : 0;
                case RETRO_DEVICE_ID_POINTER_COUNT:
                    return pointer_pressed_.load(std::memory_order_relaxed) ? 1 : 0;
                default:
                    return 0;
            }
        }
        default:
            return 0;
    }
}

// --- Trampolines -----------------------------------------------------------------

bool CoreHost::EnvTrampoline(unsigned cmd, void* data) {
    return g_current ? g_current->Environment(cmd, data) : false;
}

void CoreHost::VideoTrampoline(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (g_current)
        g_current->VideoRefresh(data, w, h, pitch);
}

void CoreHost::AudioSampleTrampoline(int16_t l, int16_t r) {
    const int16_t s[2] = {l, r};
    if (g_current)
        g_current->AudioBatch(s, 1);
}

size_t CoreHost::AudioBatchTrampoline(const int16_t* data, size_t frames) {
    return g_current ? g_current->AudioBatch(data, frames) : frames;
}

void CoreHost::InputPollTrampoline() {}

int16_t CoreHost::InputStateTrampoline(unsigned port, unsigned device, unsigned index,
                                       unsigned id) {
    return g_current ? g_current->InputState(port, device, index, id) : 0;
}

uintptr_t CoreHost::HwFramebufferTrampoline() {
    return (g_current && g_current->hw_hooks_.current_framebuffer)
               ? g_current->hw_hooks_.current_framebuffer()
               : 0;
}

retro_proc_address_t CoreHost::HwProcTrampoline(const char* sym) {
    return (g_current && g_current->hw_hooks_.get_proc_address)
               ? g_current->hw_hooks_.get_proc_address(sym)
               : nullptr;
}

void CoreHost::LogTrampoline(enum retro_log_level level, const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    // Cores end most lines with '\n'; the platform logger adds its own.
    size_t len = std::strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
        text[--len] = '\0';
    PlatformLog(level, text);
}

}  // namespace cartucho
