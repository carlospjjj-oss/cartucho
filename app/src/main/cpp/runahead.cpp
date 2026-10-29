#include "runahead.h"

#include "core_host.h"

namespace cartucho {

bool RunAhead::Verify(CoreHost& host) {
    // From state S: run the frame (sound off) the normal way -> A. Back to S
    // and run it again, the way run-ahead would -> B (the run the player
    // keeps). They must match. Loading a state may tidy a few bytes on its
    // own (VBA-M's GBA core does); those, seen by saving right after the
    // load, are not counted.
    if (!host.SerializeTo(&state_))
        return false;
    host.SetAudioSuppressed(true);
    host.RunFrame();
    host.SetAudioSuppressed(false);
    const bool got_a = host.SerializeTo(&check_a_);
    pic_a_ = host.frame().rgba;
    if (!got_a || !host.UnserializeFrom(state_) || !host.SerializeTo(&reloaded_))
        return false;
    host.RunFrame();
    if (!host.SerializeTo(&check_b_))
        return false;
    if (pic_a_ != host.frame().rgba)
        return false;
    if (check_a_.size() != check_b_.size() || reloaded_.size() != state_.size())
        return false;
    for (size_t i = 0; i < check_a_.size(); i++) {
        if (check_a_[i] != check_b_[i] && (i >= state_.size() || state_[i] == reloaded_[i]))
            return false;
    }
    return true;
}

void RunAhead::RunFrame(CoreHost& host) {
    have_state_ = false;
    const size_t size = frames_ > 0 ? host.StateSize() : 0;
    if (frames_ == 0 || size == 0 || size > kMaxStateBytes || host.uses_hw_render() ||
        verdict_ == Verdict::kFailed) {
        host.RunFrame();
        return;
    }
    // First two seconds without run-ahead: a game that just started has
    // little state to lose and would pass any check. Then check before the
    // first use, and every ten seconds.
    if (verdict_ == Verdict::kUnknown && ++frames_since_check_ < 120) {
        host.RunFrame();
        return;
    }
    if (verdict_ == Verdict::kUnknown || ++frames_since_check_ >= 600) {
        frames_since_check_ = 0;
        const bool ok = Verify(host);
        verdict_ = ok ? Verdict::kOk : Verdict::kFailed;
        failed_now_ = !ok;
        return;  // this frame was run by Verify()
    }
    // The real frame: sound yes, picture not needed.
    host.SetAvFlags(CoreHost::kAvAudio);
    host.RunFrame();
    if (!host.SerializeTo(&state_)) {
        host.SetAvFlags(CoreHost::kAvVideo | CoreHost::kAvAudio);
        return;
    }
    // The frames ahead: silent; only the last one's picture is shown.
    host.SetAudioSuppressed(true);
    for (int i = 0; i < frames_; i++) {
        host.SetAvFlags(i + 1 == frames_ ? CoreHost::kAvVideo : 0);
        host.RunFrame();
    }
    host.SetAudioSuppressed(false);
    host.SetAvFlags(CoreHost::kAvVideo | CoreHost::kAvAudio);
    have_state_ = host.UnserializeFrom(state_);
}

}  // namespace cartucho
