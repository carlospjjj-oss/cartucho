#include "session.h"

#include <algorithm>
#include <cmath>

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
    return host_->LoadCore(core_path, error);
}

bool Session::OnSurfaceCreated(const std::string& rom, const std::string& save,
                               std::string* error) {
    if (!presenter_.Init()) {
        *error = "Could not set up the display";
        return false;
    }
    if (!loaded_) {
        if (!host_->LoadGame(rom, save, error))
            return false;
        loaded_ = true;
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
        const int frames = pacer_.Step(now, host_->fps(), speed_.load(), audio_fill_);
        for (int i = 0; i < frames; i++)
            host_->RunFrame();
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
