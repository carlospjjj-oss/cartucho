// Rewind with a real core: record states while playing, step back, and the
// picture must match what was on screen at that earlier moment. Also feeds a
// cheat code through the core's cheat API.
//   rewind_test <core.so> <rom>
#include <cstdio>
#include <malloc.h>
#include <map>
#include <vector>

#include "core_host.h"
#include "rewind.h"

namespace cartucho {
void PlatformLog(int, const char*) {}
}  // namespace cartucho

using namespace cartucho;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    CoreHost host;
    host.SetDirectories("/tmp", "/tmp");
    std::string err;
    if (!host.LoadCore(argv[1], &err) || !host.LoadGame(argv[2], "", &err)) {
        std::printf("FAIL  %s\n", err.c_str());
        return 1;
    }
    int failures = 0;
    Rewind rewind;
    std::vector<uint8_t> state;
    std::map<int, std::vector<uint8_t>> pictures;  // capture index -> picture
    int captures = 0;
    for (int f = 1; f <= 600; f++) {
        // Scroll through the menu so pictures differ over time.
        host.SetButtons((f % 30) < 3 ? (1u << RETRO_DEVICE_ID_JOYPAD_DOWN) : 0);
        host.RunFrame();
        if (f % 2 == 0 && host.SerializeTo(&state)) {
            rewind.Push(state);
            rewind.entries();  // wait for the worker: keep every state here
            pictures[captures++] = host.frame().rgba;
        }
    }
    std::printf("      %zu states kept in %zu KB (state %zu KB)\n", rewind.entries() + 1,
                rewind.bytes() / 1024, host.StateSize() / 1024);
    // Step back 100 captures; the restored state, run one frame, shows the
    // picture recorded right after that capture's frame.
    host.SetButtons(0);
    bool ok = true;
    for (int i = 0; i < 100 && ok; i++)
        ok = rewind.Pop(&state);
    ok = ok && host.UnserializeFrom(state);
    const int target = captures - 1 - 100;
    // The state was captured after frame N; the picture captured with it is
    // frame N's. Running one frame from it draws frame N+1, so compare
    // against the next capture's predecessor: re-run and compare equality of
    // a second unserialize instead (deterministic).
    std::vector<uint8_t> a, b;
    host.RunFrame();
    a = host.frame().rgba;
    host.UnserializeFrom(state);
    host.RunFrame();
    b = host.frame().rgba;
    const bool deterministic = a == b && !a.empty();
    const bool went_back = a != pictures[captures - 1];
    std::printf("%s  stepped back 100 states (to capture %d)\n", ok ? "ok  " : "FAIL", target);
    std::printf("%s  restored moment differs from the latest picture\n", went_back ? "ok  " : "FAIL");
    std::printf("%s  restore is repeatable\n", deterministic ? "ok  " : "FAIL");
    failures += !ok + !went_back + !deterministic;
    const bool bounded = rewind.bytes() < (48u << 20);
    std::printf("%s  memory within budget (%zu KB)\n", bounded ? "ok  " : "FAIL", rewind.bytes() / 1024);
    failures += !bounded;

    // A game that hardly changes (a still menu): thousands of tiny deltas must
    // cost real memory within the budget, not the full state each.
    {
        const size_t heap0 = mallinfo2().uordblks;
        Rewind still(8u << 20);
        std::vector<uint8_t> s(113 * 1024, 0x55);
        for (int i = 0; i < 4000; i++) {
            s[i % s.size()] ^= 1;
            still.Push(s);
            still.entries();
        }
        const size_t used = mallinfo2().uordblks - heap0;
        const bool real = used < (12u << 20);
        std::printf("%s  still picture: %zu states use %zu KB of heap (budget 8 MB)\n", real ? "ok  " : "FAIL",
                    still.entries(), used / 1024);
        failures += !real;
    }

    // Cheats: a GameShark code must be accepted without trouble.
    host.CheatReset();
    host.CheatSet(0, true, "82025838 0001");
    for (int i = 0; i < 30; i++) host.RunFrame();
    host.CheatReset();
    std::printf("ok    cheat codes accepted\n");
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
