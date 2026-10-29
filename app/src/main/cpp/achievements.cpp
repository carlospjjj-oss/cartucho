#include "achievements.h"

#include <cstdio>

#include "chd_reader.h"
#include "platform.h"

extern "C" {
#include "rc_api_request.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_libretro.h"
}

namespace cartucho {

namespace {

// rc_libretro asks for the core's memory without user data; there is one game
// (and so one Achievements) per process.
Achievements* g_current = nullptr;
CoreHost* g_host = nullptr;

void GetCoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info) {
    size_t size = 0;
    info->data = g_host ? g_host->MemoryData(id, &size) : nullptr;
    info->size = info->data ? size : 0;
}

void Log(const char* message, const rc_client_t*) {
    PlatformLog(1, (std::string("RetroAchievements: ") + message).c_str());
}

Achievements* Self(rc_client_t* client) {
    return static_cast<Achievements*>(rc_client_get_userdata(client));
}

}  // namespace

Achievements::Achievements(CoreHost* host) : host_(host) {
    g_current = this;
    g_host = host;
    client_ = rc_client_create(&Achievements::ReadMemory, &Achievements::ServerCall);
    rc_client_set_userdata(client_, this);
    rc_client_enable_logging(client_, RC_CLIENT_LOG_LEVEL_WARN, Log);
    // Softcore: saving states, rewinding and cheats stay available. (Hardcore
    // unlocks are only accepted from approved emulators anyway.)
    rc_client_set_hardcore_enabled(client_, 0);
    // Disc games also as .chd.
    rc_hash_callbacks_t hash_callbacks{};
    GetDiscReader(&hash_callbacks.cdreader);
    rc_client_set_hash_callbacks(client_, &hash_callbacks);
    rc_client_set_event_handler(client_, [](const rc_client_event_t* event, rc_client_t* client) {
        Achievements* self = Self(client);
        CoreHost* host = self->host_;
        char text[256];
        switch (event->type) {
            case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                std::snprintf(text, sizeof(text), "🏆 Conquista: %s (+%u)", event->achievement->title,
                              event->achievement->points);
                host->PostMessage(text);
                self->OnGameLoaded();  // refresh the list
                break;
            case RC_CLIENT_EVENT_GAME_COMPLETED:
                host->PostMessage("🏆 Jogo completo: todas as conquistas!");
                break;
            case RC_CLIENT_EVENT_DISCONNECTED:
                host->PostMessage("Conquistas: sem internet; a conquista será enviada depois");
                break;
            case RC_CLIENT_EVENT_RECONNECTED:
                host->PostMessage("Conquistas: conquistas pendentes enviadas");
                break;
            case RC_CLIENT_EVENT_SERVER_ERROR:
                if (event->server_error && event->server_error->error_message)
                    Log(event->server_error->error_message, client);
                break;
            default:
                break;
        }
    });
}

Achievements::~Achievements() {
    if (client_)
        rc_client_destroy(client_);
    if (regions_) {
        auto* r = static_cast<rc_libretro_memory_regions_t*>(regions_);
        rc_libretro_memory_destroy(r);
        delete r;
    }
    if (g_current == this) {
        g_current = nullptr;
        g_host = nullptr;
    }
}

uint32_t Achievements::ReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
    Achievements* self = Self(client);
    if (!self || !self->regions_)
        return 0;
    return rc_libretro_memory_read(static_cast<rc_libretro_memory_regions_t*>(self->regions_), address, buffer,
                                   num_bytes);
}

void Achievements::ServerCall(const rc_api_request_t* request,
                              void (*callback)(const rc_api_server_response_t*, void*), void* callback_data,
                              rc_client_t* client) {
    Achievements* self = Self(client);
    std::lock_guard<std::mutex> lock(self->mutex_);
    Request r;
    r.id = self->next_id_++;
    r.url = request->url ? request->url : "";
    r.post_data = request->post_data ? request->post_data : "";
    r.content_type = request->content_type ? request->content_type : "";
    self->requests_.push_back(r);
    self->waiting_.push_back(Pending{r.id, callback, callback_data});
}

bool Achievements::TakeRequest(Request* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (requests_.empty())
        return false;
    *out = std::move(requests_.front());
    requests_.pop_front();
    return true;
}

void Achievements::Respond(int id, int status, std::string body) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
        if (it->id == id) {
            answers_.push_back(Answer{*it, status, std::move(body)});
            waiting_.erase(it);
            return;
        }
    }
}

void Achievements::ProcessAnswers() {
    for (;;) {
        Answer a;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (answers_.empty())
                return;
            a = std::move(answers_.front());
            answers_.pop_front();
        }
        // Outside the lock: the callback may send the next request.
        rc_api_server_response_t response{};
        response.body = a.body.c_str();
        response.body_length = a.body.size();
        response.http_status_code = a.status;
        a.pending.callback(&response, a.pending.callback_data);
    }
}

void Achievements::Start(const std::string& user, const std::string& token, int console) {
    console_ = console;
    // Memory first: loading the set checks every address it uses.
    auto* regions = new rc_libretro_memory_regions_t();
    if (rc_libretro_memory_init(regions, host_->memory_map(), GetCoreMemoryInfo, static_cast<uint32_t>(console)))
        regions_ = regions;
    else
        delete regions;
    rc_client_begin_login_with_token(client_, user.c_str(), token.c_str(),
        [](int result, const char* error, rc_client_t* client, void*) {
            Achievements* self = Self(client);
            if (result != RC_OK) {
                self->host_->PostMessage(std::string("Conquistas: não foi possível entrar (") +
                                         (error ? error : "erro") + "). Entre de novo nas Configurações.");
                return;
            }
            const auto& data = self->host_->rom_data();
            const std::string& path = self->host_->rom_path();
            rc_client_begin_identify_and_load_game(client, static_cast<uint32_t>(self->console_), path.c_str(),
                data.empty() ? nullptr : data.data(), data.size(),
                [](int result, const char* error, rc_client_t* client, void*) {
                    Achievements* self = Self(client);
                    if (result == RC_NO_GAME_LOADED) {
                        self->host_->PostMessage("Conquistas: este jogo não tem conquistas (ou esta versão não é reconhecida)");
                        return;
                    }
                    if (result != RC_OK) {
                        self->host_->PostMessage(std::string("Conquistas: ") + (error ? error : "erro ao carregar"));
                        return;
                    }
                    if (!self->regions_) {
                        self->host_->PostMessage("Conquistas: este emulador não mostra a memória do jogo");
                        return;
                    }
                    self->loaded_ = true;
                    self->OnGameLoaded();
                    rc_client_user_game_summary_t summary{};
                    rc_client_get_user_game_summary(client, &summary);
                    char text[160];
                    std::snprintf(text, sizeof(text), "🏆 RetroAchievements: %u de %u conquistas",
                                  summary.num_unlocked_achievements, summary.num_promoted_achievements);
                    self->host_->PostMessage(text);
                },
                nullptr);
        },
        nullptr);
}

void Achievements::OnGameLoaded() {
    std::vector<std::string> lines;
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        client_, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (list) {
        for (uint32_t b = 0; b < list->num_buckets; b++) {
            const auto& bucket = list->buckets[b];
            for (uint32_t i = 0; i < bucket.num_achievements; i++) {
                const rc_client_achievement_t* a = bucket.achievements[i];
                std::string line = a->unlocked ? "1|" : "0|";
                line += a->title ? a->title : "";
                line += "|";
                line += a->description ? a->description : "";
                line += "|" + std::to_string(a->points);
                if (!a->unlocked && a->measured_progress[0])
                    line += std::string("|") + a->measured_progress;
                lines.push_back(std::move(line));
            }
        }
        rc_client_destroy_achievement_list(list);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    list_ = std::move(lines);
}

std::vector<std::string> Achievements::List() {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_;
}

void Achievements::DoFrame() {
    ProcessAnswers();
    rc_client_do_frame(client_);
}

void Achievements::Idle() {
    ProcessAnswers();
    rc_client_idle(client_);
}

void Achievements::Reset() {
    if (loaded_)
        rc_client_reset(client_);
}

}  // namespace cartucho
