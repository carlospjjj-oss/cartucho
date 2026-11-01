// Cartucho PC - finding link cable partners and online-play rooms on the
// local network, with the same UDP protocol as the phone (LinkDiscovery.kt)
// and the desktop VBA-M: a browser broadcasts {"vbam":"link-query","v":1} to
// port 5739; a waiting host answers {"vbam":"link-host","v":1,...}.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cartucho {

namespace Discovery {

constexpr int kPort = 5739;
constexpr int kLinkPort = 5738;
constexpr int kNetplayPort = 5740;
constexpr int kModeNetplay = 9;   // announcements of online-play rooms

// Cartucho link mode (0 GBA cable, 1 wireless adapter, 2 GB cable) <-> wire.
int WireMode(int mode);
int ModeFromWire(int wire);
std::string ModeName(int mode);

std::vector<std::string> LocalAddresses();   // IPv4, no loopback
std::vector<std::string> VpnAddresses();     // Tailscale/ZeroTier/Radmin...
std::string PublicAddress();                 // asks api.ipify.org (blocking)
// Name or address to an IPv4 address ("" when not found). Blocking.
std::string Resolve(const std::string& host);

// Java's String.hashCode() & 0xffffffff: how the phone tags a game id.
uint32_t JavaHash(const std::string& s);

struct Host {
    std::string address;
    int port = 0;
    int mode = -1;        // Cartucho mode, kModeNetplay for online rooms
    int players = 2;
    std::string game, name, version;
    uint32_t crc = 0;
    int64_t seen_ms = 0;
};

struct Announcement {
    int port = 0;
    int mode = 0;
    int players = 2;
    std::string game;
    uint32_t crc = 0;
};

// Answers queries until destroyed.
class Advertiser {
public:
    explicit Advertiser(const Announcement& a);
    ~Advertiser();

private:
    void Run();
    Announcement ann_;
    std::atomic<bool> running_{true};
    std::atomic<int> fd_{-1};
    std::thread thread_;
};

// Queries every second; hosts() is the current list.
class Browser {
public:
    Browser();
    ~Browser();
    std::vector<Host> hosts();

private:
    void Run();
    std::atomic<bool> running_{true};
    std::atomic<int> fd_{-1};
    std::mutex mutex_;
    std::vector<Host> hosts_;
    std::thread thread_;
};

}  // namespace Discovery

}  // namespace cartucho
