// Netplay: two devices in one process over localhost, one running slower,
// must end with the same game as a plain run with everyone's inputs; a
// device that goes its own way is put back on track; different games are
// refused.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <thread>

#include "netplay.h"

using namespace cartucho;

static int failures = 0;
#define CHECK(cond, ...)                         \
    do {                                         \
        std::printf((cond) ? "ok    " : "FAIL  "); \
        std::printf(__VA_ARGS__);                \
        std::printf("\n");                       \
        if (!(cond)) failures++;                 \
    } while (0)

// A tiny deterministic "game": a hash of everything pressed so far.
struct Machine : NetMachine {
    struct S { uint32_t frame = 0; uint64_t h = 1469598103934665603ull; int32_t x[2] = {0, 0}; } s;
    std::map<uint32_t, uint64_t> log;  // frame -> hash after it (last run wins)
    int runs = 0;
    bool Save(std::vector<uint8_t>* out) override {
        out->resize(sizeof(S));
        std::memcpy(out->data(), &s, sizeof(S));
        return true;
    }
    bool Load(const std::vector<uint8_t>& d) override {
        if (d.size() != sizeof(S)) return false;
        std::memcpy(&s, d.data(), sizeof(S));
        return true;
    }
    void Run(const NetInput in[2], bool, bool) override {
        for (int p = 0; p < 2; p++) {
            s.h = (s.h ^ in[p].buttons ^ (uint64_t(uint16_t(in[p].ax)) << 32)) * 1099511628211ull;
            s.x[p] += (in[p].buttons & 1) - ((in[p].buttons >> 1) & 1);
        }
        log[s.frame] = s.h;
        s.frame++;
        runs++;
    }
};

static NetInput Gen(int player, int frame) {
    if (frame < Netplay::kDefaultDelay) return NetInput{};
    uint32_t v = uint32_t(frame) * 2654435761u ^ uint32_t(player + 1) * 40503u;
    v ^= v >> 13;
    // Change only every few frames, like a person.
    uint32_t w = uint32_t(frame / 7) * 2654435761u ^ uint32_t(player + 1) * 97u;
    w ^= w >> 11;
    NetInput in;
    in.buttons = w & 0x0FFF;
    in.ax = int16_t((w >> 8) & 0x7FFF);
    (void)v;
    return in;
}

static double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static bool WaitFor(Netplay& a, Machine& ma, Netplay& b, Machine& mb, Netplay::State sa, Netplay::State sb) {
    for (int i = 0; i < 300; i++) {
        a.Poll(ma, Now());
        b.Poll(mb, Now());
        if (a.state() == sa && b.state() == sb) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static void Play(int frames, double guest_speed, int corrupt_at, int* rollbacks, int* resyncs) {
    Netplay host, guest;
    Machine mh, mg;
    // The guest starts from another state: it must take the host's.
    mg.s.h = 42;
    CHECK(host.Host(0, "core|game-crc"), "host listens");
    CHECK(host.port() > 0, "on port %d", host.port());
    guest.Join("127.0.0.1", host.port(), "core|game-crc");
    CHECK(WaitFor(host, mh, guest, mg, Netplay::State::kPlaying, Netplay::State::kPlaying), "both playing");
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(0, 1);
    bool corrupted = false;
    for (int tick = 0; tick < frames * 6 && (host.frame() < frames || guest.frame() < frames); tick++) {
        const double t = Now();
        host.Poll(mh, t);
        guest.Poll(mg, t);
        if (host.frame() < frames + 20) host.Advance(mh, Gen(0, host.frame() + Netplay::kDefaultDelay));
        if (guest.frame() < frames + 20 && u(rng) < guest_speed) guest.Advance(mg, Gen(1, guest.frame() + Netplay::kDefaultDelay));
        if (!corrupted && corrupt_at > 0 && guest.frame() == corrupt_at) {
            mg.s.h ^= 0xDEAD;  // the guest's game goes wrong
            corrupted = true;
        }
        if (tick % 3 == 0) std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    // Let the last inputs arrive and be run.
    for (int i = 0; i < 40; i++) {
        host.Poll(mh, Now());
        guest.Poll(mg, Now());
        host.Advance(mh, Gen(0, host.frame() + Netplay::kDefaultDelay));
        guest.Advance(mg, Gen(1, guest.frame() + Netplay::kDefaultDelay));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // The reference: one machine, everyone's real input.
    Machine ref;
    ref.s = mh.s;
    ref.s = Machine::S{};
    for (int f = 0; f < frames; f++) {
        NetInput in[2] = {Gen(0, f), Gen(1, f)};
        ref.Run(in, true, true);
    }
    const int check_from = corrupt_at > 0 ? corrupt_at + 360 : 0;
    int bad_h = 0, bad_g = 0, checked = 0;
    for (int f = check_from; f < frames - Netplay::kMaxRollback - 4; f++) {
        checked++;
        if (mh.log[f] != ref.log[f]) bad_h++;
        if (mg.log[f] != ref.log[f]) bad_g++;
    }
    CHECK(checked > 100, "%d frames compared", checked);
    CHECK(bad_h == 0, "host matches a plain run (%d frames differ)", bad_h);
    CHECK(bad_g == 0, "guest matches a plain run (%d frames differ)", bad_g);
    *rollbacks = guest.rollbacks() + host.rollbacks();
    *resyncs = guest.resyncs() + host.resyncs();
    host.Stop();
    guest.Stop();
}

int main() {
    int rb = 0, rs = 0;
    std::printf("-- even pace\n");
    Play(600, 1.0, 0, &rb, &rs);
    std::printf("   rollbacks %d, resyncs %d\n", rb, rs);
    std::printf("-- guest slower (rollbacks)\n");
    Play(900, 0.7, 0, &rb, &rs);
    CHECK(rb > 0, "frames run again: %d", rb);
    std::printf("-- guest goes wrong at frame 200\n");
    Play(1200, 0.9, 200, &rb, &rs);
    CHECK(rs > 0, "state sent again: %d", rs);

    std::printf("-- different games\n");
    Netplay host, guest;
    Machine mh, mg;
    host.Host(0, "core|A");
    guest.Join("127.0.0.1", host.port(), "core|B");
    bool refused = false;
    for (int i = 0; i < 300 && !refused; i++) {
        host.Poll(mh, Now());
        guest.Poll(mg, Now());
        refused = host.state() == Netplay::State::kFailed || guest.state() == Netplay::State::kFailed;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(refused, "refused: %s", (host.state() == Netplay::State::kFailed ? host.message() : guest.message()).c_str());

    std::printf("-- nobody there\n");
    Netplay lone;
    Machine ml;
    lone.Join("127.0.0.1", 1, "core|A");
    for (int i = 0; i < 300 && lone.state() == Netplay::State::kConnecting; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(lone.state() == Netplay::State::kFailed, "fails: %s", lone.message().c_str());

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
