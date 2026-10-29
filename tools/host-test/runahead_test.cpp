// Run-ahead must show exactly the picture the normal run shows N frames
// later, given the same input. Run twice in separate processes (a core keeps
// global state): once normally, once with run-ahead, and compare.
//   runahead_test <core.so> <rom> <frames-ahead>   (prints one hash per frame)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core_host.h"
#include "runahead.h"

namespace cartucho {
void PlatformLog(int, const char*) {}
}  // namespace cartucho

using namespace cartucho;

// Input changes every 40 frames, so frames ahead mostly share the input.
static uint32_t InputFor(int frame) {
    static const uint32_t seq[] = {0, 1u << RETRO_DEVICE_ID_JOYPAD_DOWN, 0, 1u << RETRO_DEVICE_ID_JOYPAD_A,
                                   0, 1u << RETRO_DEVICE_ID_JOYPAD_DOWN, 1u << RETRO_DEVICE_ID_JOYPAD_B, 0};
    return seq[(frame / 40) % 8];
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const int ahead = std::atoi(argv[3]);
    CoreHost host;
    host.SetDirectories("/tmp", "/tmp");
    std::string err;
    if (!host.LoadCore(argv[1], &err) || !host.LoadGame(argv[2], "", &err)) return 1;
    RunAhead ra;
    ra.set_frames(ahead);
    for (int f = 0; f < 400; f++) {
        // With run-ahead, the frames ahead see this frame's input, which is
        // what the player is pressing now.
        host.SetButtons(InputFor(f));
        ra.RunFrame(host);
        if (ra.TakeFailure()) std::fprintf(stderr, "verify failed at frame %d\n", f);
        uint64_t h = 1469598103934665603ull;
        for (uint8_t b : host.frame().rgba) h = (h ^ b) * 1099511628211ull;
        std::printf("%d %016llx\n", f, static_cast<unsigned long long>(h));
    }
    std::fprintf(stderr, "verdict %d\n", static_cast<int>(ra.verdict()));
    return 0;
}
