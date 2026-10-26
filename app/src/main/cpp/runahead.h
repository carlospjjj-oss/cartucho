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

    // Runs one real frame (with run-ahead when on and possible). After it,
    // `real_state()` holds the real state when run-ahead ran.
    void RunFrame(CoreHost& host);
    bool have_real_state() const { return have_state_; }
    const std::vector<uint8_t>& real_state() const { return state_; }

private:
    int frames_ = 0;
    std::vector<uint8_t> state_;
    bool have_state_ = false;
};

}  // namespace cartucho
