// RetroAchievements plumbing with a real core and a fake server: sign-in with
// a token, game identified by hash, achievement set loaded, memory read
// through the core's memory map, an unlock sent and announced.
//   achievements_test vbam_libretro.so game.gb
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "achievements.h"
#include "core_host.h"
extern "C" {
#include "rc_libretro.h"
}

using namespace cartucho;

namespace cartucho {
void PlatformLog(int, const char* text) { std::fprintf(stderr, "%s\n", text); }
}

namespace {

int failures = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        std::printf((cond) ? "ok    " : "FAIL  ");          \
        std::printf(__VA_ARGS__);                           \
        std::printf("\n");                                  \
        if (!(cond)) failures++;                            \
    } while (0)

// One achievement: the byte at $C000 (work RAM) becomes 42.
const char* kSet =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Test Game\",\"ConsoleId\":4,"
    "\"ImageIconUrl\":\"http://server/Images/1.png\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
    "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"http://server/Images/1.png\",\"Achievements\":["
    "{\"ID\":5501,\"Title\":\"Resposta\",\"Description\":\"The answer\",\"Flags\":3,\"Points\":10,"
    "\"MemAddr\":\"0xHc000=42\",\"Author\":\"me\",\"BadgeName\":\"00001\",\"Created\":1,\"Modified\":1}"
    "],\"Leaderboards\":[]}]}";

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s core game\n", argv[0]);
        return 2;
    }
    CoreHost host;
    host.SetDirectories("/tmp", "/tmp");
    std::string error;
    if (!host.LoadCore(argv[1], &error) || !host.LoadGame(argv[2], "", &error)) {
        std::printf("FAIL  %s\n", error.c_str());
        return 1;
    }
    rc_libretro_init_verbose_message_callback([](const char* m) { std::fprintf(stderr, "RCV %s\n", m); });
    Achievements ra(&host);
    ra.Start("User", "ApiToken", 4 /* RC_CONSOLE_GAMEBOY */);

    std::vector<std::string> seen;
    std::vector<std::string> messages;
    bool award_sent = false;
    for (int frame = 0; frame < 400; frame++) {
        host.RunFrame();
        ra.DoFrame();
        // The app's HTTP thread: answer whatever was asked.
        Achievements::Request r;
        while (ra.TakeRequest(&r)) {
            const std::string q = r.post_data.empty() ? r.url : r.post_data;
            std::string kind = q.substr(0, q.find('&'));
            seen.push_back(kind);
            std::string body = "{\"Success\":true}";
            if (kind == "r=login2")
                body = "{\"Success\":true,\"User\":\"User\",\"Token\":\"ApiToken\",\"Score\":10,\"Messages\":0}";
            else if (kind == "r=achievementsets")
                body = kSet;
            else if (kind == "r=awardachievement") {
                award_sent = true;
                body = "{\"Success\":true,\"Score\":20,\"SoftcoreScore\":10,\"AchievementID\":5501,"
                       "\"AchievementsRemaining\":0}";
            }
            ra.Respond(r.id, 200, body);
        }
        // The game "does it" after a while.
        if (frame == 200) {
            const retro_memory_map* map = host.memory_map();
            bool poked = false;
            if (map) {
                for (unsigned i = 0; i < map->num_descriptors && !poked; i++) {
                    const auto& d = map->descriptors[i];
                    if (d.ptr && d.start <= 0xC000 && 0xC000 < d.start + d.len) {
                        static_cast<uint8_t*>(d.ptr)[d.offset + (0xC000 - d.start)] = 42;
                        poked = true;
                    }
                }
            }
            if (!poked) {
                size_t size = 0;
                uint8_t* ram = host.MemoryData(RETRO_MEMORY_SYSTEM_RAM, &size);
                if (ram && size) {
                    ram[0] = 42;
                    poked = true;
                }
            }
            CHECK(poked, "wrote 42 at $C000 (%s)", map ? "core memory map" : "system RAM");
        }
        for (std::string m = host.TakeMessage(); !m.empty(); m = host.TakeMessage())
            messages.push_back(m);
    }
    auto asked = [&](const char* k) {
        for (const auto& s : seen)
            if (s == k) return true;
        return false;
    };
    CHECK(asked("r=login2"), "signed in with the token");
    CHECK(asked("r=achievementsets"), "asked for the game's achievements by hash");
    CHECK(asked("r=startsession"), "started a session");
    CHECK(ra.active(), "game loaded, memory reachable");
    CHECK(award_sent, "unlock sent to the server");
    bool announced = false;
    for (const auto& m : messages) {
        std::printf("      message: %s\n", m.c_str());
        if (m.find("Resposta") != std::string::npos) announced = true;
    }
    CHECK(announced, "unlock announced on screen");
    const auto list = ra.List();
    CHECK(list.size() == 1 && list[0].rfind("1|Resposta", 0) == 0, "list shows it earned (%s)",
          list.empty() ? "empty" : list[0].c_str());
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
