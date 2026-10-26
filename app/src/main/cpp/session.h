// Cartucho - one play session: the core, the picture and the timing.
//
// Driven from the render thread once per display refresh (Frame()), it decides
// how many emulated frames to run so the game goes at its own speed whatever
// the display rate (60, 90, 120 Hz...), nudged by the audio queue so sound and
// picture never drift apart, then draws.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "core_host.h"
#include "gl_presenter.h"
#include "rewind.h"

namespace cartucho {

class FramePacer {
public:
    // How many emulated frames to run for a display refresh at time `now`
    // (seconds). `audio_fill` is the audio queue level relative to its target
    // (1.0 = on target, <1 starving, >1 piling up), or a negative value when
    // there is no audio to follow.
    int Step(double now, double core_fps, double speed, double audio_fill);
    void Reset() { have_last_ = false; credit_ = 0.0; }

private:
    bool have_last_ = false;
    double last_ = 0.0;
    double credit_ = 0.0;  // emulated frames owed
};

class Session {
public:
    Session();
    ~Session();

    bool Open(const std::string& core_path, const std::string& system_dir,
              const std::string& save_dir, std::string* error);
    CoreHost& host() { return *host_; }
    GlPresenter& presenter() { return presenter_; }

    // GL thread: a (new) context is current. Loads the game the first time.
    bool OnSurfaceCreated(const std::string& rom, const std::string& save, std::string* error);
    void OnSurfaceChanged(int width, int height);
    // GL thread, once per display refresh.
    void Frame(double now);

    // Safe from any thread.
    void set_paused(bool paused);
    bool paused() const { return paused_; }
    void set_speed(double speed);
    double speed() const { return speed_; }
    // Audio queue level relative to target, fed by the platform each frame.
    void set_audio_fill(double fill) { audio_fill_ = fill; }

    // Maps a touch at surface pixel (x, y) to the core's pointer.
    void Touch(float x, float y, bool pressed);

    // --- Rewind -------------------------------------------------------------
    // Whether states are being recorded (off for cores with huge states).
    bool rewind_available() const { return rewind_available_; }
    void set_rewind_enabled(bool on) { rewind_enabled_ = on; }
    // While true, each refresh steps back instead of forward. Any thread.
    void set_rewinding(bool on) { rewinding_ = on; }
    // GL thread: after loading a state or resetting, the history is stale.
    void ClearRewind() { rewind_.Clear(); }

    // GL thread: the picture currently shown, as RGBA rows top to bottom.
    bool Screenshot(std::vector<uint8_t>* rgba, int* width, int* height);

    // --- Network link (patched VBA-M core only) ---------------------------
    bool link_supported() const;
    // GL thread. mode: 0 GBA cable, 1 GBA wireless adapter, 2 Game Boy.
    void LinkStart(int mode, bool server, int players, const std::string& host, int port);
    void LinkStop();
    // -1 off, 0 connected, 1 failed, 2 connecting. Any thread.
    int link_state() const { return link_state_; }
    std::string link_message();

    // Measured emulation speed (frames per second) over the last second.
    double measured_fps() const { return measured_fps_; }

private:
    std::unique_ptr<CoreHost> host_;
    GlPresenter presenter_;
    FramePacer pacer_;
    int width_ = 1, height_ = 1;
    bool loaded_ = false;
    std::atomic<bool> paused_{false};
    std::atomic<bool> reset_pacer_{false};
    std::atomic<double> speed_{1.0};
    double audio_fill_ = -1.0;
    double fps_window_start_ = 0.0;
    int fps_window_frames_ = 0;
    double measured_fps_ = 0.0;
    double last_flush_ = 0.0;

    void SetupRewind();
    void RunOneFrame();
    Rewind rewind_;
    bool rewind_available_ = false;
    std::atomic<bool> rewind_enabled_{true};
    std::atomic<bool> rewinding_{false};
    int rewind_interval_ = 2;
    int frames_since_capture_ = 0;
    std::vector<uint8_t> state_buf_;

    void LinkStep();
    using LinkStartFn = int (*)(int, int, int, const char*, int, int);
    using LinkPollFn = int (*)(char*, int);
    using LinkVoidFn = void (*)();
    using LinkIntFn = int (*)();
    LinkStartFn link_start_ = nullptr;
    LinkPollFn link_poll_ = nullptr;
    LinkVoidFn link_frame_ = nullptr;
    LinkVoidFn link_stop_ = nullptr;
    LinkIntFn link_active_ = nullptr;
    std::atomic<int> link_state_{-1};
    std::mutex link_mutex_;
    std::string link_message_;
};

}  // namespace cartucho
