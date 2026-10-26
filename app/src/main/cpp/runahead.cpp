#include "runahead.h"

#include "core_host.h"

namespace cartucho {

void RunAhead::RunFrame(CoreHost& host) {
    have_state_ = false;
    const size_t size = frames_ > 0 ? host.StateSize() : 0;
    if (frames_ == 0 || size == 0 || size > kMaxStateBytes || host.uses_hw_render()) {
        host.RunFrame();
        return;
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
