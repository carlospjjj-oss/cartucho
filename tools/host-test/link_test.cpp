// Two emulators linked over TCP, as on two phones: run one process as server
// and one as client (127.0.0.1), each with the patched VBA-M core.
//   link_test <core.so> <rom> server|client [port]
// Exits 0 once the session is up and both kept running linked for 2 s.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core_host.h"

namespace cartucho {
void PlatformLog(int level, const char* text) {
    if (level >= 2) std::fprintf(stderr, "[core] %s\n", text);
}
}  // namespace cartucho

using namespace cartucho;
using StartFn = int (*)(int, int, int, const char*, int, int);
using PollFn = int (*)(char*, int);
using FrameFn = void (*)();
using ActiveFn = int (*)();

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const bool server = std::strcmp(argv[3], "server") == 0;
    const int port = argc > 4 ? std::atoi(argv[4]) : 5738;
    const char* who = server ? "server" : "client";
    CoreHost host;
    host.SetDirectories("/tmp", "/tmp");
    std::string err;
    if (!host.LoadCore(argv[1], &err) || !host.LoadGame(argv[2], "", &err)) {
        std::printf("FAIL  %s: %s\n", who, err.c_str());
        return 1;
    }
    auto start = reinterpret_cast<StartFn>(host.Symbol("cartucho_link_start"));
    auto poll = reinterpret_cast<PollFn>(host.Symbol("cartucho_link_poll"));
    auto frame = reinterpret_cast<FrameFn>(host.Symbol("cartucho_link_frame"));
    auto active = reinterpret_cast<ActiveFn>(host.Symbol("cartucho_link_active"));
    if (!start || !poll || !frame || !active) {
        std::printf("FAIL  %s: core has no link extension\n", who);
        return 1;
    }
    int state = start(0, server, 2, "127.0.0.1", port, 1000);
    char msg[256] = {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (state == 2 && std::chrono::steady_clock::now() < deadline) {
        state = poll(msg, sizeof(msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("%s  %s: connect state %d \"%s\"\n", state == 0 ? "ok  " : "FAIL", who, state, msg);
    if (state != 0) return 1;
    // Check halfway through, then keep going so the peer (which may have
    // connected a moment later) is not left alone before its own check.
    bool still = false;
    for (int i = 0; i < 240; i++) {
        host.RunFrame();
        frame();
        if (i == 120) still = active() != 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::printf("%s  %s: still linked after 120 frames\n", still ? "ok  " : "FAIL", who);
    return still ? 0 : 1;
}
