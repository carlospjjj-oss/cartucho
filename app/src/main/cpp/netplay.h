// Cartucho - two players on two devices, over the network, with rollback.
//
// Both devices run the same game from the same starting state. Each frame
// every device sends its own controller input for a few frames ahead (the
// input delay) and runs the frame with the other player's input if it has
// arrived, or a guess (their last input) if not. When the real input turns
// out different from the guess, the device goes back to the saved state of
// that frame and runs the frames again with the right input ("rollback"),
// all within one display refresh, so the game never waits for the network
// unless the other player falls far behind.
//
// The host is player 1 and sends its state when the other player joins;
// every two seconds both compare a checksum of a confirmed state and the
// host sends its state again if they ever differ.
//
// Threading: Host()/Join()/Stop() and the getters from any thread; Poll()
// and Advance() from the emulation thread. A network thread connects and
// reads; the emulation thread writes.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cartucho {

struct NetInput {
    uint32_t buttons = 0;
    int16_t ax = 0, ay = 0;  // left stick
    bool operator==(const NetInput& o) const { return buttons == o.buttons && ax == o.ax && ay == o.ay; }
    bool operator!=(const NetInput& o) const { return !(*this == o); }
};

// What Netplay drives: the emulated machine.
class NetMachine {
public:
    virtual ~NetMachine() = default;
    virtual bool Save(std::vector<uint8_t>* out) = 0;
    virtual bool Load(const std::vector<uint8_t>& data) = 0;
    // Runs one frame with these inputs for player 1 and 2. `video` false:
    // the picture is not needed; `audio` false: the sound is dropped.
    virtual void Run(const NetInput inputs[2], bool video, bool audio) = 0;
};

class Netplay {
public:
    enum class State : int {
        kIdle = 0,
        kWaiting = 1,     // host: waiting for the other player
        kConnecting = 2,  // guest: reaching the host
        kSyncing = 3,     // exchanging the game and the starting state
        kPlaying = 4,
        kClosed = 5,      // the other player left (or the connection broke)
        kFailed = 6,      // could not start: see message()
    };
    static constexpr int kMaxRollback = 8;   // frames run again at most
    static constexpr int kDefaultDelay = 2;  // input delay, frames

    Netplay();
    ~Netplay();
    Netplay(const Netplay&) = delete;
    Netplay& operator=(const Netplay&) = delete;

    // `game` identifies the game (core and content); both sides must match.
    bool Host(int port, const std::string& game);
    bool Join(const std::string& host, int port, const std::string& game);
    void Stop();

    State state() const { return state_.load(); }
    bool active() const {
        const State s = state_.load();
        return s == State::kWaiting || s == State::kConnecting || s == State::kSyncing || s == State::kPlaying;
    }
    bool playing() const { return state_.load() == State::kPlaying; }
    bool is_host() const { return host_; }
    int local_player() const { return host_ ? 0 : 1; }
    int port() const { return port_.load(); }
    int rtt_ms() const { return rtt_ms_.load(); }
    // The other player is too far behind (or paused): the game waits.
    bool waiting() const { return waiting_.load(); }
    // Frames run again so far, and states resent after a mismatch (stats).
    int rollbacks() const { return rollbacks_.load(); }
    int resyncs() const { return resyncs_.load(); }
    void set_delay(int frames) { delay_ = frames < 0 ? 0 : frames > 6 ? 6 : frames; }
    std::string message();

    // Emulation thread, every display refresh while active(): connection
    // handshake, starting state, pings.
    void Poll(NetMachine& m, double now);
    // Emulation thread: one emulated frame with this device's input. False
    // when it had to wait for the other player (nothing was run).
    bool Advance(NetMachine& m, const NetInput& local);
    // Next frame number (tests).
    int frame() const { return cur_; }

private:
    struct Message {
        uint8_t type = 0;
        std::vector<uint8_t> data;
    };
    enum : uint8_t { kHello = 1, kState = 2, kInput = 3, kCrc = 4, kPing = 5, kPong = 6, kBye = 7, kReady = 8 };

    void NetThread(std::string host, int port);
    bool ReadLoop(int fd);
    void Send(uint8_t type, const void* data, size_t size);
    void Fail(State s, const std::string& why);
    void Handle(NetMachine& m, Message& msg, double now);
    void StartPlaying();
    void RunAt(NetMachine& m, int f, bool video, bool audio);
    void Resimulate(NetMachine& m, int from);
    void CheckCrc();
    void CompareCrc(int f);
    NetInput LocalAt(int f) const;
    static uint32_t Crc32(const uint8_t* data, size_t size);

    static constexpr int kRing = 32;  // frames kept (states and inputs)
    struct Slot {
        int frame = -1;
        std::vector<uint8_t> state;  // before running the frame
        NetInput local;
        bool have_local = false;
        NetInput remote;
        bool have_remote = false;
        NetInput used_remote;        // what the frame was run with
        uint32_t crc = 0;
        bool have_crc = false;
    };
    Slot& slot(int f) { return ring_[((f % kRing) + kRing) % kRing]; }
    const Slot& slot(int f) const { return ring_[((f % kRing) + kRing) % kRing]; }

    std::atomic<State> state_{State::kIdle};
    bool host_ = false;
    std::string game_;
    std::atomic<int> port_{0};
    std::atomic<int> rtt_ms_{-1};
    std::atomic<bool> waiting_{false};
    std::atomic<int> rollbacks_{0};
    std::atomic<int> resyncs_{0};
    int delay_ = kDefaultDelay;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> fd_{-1};
    int listen_fd_ = -1;
    std::mutex send_mutex_;
    std::mutex queue_mutex_;
    std::deque<Message> queue_;
    std::mutex message_mutex_;
    std::string message_;

    // Emulation-thread state.
    Slot ring_[kRing];
    int cur_ = 0;                  // next frame to run
    int remote_upto_ = -1;         // last frame with the other player's input
    NetInput last_remote_;
    int rollback_from_ = 1 << 30;
    int next_crc_ = 120;
    double last_ping_ = 0.0;
    double last_resync_ = -100.0;
    bool state_sent_ = false;
    int skip_counter_ = 0;
    int remote_advantage_ = 0;
    double last_now_ = 0.0;
    std::vector<std::pair<int, uint32_t>> remote_crcs_;
};

}  // namespace cartucho
