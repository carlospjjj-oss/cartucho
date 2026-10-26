// Runs a libretro core headless through Cartucho's CoreHost, the same code the
// Android app uses, and checks the basics: the core loads, the game boots,
// frames and audio come out, input reaches the core, save states round-trip.
//
//   host_test <core.so> <rom> <out.ppm> [frames]
//
// Writes the last frame as a PPM so it can be looked at.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core_host.h"
#include "platform.h"

namespace cartucho {
void PlatformLog(int level, const char* text) {
    static const char* names[] = {"debug", "info", "warn", "error"};
    if (level >= 1 || std::getenv("CARTUCHO_DEBUG"))
        std::fprintf(stderr, "[%s] %s\n", names[level & 3], text);
}
}  // namespace cartucho

using cartucho::CoreHost;

static bool WritePpm(const char* path, const cartucho::VideoFrame& f) {
    FILE* out = std::fopen(path, "wb");
    if (!out)
        return false;
    std::fprintf(out, "P6\n%u %u\n255\n", f.width, f.height);
    for (size_t i = 0; i < static_cast<size_t>(f.width) * f.height; i++)
        std::fwrite(&f.rgba[i * 4], 1, 3, out);
    std::fclose(out);
    return true;
}

static int failures = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) {                                         \
            std::printf("ok    " __VA_ARGS__);              \
        } else {                                            \
            std::printf("FAIL  " __VA_ARGS__);              \
            failures++;                                     \
        }                                                   \
        std::printf("\n");                                  \
    } while (0)

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s core.so rom out.ppm [frames]\n", argv[0]);
        return 2;
    }
    const int frames = argc > 4 ? std::atoi(argv[4]) : 300;
    const std::string dir = "/tmp/cartucho-host-test";
    if (std::system(("mkdir -p " + dir).c_str()) != 0) return 2;

    CoreHost host;
    host.SetDirectories(dir, dir);
    std::string error;
    CHECK(host.LoadCore(argv[1], &error), "load core %s", error.c_str());
    if (failures)
        return 1;
    CHECK(host.LoadGame(argv[2], dir + "/game.srm", &error), "load game %s", error.c_str());
    if (failures)
        return 1;

    std::printf("      core %s %s, %.3f fps, %.0f Hz, aspect %.3f\n",
                host.system_info().library_name, host.system_info().library_version, host.fps(),
                host.sample_rate(), host.aspect());
    CHECK(!host.DeclaredOptions().empty(), "core declared %zu options",
          host.DeclaredOptions().size());

    size_t audio_frames = 0;
    std::vector<int16_t> sink(4096 * 2);
    for (int i = 0; i < frames; i++) {
        // Tap A now and then so menus advance, like a player would.
        host.SetButtons((i % 40) < 2 ? (1u << RETRO_DEVICE_ID_JOYPAD_A) : 0);
        host.RunFrame();
        audio_frames += host.PopAudio(sink.data(), 4096);
    }
    const auto& f = host.frame();
    CHECK(f.width > 0 && f.height > 0 && !f.rgba.empty(), "video %ux%u, %llu frames", f.width,
          f.height, static_cast<unsigned long long>(f.serial));

    // Not a blank screen.
    size_t distinct = 0;
    uint32_t first = 0;
    std::memcpy(&first, f.rgba.data(), 4);
    for (size_t i = 0; i < f.rgba.size(); i += 4) {
        uint32_t px;
        std::memcpy(&px, &f.rgba[i], 4);
        distinct += px != first;
    }
    CHECK(distinct > f.rgba.size() / 4 / 100, "picture is not blank (%zu differing pixels)",
          distinct);
    const double expected = host.sample_rate() * frames / host.fps();
    CHECK(audio_frames > expected * 0.9 && audio_frames < expected * 1.1,
          "audio %zu frames (expected about %.0f)", audio_frames, expected);

    // Save state round trip: the picture after loading must match the one at
    // save time once the same frame is re-run.
    const std::string state = dir + "/slot1.state";
    CHECK(host.SaveState(state, &error), "save state %s", error.c_str());
    host.SetButtons(0);
    host.RunFrame();
    const std::vector<uint8_t> after_save = host.frame().rgba;
    for (int i = 0; i < 60; i++) {
        host.SetButtons(1u << RETRO_DEVICE_ID_JOYPAD_START);
        host.RunFrame();
    }
    CHECK(host.LoadState(state, &error), "load state %s", error.c_str());
    host.SetButtons(0);
    host.RunFrame();
    CHECK(host.frame().rgba == after_save, "state restores the same picture");

    CHECK(!host.LoadState(dir + "/missing.state", &error), "missing state reported: %s",
          error.c_str());

    CHECK(WritePpm(argv[3], host.frame()), "wrote %s", argv[3]);
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
