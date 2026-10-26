// FramePacer: right number of emulated frames at 60/90/120/144 Hz displays,
// fast-forward, slow devices, and the audio nudge.
#include <cmath>
#include <cstdio>

#include "session.h"

namespace cartucho {
void PlatformLog(int, const char*) {}
retro_proc_address_t PlatformGetProcAddress(const char*) { return nullptr; }
}  // namespace cartucho

using cartucho::FramePacer;

static int failures = 0;

static double Run(double display_hz, double core_fps, double speed, double fill, double seconds,
                  int* max_per_refresh = nullptr) {
    FramePacer p;
    long frames = 0;
    int max_n = 0;
    const int refreshes = static_cast<int>(display_hz * seconds);
    for (int i = 0; i <= refreshes; i++) {
        const int n = p.Step(i / display_hz, core_fps, speed, fill);
        frames += n;
        max_n = n > max_n ? n : max_n;
    }
    if (max_per_refresh)
        *max_per_refresh = max_n;
    return frames / seconds;
}

static void Expect(const char* what, double got, double want, double tol) {
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("%s  %-40s %.2f (want %.2f)\n", ok ? "ok  " : "FAIL", what, got, want);
    failures += !ok;
}

int main() {
    const double gba = 59.7275;
    for (double hz : {60.0, 90.0, 120.0, 144.0}) {
        char what[64];
        int max_n = 0;
        std::snprintf(what, sizeof(what), "GBA speed on %.0f Hz display", hz);
        Expect(what, Run(hz, gba, 1.0, -1, 60, &max_n), gba, 0.05);
        Expect("  at most 2 frames per refresh", max_n, hz >= 60 ? (hz < 61 ? 2 : 1) : 2, 1);
    }
    Expect("fast-forward 3x on 60 Hz", Run(60, gba, 3.0, -1, 30), gba * 3, 0.5);
    // A 30 Hz device (too slow): runs at most 2 per refresh, no backlog.
    Expect("slow device caps at 2 per refresh", Run(20, gba, 1.0, -1, 30), 40, 0.5);
    Expect("audio starving speeds up 1%", Run(60, gba, 1.0, 0.0, 120), gba * 1.01, 0.05);
    Expect("audio full slows down 1%", Run(60, gba, 1.0, 3.0, 120), gba * 0.99, 0.05);
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
