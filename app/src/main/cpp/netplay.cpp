#include "netplay.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace cartucho {

namespace {

constexpr uint32_t kVersion = 1;
constexpr int kCrcEvery = 120;              // frames between checksums
constexpr size_t kMaxMessage = 64u << 20;   // biggest state accepted

template <typename T>
void Put(std::vector<uint8_t>* out, T v) {
    const size_t at = out->size();
    out->resize(at + sizeof(T));
    std::memcpy(out->data() + at, &v, sizeof(T));
}

template <typename T>
bool Get(const std::vector<uint8_t>& in, size_t* at, T* v) {
    if (*at + sizeof(T) > in.size())
        return false;
    std::memcpy(v, in.data() + *at, sizeof(T));
    *at += sizeof(T);
    return true;
}

void NoDelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    timeval tv{0, 200 * 1000};  // reads wake up to check for Stop()
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

}  // namespace

Netplay::Netplay() = default;

Netplay::~Netplay() { Stop(); }

bool Netplay::Host(int port, const std::string& game) {
    Stop();
    host_ = true;
    game_ = game;
    stop_ = false;
    state_ = State::kWaiting;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        message_ = "Esperando o outro jogador…";
    }
    listen_fd_ = socket(AF_INET6, SOCK_STREAM, 0);
    bool v6 = listen_fd_ >= 0;
    if (!v6)
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        Fail(State::kFailed, "Não foi possível abrir a conexão");
        return false;
    }
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int rc;
    if (v6) {
        int zero = 0;  // IPv4 and IPv6 both
        setsockopt(listen_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(static_cast<uint16_t>(port));
        rc = bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(static_cast<uint16_t>(port));
        rc = bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }
    if (rc != 0 || listen(listen_fd_, 1) != 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        Fail(State::kFailed, "A porta " + std::to_string(port) + " está ocupada");
        return false;
    }
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                                              : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
    thread_ = std::thread(&Netplay::NetThread, this, std::string(), port_.load());
    return true;
}

bool Netplay::Join(const std::string& host, int port, const std::string& game) {
    Stop();
    host_ = false;
    game_ = game;
    stop_ = false;
    port_ = port;
    state_ = State::kConnecting;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        message_ = "Conectando…";
    }
    thread_ = std::thread(&Netplay::NetThread, this, host, port);
    return true;
}

void Netplay::Stop() {
    if (thread_.joinable()) {
        if (fd_ >= 0 && state_ == State::kPlaying)
            Send(kBye, nullptr, 0);
        stop_ = true;
        const int fd = fd_.load();
        if (fd >= 0)
            shutdown(fd, SHUT_RDWR);
        thread_.join();
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    const int fd = fd_.exchange(-1);
    if (fd >= 0)
        close(fd);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.clear();
    }
    state_ = State::kIdle;
    waiting_ = false;
    rtt_ms_ = -1;
    cur_ = 0;
    remote_upto_ = -1;
    rollback_from_ = 1 << 30;
    state_sent_ = false;
    remote_crcs_.clear();
    for (Slot& s : ring_)
        s.frame = -1;
}

std::string Netplay::message() {
    std::lock_guard<std::mutex> lock(message_mutex_);
    return message_;
}

void Netplay::Fail(State s, const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        message_ = why;
    }
    state_ = s;
    waiting_ = false;
}

// --- Network thread ------------------------------------------------------------

void Netplay::NetThread(std::string host, int port) {
    int fd = -1;
    if (host_) {
        // Wait for the guest.
        while (!stop_) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (poll(&p, 1, 200) > 0) {
                fd = accept(listen_fd_, nullptr, nullptr);
                if (fd >= 0)
                    break;
            }
        }
        if (fd < 0)
            return;
        // One guest per session.
        close(listen_fd_);
        listen_fd_ = -1;
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
            Fail(State::kFailed, "Endereço não encontrado: " + host);
            return;
        }
        for (addrinfo* a = res; a && fd < 0 && !stop_; a = a->ai_next) {
            int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0)
                continue;
            fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
            int rc = connect(s, a->ai_addr, a->ai_addrlen);
            if (rc != 0 && errno == EINPROGRESS) {
                pollfd p{s, POLLOUT, 0};
                for (int i = 0; i < 50 && !stop_; i++) {  // 10 s
                    rc = poll(&p, 1, 200);
                    if (rc != 0)
                        break;
                }
                int err = 0;
                socklen_t len = sizeof(err);
                rc = (rc > 0 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) ? 0 : -1;
            }
            if (rc == 0) {
                fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
                fd = s;
            } else {
                close(s);
            }
        }
        freeaddrinfo(res);
        if (fd < 0) {
            if (!stop_)
                Fail(State::kFailed, "Não foi possível conectar a " + host + ":" + std::to_string(port) +
                                         ". Confira o endereço e se o outro jogador criou a sala.");
            return;
        }
    }
    NoDelay(fd);
    fd_ = fd;
    state_ = State::kSyncing;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        message_ = "Sincronizando…";
    }
    if (!host_) {
        // Say which game we have; the host answers with its own and the state.
        std::vector<uint8_t> hello;
        Put<uint32_t>(&hello, kVersion);
        hello.insert(hello.end(), game_.begin(), game_.end());
        Send(kHello, hello.data(), hello.size());
    }
    ReadLoop(fd);
    if (!stop_ && (state_ == State::kPlaying || state_ == State::kSyncing))
        Fail(State::kClosed, "O outro jogador saiu da partida");
}

bool Netplay::ReadLoop(int fd) {
    auto read_all = [&](void* buf, size_t size) {
        uint8_t* p = static_cast<uint8_t*>(buf);
        size_t got = 0;
        while (got < size) {
            if (stop_)
                return false;
            const ssize_t n = recv(fd, p + got, size - got, 0);
            if (n > 0) {
                got += static_cast<size_t>(n);
            } else if (n == 0) {
                return false;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return false;
            }
        }
        return true;
    };
    while (!stop_) {
        uint32_t len = 0;
        if (!read_all(&len, sizeof(len)) || len == 0 || len > kMaxMessage)
            return false;
        Message m;
        if (!read_all(&m.type, 1))
            return false;
        m.data.resize(len - 1);
        if (len > 1 && !read_all(m.data.data(), len - 1))
            return false;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(std::move(m));
    }
    return true;
}

void Netplay::Send(uint8_t type, const void* data, size_t size) {
    const int fd = fd_.load();
    if (fd < 0)
        return;
    std::vector<uint8_t> buf;
    buf.reserve(5 + size);
    Put<uint32_t>(&buf, static_cast<uint32_t>(size + 1));
    buf.push_back(type);
    const uint8_t* d = static_cast<const uint8_t*>(data);
    if (size)
        buf.insert(buf.end(), d, d + size);
    std::lock_guard<std::mutex> lock(send_mutex_);
    size_t sent = 0;
    while (sent < buf.size()) {
        const ssize_t n = send(fd, buf.data() + sent, buf.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return;  // the reader notices the broken connection
        }
        sent += static_cast<size_t>(n);
    }
}

// --- Emulation thread -----------------------------------------------------------

void Netplay::Poll(NetMachine& m, double now) {
    std::deque<Message> got;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        got.swap(queue_);
    }
    for (Message& msg : got)
        Handle(m, msg, now);
    if (playing() && now - last_ping_ >= 1.0) {
        last_ping_ = now;
        Send(kPing, &now, sizeof(now));
    }
    last_now_ = now;
}

void Netplay::Handle(NetMachine& m, Message& msg, double now) {
    size_t at = 0;
    switch (msg.type) {
        case kHello: {
            uint32_t version = 0;
            Get(msg.data, &at, &version);
            const std::string game(msg.data.begin() + static_cast<long>(std::min(at, msg.data.size())), msg.data.end());
            if (version != kVersion) {
                Send(kBye, nullptr, 0);
                Fail(State::kFailed, "O outro aparelho tem outra versão do Cartucho. Atualizem os dois.");
                return;
            }
            if (game != game_) {
                Send(kBye, nullptr, 0);
                Fail(State::kFailed, "O outro jogador abriu outro jogo (ou outra versão dele).");
                return;
            }
            if (host_ && !state_sent_) {
                // The guest is ready to receive: our hello, then the state now.
                std::vector<uint8_t> hello;
                Put<uint32_t>(&hello, kVersion);
                hello.insert(hello.end(), game_.begin(), game_.end());
                Send(kHello, hello.data(), hello.size());
                for (Slot& s : ring_)
                    s.frame = -1;
                Slot& s0 = slot(0);
                s0.frame = 0;
                if (!m.Save(&s0.state)) {
                    Send(kBye, nullptr, 0);
                    Fail(State::kFailed, "Este jogo não permite jogar online (sem estados salvos).");
                    return;
                }
                std::vector<uint8_t> st;
                Put<int32_t>(&st, 0);
                st.insert(st.end(), s0.state.begin(), s0.state.end());
                Send(kState, st.data(), st.size());
                state_sent_ = true;
            }
            break;
        }
        case kState: {
            int32_t f = 0;
            Get(msg.data, &at, &f);
            std::vector<uint8_t> data(msg.data.begin() + static_cast<long>(at), msg.data.end());
            if (!host_ && state_ == State::kSyncing) {
                // The starting state.
                if (!m.Load(data)) {
                    Send(kBye, nullptr, 0);
                    Fail(State::kFailed, "Não foi possível carregar o jogo do outro jogador.");
                    return;
                }
                for (Slot& s : ring_)
                    s.frame = -1;
                Slot& s0 = slot(0);
                s0.frame = 0;
                s0.state = std::move(data);
                Send(kReady, nullptr, 0);
                StartPlaying();
            } else if (!host_ && playing() && f < cur_ && cur_ - f < kRing - 2) {
                // A correction after a mismatch: run again from there.
                Slot& s = slot(f);
                if (s.frame == f) {
                    s.state = std::move(data);
                    rollback_from_ = std::min(rollback_from_, static_cast<int>(f));
                    resyncs_++;
                }
            }
            break;
        }
        case kReady:
            if (host_ && state_ == State::kSyncing)
                StartPlaying();
            break;
        case kInput: {
            int32_t f = 0;
            NetInput in;
            int16_t advantage = 0;
            if (!Get(msg.data, &at, &f) || !Get(msg.data, &at, &in.buttons) || !Get(msg.data, &at, &in.ax) ||
                !Get(msg.data, &at, &in.ay))
                return;
            Get(msg.data, &at, &advantage);
            remote_advantage_ = advantage;
            if (f < cur_ - (kRing - 2))
                return;  // too old to matter
            Slot& s = slot(f);
            if (s.frame != f) {
                s.frame = f;
                s.have_local = false;
                s.have_crc = false;
                s.state.clear();
            }
            s.remote = in;
            s.have_remote = true;
            if (f > remote_upto_) {
                remote_upto_ = f;
                last_remote_ = in;
            }
            if (f < cur_ && s.used_remote != in)
                rollback_from_ = std::min(rollback_from_, static_cast<int>(f));
            break;
        }
        case kCrc: {
            int32_t f = 0;
            uint32_t crc = 0;
            if (!Get(msg.data, &at, &f) || !Get(msg.data, &at, &crc))
                return;
            remote_crcs_.emplace_back(f, crc);
            if (remote_crcs_.size() > 8)
                remote_crcs_.erase(remote_crcs_.begin());
            CompareCrc(f);
            break;
        }
        case kPing:
            Send(kPong, msg.data.data(), msg.data.size());
            break;
        case kPong: {
            double t = 0;
            if (Get(msg.data, &at, &t))
                rtt_ms_ = static_cast<int>((now - t) * 1000.0 + 0.5);
            break;
        }
        case kBye:
            Fail(State::kClosed, "O outro jogador saiu da partida");
            break;
        default:
            break;
    }
}

void Netplay::StartPlaying() {
    cur_ = 0;
    remote_upto_ = delay_ - 1;
    last_remote_ = NetInput{};
    rollback_from_ = 1 << 30;
    next_crc_ = kCrcEvery;
    remote_crcs_.clear();
    remote_advantage_ = 0;
    // The first frames, before anyone's input can arrive: no buttons.
    for (int f = 0; f < delay_; f++) {
        Slot& s = slot(f);
        if (s.frame != f) {
            s.frame = f;
            s.state.clear();
        }
        s.local = NetInput{};
        s.have_local = true;
        s.remote = NetInput{};
        s.have_remote = true;
        s.have_crc = false;
    }
    state_ = State::kPlaying;
    std::lock_guard<std::mutex> lock(message_mutex_);
    message_ = "Jogando online";
}

NetInput Netplay::LocalAt(int f) const {
    const Slot& s = slot(f);
    return (s.frame == f && s.have_local) ? s.local : NetInput{};
}

void Netplay::RunAt(NetMachine& m, int f, bool video, bool audio) {
    Slot& s = slot(f);
    if (s.frame != f) {
        s.frame = f;
        s.have_local = false;
        s.have_remote = false;
        s.have_crc = false;
    }
    m.Save(&s.state);
    // The other player's input, or the guess: their latest known.
    NetInput remote = s.have_remote ? s.remote : last_remote_;
    if (!s.have_remote && remote_upto_ < f) {
        const Slot& known = slot(remote_upto_);
        if (known.frame == remote_upto_ && known.have_remote)
            remote = known.remote;
    }
    s.used_remote = remote;
    s.have_crc = false;
    NetInput inputs[2];
    inputs[local_player()] = LocalAt(f);
    inputs[1 - local_player()] = remote;
    m.Run(inputs, video, audio);
}

void Netplay::Resimulate(NetMachine& m, int from) {
    Slot& s = slot(from);
    if (s.frame != from || s.state.empty() || !m.Load(s.state))
        return;
    for (int f = from; f < cur_; f++)
        RunAt(m, f, f + 1 == cur_, false);
    rollbacks_ += cur_ - from;
}

bool Netplay::Advance(NetMachine& m, const NetInput& local) {
    if (!playing())
        return false;
    Poll(m, last_now_);
    if (!playing())
        return false;
    if (rollback_from_ < cur_)
        Resimulate(m, rollback_from_);
    rollback_from_ = 1 << 30;
    CheckCrc();
    // Too far ahead of what we know of the other player: wait for them.
    if (cur_ - remote_upto_ > kMaxRollback) {
        waiting_ = true;
        return false;
    }
    waiting_ = false;
    // Keep both devices at the same pace: the one further ahead (counting
    // the network delay the same for both) gives up a frame now and then.
    const int advantage = cur_ - (remote_upto_ - delay_);
    if (advantage - remote_advantage_ >= 2 && ++skip_counter_ % 3 == 0)
        return false;
    // Our input for a few frames ahead goes out now.
    const int target = cur_ + delay_;
    Slot& t = slot(target);
    if (t.frame != target) {
        t.frame = target;
        t.have_remote = false;
        t.have_crc = false;
        t.state.clear();
        t.have_local = false;
    }
    if (!t.have_local) {
        t.local = local;
        t.have_local = true;
        std::vector<uint8_t> msg;
        Put<int32_t>(&msg, target);
        Put<uint32_t>(&msg, local.buttons);
        Put<int16_t>(&msg, local.ax);
        Put<int16_t>(&msg, local.ay);
        Put<int16_t>(&msg, static_cast<int16_t>(std::max(-100, std::min(100, advantage))));
        Send(kInput, msg.data(), msg.size());
    }
    RunAt(m, cur_, true, true);
    cur_++;
    return true;
}

void Netplay::CheckCrc() {
    // A frame's starting state is final once the other player's inputs for
    // every frame before it are known (and run).
    while (next_crc_ < cur_ && remote_upto_ >= next_crc_ - 1) {
        const int f = next_crc_;
        next_crc_ += kCrcEvery;
        Slot& s = slot(f);
        if (s.frame != f || s.state.empty())
            continue;
        s.crc = Crc32(s.state.data(), s.state.size());
        s.have_crc = true;
        std::vector<uint8_t> msg;
        Put<int32_t>(&msg, f);
        Put<uint32_t>(&msg, s.crc);
        Send(kCrc, msg.data(), msg.size());
        CompareCrc(f);
    }
}

void Netplay::CompareCrc(int f) {
    const Slot& s = slot(f);
    if (s.frame != f || !s.have_crc)
        return;
    for (const auto& rc : remote_crcs_) {
        if (rc.first != f)
            continue;
        if (rc.second != s.crc && host_ && last_now_ - last_resync_ > 5.0 && !s.state.empty()) {
            // The guest went its own way: send it our state of that frame.
            last_resync_ = last_now_;
            resyncs_++;
            std::vector<uint8_t> st;
            Put<int32_t>(&st, f);
            st.insert(st.end(), s.state.begin(), s.state.end());
            Send(kState, st.data(), st.size());
        }
    }
}

uint32_t Netplay::Crc32(const uint8_t* data, size_t size) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace cartucho
