#include "discovery.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "http.h"
#include "paths.h"
#include "socket_compat.h"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef CARTUCHO_VERSION
#define CARTUCHO_VERSION "dev"
#endif

namespace cartucho {

namespace Discovery {

namespace {

constexpr int kWireCable = 2, kWireRfu = 4, kWireGb = 7;

std::string Ip(uint32_t be) {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a{};
    a.s_addr = be;
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

struct Iface {
    std::string name;
    uint32_t addr = 0;       // network order
    uint32_t broadcast = 0;  // network order, 0 if none
};

std::vector<Iface> Interfaces() {
    std::vector<Iface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, list, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, list, &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::string name;
        if (a->FriendlyName) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
            name.resize(static_cast<size_t>(n > 0 ? n : 1));
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name.data(), n, nullptr, nullptr);
            name.resize(std::strlen(name.c_str()));
        }
        if (a->Description) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, nullptr, 0, nullptr, nullptr);
            std::string d(static_cast<size_t>(n > 0 ? n : 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, d.data(), n, nullptr, nullptr);
            name += " " + std::string(d.c_str());
        }
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            Iface f;
            f.name = name;
            f.addr = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            const unsigned prefix = u->OnLinkPrefixLength;
            const uint32_t mask = prefix == 0 ? 0 : prefix >= 32 ? 0xFFFFFFFFu : htonl(0xFFFFFFFFu << (32 - prefix));
            if (prefix > 0 && prefix < 31) f.broadcast = f.addr | ~mask;
            out.push_back(f);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (auto* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if ((a->ifa_flags & IFF_UP) == 0 || (a->ifa_flags & IFF_LOOPBACK)) continue;
        Iface f;
        f.name = a->ifa_name ? a->ifa_name : "";
        f.addr = reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr;
        if ((a->ifa_flags & IFF_BROADCAST) && a->ifa_broadaddr)
            f.broadcast = reinterpret_cast<sockaddr_in*>(a->ifa_broadaddr)->sin_addr.s_addr;
        out.push_back(f);
    }
    freeifaddrs(list);
#endif
    return out;
}

bool IsVpn(const Iface& f) {
    const uint32_t h = ntohl(f.addr);
    const bool tailscale = (h >> 24) == 100 && ((h >> 16) & 0xC0) == 64;
    const std::string n = Lower(f.name);
    return tailscale || n.rfind("tun", 0) == 0 || n.rfind("zt", 0) == 0 || n.find("tailscale") != std::string::npos ||
           n.find("zerotier") != std::string::npos || n.find("radmin") != std::string::npos ||
           n.find("hamachi") != std::string::npos;
}

nlohmann::json Parse(const char* data, int len) {
    if (len <= 0 || len > 1024) return nullptr;
    auto j = nlohmann::json::parse(std::string(data, static_cast<size_t>(len)), nullptr, false);
    if (!j.is_object() || j.value("v", 0) != 1) return nullptr;
    return j;
}

std::string HostName() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return std::string(buf).substr(0, 64);
#ifdef _WIN32
    return "PC Windows";
#else
    return "PC";
#endif
}

int RecvFrom(int fd, char* buf, int size, sockaddr_in* from) {
#ifdef _WIN32
    int len = sizeof(*from);
    return recvfrom(static_cast<SOCKET>(fd), buf, size, 0, reinterpret_cast<sockaddr*>(from), &len);
#else
    socklen_t len = sizeof(*from);
    return static_cast<int>(recvfrom(fd, buf, static_cast<size_t>(size), 0, reinterpret_cast<sockaddr*>(from), &len));
#endif
}

void SendTo(int fd, const std::string& data, const sockaddr_in& to) {
#ifdef _WIN32
    sendto(static_cast<SOCKET>(fd), data.data(), static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&to),
           sizeof(to));
#else
    sendto(fd, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#endif
}

std::string Hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x", v);
    return buf;
}

}  // namespace

int WireMode(int mode) { return mode == 1 ? kWireRfu : mode == 2 ? kWireGb : kWireCable; }
int ModeFromWire(int wire) { return wire == kWireRfu ? 1 : wire == kWireGb ? 2 : wire == kWireCable ? 0 : -1; }

std::string ModeName(int mode) {
    switch (mode) {
        case kModeNetplay: return "Jogo online";
        case 1: return "Adaptador sem fio";
        case 2: return "Cabo de Game Boy";
        default: return "Cabo de GBA";
    }
}

std::vector<std::string> LocalAddresses() {
    std::vector<std::string> out;
    for (const auto& f : Interfaces()) out.push_back(Ip(f.addr));
    return out;
}

std::vector<std::string> VpnAddresses() {
    std::vector<std::string> out;
    for (const auto& f : Interfaces())
        if (IsVpn(f)) out.push_back(Ip(f.addr));
    return out;
}

std::string PublicAddress() {
    const HttpResult r = HttpGet("https://api.ipify.org", 6000, 64);
    if (!r.ok()) return "";
    std::string t = r.text();
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    for (char c : t)
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) return "";
    return t;
}

std::string Resolve(const std::string& host) {
    SockInit();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return "";
    const std::string ip = Ip(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return ip;
}

uint32_t JavaHash(const std::string& s) {
    // UTF-8 to UTF-16 code units, then h = 31*h + c.
    uint32_t h = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 31; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 15; n = 3; }
        else { cp = c & 7; n = 4; }
        for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 63);
        i += static_cast<size_t>(n);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            h = 31 * h + (0xD800 + (cp >> 10));
            h = 31 * h + (0xDC00 + (cp & 0x3FF));
        } else {
            h = 31 * h + cp;
        }
    }
    return h;
}

// --- Advertiser -----------------------------------------------------------------

Advertiser::Advertiser(const Announcement& a) : ann_(a) { thread_ = std::thread(&Advertiser::Run, this); }

Advertiser::~Advertiser() {
    running_ = false;
    const int fd = fd_.exchange(-1);
    if (fd >= 0) SockClose(fd);
    if (thread_.joinable()) thread_.join();
}

void Advertiser::Run() {
    const int fd = SockOpen(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    fd_ = fd;
    SockOpt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
    SockRecvTimeout(fd, 400);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        // Port taken (another app advertising): joining by address still works.
        if (fd_.exchange(-1) >= 0) SockClose(fd);
        return;
    }
    nlohmann::json j = {{"vbam", "link-host"}, {"v", 1}, {"port", ann_.port}, {"mode", WireMode(ann_.mode)},
                        {"players", ann_.players}};
    if (ann_.mode == kModeNetplay) j["cartucho_net"] = 1;
    j["game"] = ann_.game.substr(0, 64);
    j["crc"] = Hex(ann_.crc);
    j["name"] = HostName();
    j["ver"] = std::string("Cartucho PC ") + CARTUCHO_VERSION;
    const std::string reply = j.dump();
    char buf[1500];
    while (running_) {
        sockaddr_in from{};
        const int n = RecvFrom(fd, buf, sizeof(buf), &from);
        if (n <= 0) continue;
        const auto q = Parse(buf, n);
        if (q.is_null() || q.value("vbam", "") != "link-query") continue;
        SendTo(fd, reply, from);
    }
}

// --- Browser --------------------------------------------------------------------

Browser::Browser() { thread_ = std::thread(&Browser::Run, this); }

Browser::~Browser() {
    running_ = false;
    const int fd = fd_.exchange(-1);
    if (fd >= 0) SockClose(fd);
    if (thread_.joinable()) thread_.join();
}

std::vector<Host> Browser::hosts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return hosts_;
}

void Browser::Run() {
    const int fd = SockOpen(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    fd_ = fd;
    SockOpt(fd, SOL_SOCKET, SO_BROADCAST, 1);
    SockRecvTimeout(fd, 250);
    const std::string query = nlohmann::json({{"vbam", "link-query"}, {"v", 1}}).dump();
    char buf[1500];
    int64_t last_query = 0;
    while (running_) {
        const int64_t now = NowMs();
        if (now - last_query >= 1000) {
            last_query = now;
            std::vector<uint32_t> targets = {htonl(INADDR_BROADCAST)};
            for (const auto& f : Interfaces())
                if (f.broadcast && std::find(targets.begin(), targets.end(), f.broadcast) == targets.end())
                    targets.push_back(f.broadcast);
            for (uint32_t t : targets) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(kPort);
                to.sin_addr.s_addr = t;
                SendTo(fd, query, to);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            hosts_.erase(std::remove_if(hosts_.begin(), hosts_.end(), [&](const Host& h) { return now - h.seen_ms > 3500; }),
                         hosts_.end());
        }
        sockaddr_in from{};
        const int n = RecvFrom(fd, buf, sizeof(buf), &from);
        if (n <= 0) continue;
        const auto j = Parse(buf, n);
        if (j.is_null() || j.value("vbam", "") != "link-host") continue;
        const int port = j.value("port", 0);
        if (port < 1 || port > 65535) continue;
        Host h;
        h.address = Ip(from.sin_addr.s_addr);
        h.port = port;
        h.mode = j.value("cartucho_net", 0) == 1 ? kModeNetplay : ModeFromWire(j.value("mode", 0));
        h.players = j.value("players", 2);
        h.game = j.value("game", "");
        h.crc = static_cast<uint32_t>(std::strtoul(j.value("crc", "0").c_str(), nullptr, 16));
        h.name = j.value("name", "");
        h.version = j.value("ver", "");
        h.seen_ms = NowMs();
        std::lock_guard<std::mutex> lock(mutex_);
        bool found = false;
        for (auto& o : hosts_)
            if (o.address == h.address && o.port == h.port) {
                o = h;
                found = true;
            }
        if (!found) hosts_.push_back(h);
    }
}

}  // namespace Discovery

}  // namespace cartucho
