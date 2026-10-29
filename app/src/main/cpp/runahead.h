// Cartucho - run-ahead: removes the input lag games build in.
//
// Most games react to a button one or two frames after reading it. Run-ahead
// hides that: each frame, run the real frame (audio on, picture off), save
// the state, run N more frames silently with the same input and show the last
// one's picture, then load the saved state again. The player sees the future
// their input causes N frames earlier. Costs N+1 frames plus a save and a
// load per frame, so it is offered for small consoles only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cartucho {

class CoreHost;

class RunAhead {
public:
    // Largest state worth saving and loading every frame.
    static constexpr size_t kMaxStateBytes = 2u << 20;

    void set_frames(int frames) { frames_ = frames < 0 ? 0 : (frames > 3 ? 3 : frames); }
    int frames() const { return frames_; }

    // Run-ahead only works if loading a state and running a frame lands
    // exactly where running the frame did; some cores lose bits of state
    // (VBA-M's Game Boy core, for one), and run-ahead then drifts into a
    // broken game (blank screen). Before switching on, and every few
    // seconds after, the frame is run twice from the same state and the
    // results compared; on any difference run-ahead stays off.
    enum class Verdict { kUnknown, kOk, kFailed };
    Verdict verdict() const { return verdict_; }
    // True when the last check just failed (to tell the player once).
    bool TakeFailure() {
        const bool f = failed_now_;
        failed_now_ = false;
        return f;
    }

    // Runs one real frame (with run-ahead when on and possible). After it,
    // `real_state()` holds the real state when run-ahead ran.
    void RunFrame(CoreHost& host);
    bool have_real_state() const { return have_state_; }
    const std::vector<uint8_t>& real_state() const { return state_; }

private:
    // Runs the frame twice from the same state; true if identical.
    bool Verify(CoreHost& host);

    int frames_ = 0;
    Verdict verdict_ = Verdict::kUnknown;
    bool failed_now_ = false;
    int frames_since_check_ = 0;
    std::vector<uint8_t> check_a_, check_b_, pic_a_, reloaded_;
    std::vector<uint8_t> state_;
    bool have_state_ = false;
};

}  // namespace cartucho
