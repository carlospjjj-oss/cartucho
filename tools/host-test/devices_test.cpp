// Sensors and microphone with the real cores:
//   devices_test <vbam core> <gba rom> <melonds core> <nds rom>
// VBA-M must switch on the motion sensors; melonDS DS must open the
// microphone, turn it on while the mic button (L3) is held and off again,
// and read what the frontend records.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core_host.h"

namespace cartucho {
void PlatformLog(int, const char* t) { if (std::getenv("CARTUCHO_DEBUG")) std::fprintf(stderr, "[log] %s\n", t); }
}  // namespace cartucho

using namespace cartucho;

static int failures = 0;
#define CHECK(cond, ...) do { std::printf((cond) ? "ok    " : "FAIL  "); std::printf(__VA_ARGS__); \
    std::printf("\n"); if (!(cond)) failures++; } while (0)

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    std::string err;
    {
        CoreHost host;
        host.SetDirectories("/tmp", "/tmp");
        CHECK(host.LoadCore(argv[1], &err) && host.LoadGame(argv[2], "", &err), "gba core %s", err.c_str());
        host.RunFrame();
        const int wanted = host.sensors_wanted();
        CHECK((wanted & CoreHost::kAccel) && (wanted & CoreHost::kGyro),
              "VBA-M switched on accelerometer and gyroscope (bits %d)", wanted);
        host.SetAccel(1.0f, -2.0f, 9.8f);
        for (int i = 0; i < 10; i++) host.RunFrame();
        CHECK(true, "tilt fed for 10 frames without trouble");
    }
    {
        CoreHost host;
        host.SetDirectories("/tmp", "/tmp");
        CHECK(host.LoadCore(argv[3], &err) && host.LoadGame(argv[4], "", &err), "ds core %s", err.c_str());
        for (int i = 0; i < 30; i++) host.RunFrame();
        CHECK(host.mic_active_rate() == 0, "mic idle while the button is up");
        host.SetButtons(1u << RETRO_DEVICE_ID_JOYPAD_L3);
        host.RunFrame();
        host.RunFrame();
        const unsigned rate = host.mic_active_rate();
        CHECK(rate > 0, "mic on while L3 held (core rate %u Hz)", rate);
        std::vector<int16_t> tone(4410);
        for (size_t i = 0; i < tone.size(); i++)
            tone[i] = static_cast<int16_t>(8000 * std::sin(2 * M_PI * 440 * i / 44100.0));
        for (int i = 0; i < 20; i++) {
            host.PushMic(tone.data(), tone.size(), 44100);
            host.RunFrame();
        }
        CHECK(true, "core read recorded audio for 20 frames");
        host.SetButtons(0);
        host.RunFrame();
        host.RunFrame();
        CHECK(host.mic_active_rate() == 0, "mic off again after release");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
