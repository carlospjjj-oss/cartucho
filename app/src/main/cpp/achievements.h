// Cartucho - RetroAchievements (retroachievements.org) through rcheevos.
//
// rc_client does the work: identifying the game by hash, downloading its
// achievements, watching the emulated memory every frame and reporting
// unlocks. It talks to the server through callbacks; this class turns them
// into a queue the app drains over HTTP (Android has no HTTP in the NDK), and
// queues the answers back to be processed on the emulation thread, where every
// other rc_client call happens.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core_host.h"

struct rc_client_t;
struct rc_api_request_t;
struct rc_api_server_response_t;

namespace cartucho {

class Achievements {
public:
    struct Request {
        int id = 0;
        std::string url;
        std::string post_data;
        std::string content_type;
    };

    explicit Achievements(CoreHost* host);
    ~Achievements();
    Achievements(const Achievements&) = delete;
    Achievements& operator=(const Achievements&) = delete;

    // Emulation thread. Logs in with the saved token, then identifies and
    // loads the game. `console` is an RC_CONSOLE_* id.
    void Start(const std::string& user, const std::string& token, int console);

    // Emulation thread: after each real emulated frame / when not running.
    void DoFrame();
    void Idle();
    // Emulation thread: a state was loaded (or the game reset).
    void Reset();

    // Any thread: the app takes the next HTTP request and gives its answer.
    bool TakeRequest(Request* out);
    void Respond(int id, int status, std::string body);

    // Any thread: the game's achievements as lines for the app's list
    // ("1|Title|Description|points" unlocked, "0|..." locked); empty if none.
    std::vector<std::string> List();
    bool active() const { return loaded_; }

private:
    struct Pending {
        int id;
        void (*callback)(const rc_api_server_response_t*, void*);
        void* callback_data;
    };
    struct Answer {
        Pending pending;
        int status;
        std::string body;
    };

    static uint32_t ReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client);
    static void ServerCall(const ::rc_api_request_t* request,
                           void (*callback)(const rc_api_server_response_t*, void*),
                           void* callback_data, rc_client_t* client);
    void ProcessAnswers();
    void OnGameLoaded();

    CoreHost* host_;
    rc_client_t* client_ = nullptr;
    int console_ = 0;
    bool loaded_ = false;
    void* regions_ = nullptr;  // rc_libretro_memory_regions_t

    std::mutex mutex_;  // guards the queues and the list snapshot
    int next_id_ = 1;
    std::deque<Request> requests_;
    std::vector<Pending> waiting_;
    std::deque<Answer> answers_;
    std::vector<std::string> list_;
};

}  // namespace cartucho
