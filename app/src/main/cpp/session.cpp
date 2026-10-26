#include "session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "platform.h"

namespace cartucho {

// --- FramePacer --------------------------------------------------------------

int FramePacer::Step(double now, double core_fps, double speed, double audio_fill) {
    if (!have_last_) {
        have_last_ = true;
        last_ = now;
        credit_ = 1.0;  // show something on the very first refresh
    }
    // A long gap (app paused, debugger, slow first frame) is not owed time.
    const double dt = std::clamp(now - last_, 0.0, 0.25);
    last_ = now;

    // Follow the audio clock: run up to 1% fast while the queue is short and
    // up to 1% slow while it piles up. Inaudible, and it keeps the two locked.
    double adjust = 1.0;
    if (audio_fill >= 0.0 && speed <= 1.0)
        adjust = std::clamp(1.0 + (1.0 - audio_fill) * 0.01, 0.99, 1.01);

    credit_ += dt * core_fps * speed * adjust;
    const int max_frames = speed > 1.0 ? static_cast<int>(std::ceil(speed)) + 1 : 2;
    int frames = static_cast<int>(std::floor(credit_));
    frames = std::clamp(frames, 0, max_frames);
    credit_ -= frames;
    // Too slow to keep up: don't build a backlog that would play back as a
    // burst of fast motion later.
    if (credit_ > 1.0)
        credit_ = 1.0;
    return frames;
}

// --- Session -----------------------------------------------------------------

Session::Session() : host_(std::make_unique<CoreHost>()) {}

Session::~Session() {
    if (host_ && loaded_)
        host_->UnloadGame();
}

bool Session::Open(const std::string& core_path, const std::string& system_dir,
                   const std::string& save_dir, std::string* error) {
    host_->SetDirectories(system_dir, save_dir);
    host_->SetHwHooks({[this] { return static_cast<uintptr_t>(presenter_.hw_framebuffer()); },
                       [](const char* sym) { return PlatformGetProcAddress(sym); }});
    if (!host_->LoadCore(core_path, error))
        return false;
    link_start_ = reinterpret_cast<LinkStartFn>(host_->Symbol("cartucho_link_start"));
    link_poll_ = reinterpret_cast<LinkPollFn>(host_->Symbol("cartucho_link_poll"));
    link_frame_ = reinterpret_cast<LinkVoidFn>(host_->Symbol("cartucho_link_frame"));
    link_stop_ = reinterpret_cast<LinkVoidFn>(host_->Symbol("cartucho_link_stop"));
    link_active_ = reinterpret_cast<LinkIntFn>(host_->Symbol("cartucho_link_active"));
    return true;
}

bool Session::link_supported() const {
    return link_start_ && link_poll_ && link_frame_ && link_stop_ && link_active_;
}

std::string Session::link_message() {
    std::lock_guard<std::mutex> lock(link_mutex_);
    return link_message_;
}

void Session::LinkStart(int mode, bool server, int players, const std::string& host, int port) {
    if (!link_supported()) {
        link_state_ = 1;
        std::lock_guard<std::mutex> lock(link_mutex_);
        link_message_ = "This emulator has no link support";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(link_mutex_);
        link_message_.clear();
    }
    // A generous timeout: phones on Wi-Fi have jittery latency.
    link_state_ = link_start_(mode, server ? 1 : 0, players, host.c_str(), port, 2000);
    if (link_state_ == 1) {
        std::lock_guard<std::mutex> lock(link_mutex_);
        link_message_ = server ? "Could not open the port" : "Could not reach the host";
    }
}

void Session::LinkStop() {
    if (link_stop_)
        link_stop_();
    link_state_ = -1;
}

// Render thread, every refresh (also while paused, so connecting proceeds
// while the game waits).
void Session::LinkStep() {
    const int state = link_state_;
    if (state == 2) {
        char message[256] = {};
        const int next = link_poll_(message, sizeof(message));
        if (message[0]) {
            std::lock_guard<std::mutex> lock(link_mutex_);
            link_message_ = message;
        }
        link_state_ = next;
    } else if (state == 0) {
        link_frame_();
        if (!link_active_()) {
            link_state_ = 1;
            std::lock_guard<std::mutex> lock(link_mutex_);
            link_message_ = "The other player disconnected";
        }
    }
}

bool Session::OnSurfaceCreated(const std::string& rom, const std::string& save,
                               std::string* error) {
    // For error reports: which GPU and driver the game runs on.
    {
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        std::string line = std::string("GPU: ") + (renderer ? renderer : "?") + " / " +
                           (version ? version : "?");
        PlatformLog(1, line.c_str());
    }
    if (!presenter_.Init()) {
        *error = "Could not set up the display";
        return false;
    }
    if (!loaded_) {
        if (!host_->LoadGame(rom, save, error))
            return false;
        loaded_ = true;
        SetupRewind();
    }
    if (host_->uses_hw_render()) {
        const auto& g = host_->av_info().geometry;
        const auto& hw = host_->hw_render();
        if (!presenter_.CreateHwFramebuffer(std::max(g.max_width, g.base_width),
                                            std::max(g.max_height, g.base_height), hw.depth,
                                            hw.stencil)) {
            *error = "Could not create the 3D framebuffer";
            return false;
        }
        host_->ContextReset();
    }
    pacer_.Reset();
    return true;
}

void Session::SetupRewind() {
    // A state every couple of frames for small consoles, less often for
    // bigger ones; none for cores whose states are too big to keep many of
    // (3DS states are tens of MB).
    const size_t size = host_->StateSize();
    rewind_available_ = size > 0 && size <= (24u << 20);
    // Big states (DS, ~19 MB) are copied less often; the copy itself happens
    // on this thread, the compression on the rewind worker.
    rewind_interval_ = size < (1u << 20) ? 2 : 10;
    char line[96];
    std::snprintf(line, sizeof(line), "Rewind: state %zu KB, %s", size / 1024,
                  rewind_available_ ? "on" : "off (state too big)");
    PlatformLog(1, line);
}

bool Session::run_ahead_possible() {
    const size_t size = host_->StateSize();
    return size > 0 && size <= RunAhead::kMaxStateBytes && !host_->uses_hw_render();
}

void Session::RunOneFrame() {
    // Never with a link session: replaying frames would replay link traffic.
    const int link = link_state_;
    run_ahead_.set_frames(link == 0 || link == 2 ? 0 : run_ahead_frames_.load());
    run_ahead_.RunFrame(*host_);
    if (rewind_available_ && rewind_enabled_ && ++frames_since_capture_ >= rewind_interval_) {
        frames_since_capture_ = 0;
        // Run-ahead already saved the real state; reuse it.
        if (run_ahead_.have_real_state())
            rewind_.Push(run_ahead_.real_state());
        else if (host_->SerializeTo(&state_buf_))
            rewind_.Push(state_buf_);
    }
}

bool Session::Screenshot(std::vector<uint8_t>* rgba, int* width, int* height) {
    const VideoFrame& f = host_->frame();
    if (f.width == 0 || f.height == 0)
        return false;
    *width = static_cast<int>(f.width);
    *height = static_cast<int>(f.height);
    if (!f.hw) {
        *rgba = f.rgba;
        return !rgba->empty();
    }
    return presenter_.ReadHwFrame(f.width, f.height, host_->hw_render().bottom_left_origin, rgba);
}

void Session::OnSurfaceChanged(int width, int height) {
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
}

void Session::set_paused(bool paused) {
    paused_ = paused;
    // The pacer belongs to the render thread; ask it to start over there.
    reset_pacer_ = true;
}

void Session::set_speed(double speed) {
    speed_ = std::clamp(speed, 0.25, 8.0);
    host_->SetAudioMuted(speed_ > 1.0);
}

void Session::Frame(double now) {
    if (reset_pacer_.exchange(false))
        pacer_.Reset();
    if (link_supported())
        LinkStep();
    if (loaded_ && !paused_) {
        if (host_->uses_hw_render()) {
            // A hardware core may grow its picture (resolution scale changed
            // in the options); follow it.
            const auto& g = host_->av_info().geometry;
            const auto& hw = host_->hw_render();
            presenter_.CreateHwFramebuffer(std::max(g.max_width, g.base_width),
                                           std::max(g.max_height, g.base_height), hw.depth,
                                           hw.stencil);
        }
        int frames;
        if (rewinding_ && rewind_available_) {
            // One step back per refresh: interval x faster than real time.
            frames = 0;
            host_->SetAudioMuted(true);
            if (rewind_.Pop(&state_buf_) && host_->UnserializeFrom(state_buf_)) {
                host_->RunFrame();  // draw the restored moment
                frames_since_capture_ = 0;
            }
            pacer_.Reset();
        } else {
            host_->SetAudioMuted(speed_ > 1.0);
            frames = pacer_.Step(now, host_->fps(), speed_.load(), audio_fill_);
            for (int i = 0; i < frames; i++)
                RunOneFrame();
        }
        fps_window_frames_ += frames;
        if (now - fps_window_start_ >= 1.0) {
            measured_fps_ = fps_window_frames_ / (now - fps_window_start_);
            fps_window_start_ = now;
            fps_window_frames_ = 0;
        }
        // Battery saves reach the disk within a few seconds of the game
        // writing them, not only when leaving.
        if (now - last_flush_ > 5.0) {
            last_flush_ = now;
            host_->FlushSaveRam();
        }
    }
    presenter_.Draw(*host_, width_, height_);
}

void Session::Touch(float x, float y, bool pressed) {
    const Rect r = presenter_.viewport();
    if (r.w <= 0 || r.h <= 0) {
        host_->SetPointer(-0x8000, -0x8000, false);
        return;
    }
    const float nx = (x - r.x) / r.w;
    const float ny = (y - r.y) / r.h;
    if (nx < 0.f || nx > 1.f || ny < 0.f || ny > 1.f) {
        // libretro's "outside the picture" value.
        host_->SetPointer(-0x8000, -0x8000, false);
        return;
    }
    host_->SetPointer(static_cast<int16_t>(std::lround((nx * 2.f - 1.f) * 0x7fff)),
                      static_cast<int16_t>(std::lround((ny * 2.f - 1.f) * 0x7fff)), pressed);
}

}  // namespace cartucho
