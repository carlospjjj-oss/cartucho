#include "ra.h"

#include <json.hpp>

#include <chrono>

#include "achievements.h"
#include "http.h"
#include "paths.h"
#include "settings.h"

#ifndef CARTUCHO_VERSION
#define CARTUCHO_VERSION "dev"
#endif

namespace cartucho {

namespace RA {

namespace {

const char* kServer = "https://retroachievements.org/dorequest.php";

std::string UserAgent() {
#ifdef _WIN32
    return std::string("Cartucho/") + CARTUCHO_VERSION + " (Windows) rcheevos";
#else
    return std::string("Cartucho/") + CARTUCHO_VERSION + " (Linux) rcheevos";
#endif
}

HttpResult Post(const std::string& url, const std::string& body, const std::string& content_type) {
    std::vector<std::string> headers = {"User-Agent: " + UserAgent()};
    if (body.empty()) return HttpRequest("GET", url, "", headers, 30000);
    headers.push_back("Content-Type: " + (content_type.empty() ? std::string("application/x-www-form-urlencoded") : content_type));
    return HttpRequest("POST", url, body, headers, 30000);
}

}  // namespace

std::string User() { return Settings::Get().Str("ra_user", ""); }
bool SignedIn() { return !User().empty() && !Settings::Get().Str("ra_token", "").empty(); }
bool Enabled() { return SignedIn() && Settings::Get().Bool("ra_enabled", true); }
int Score() { return Settings::Get().Int("ra_score", 0); }

std::string SignIn(const std::string& user, const std::string& password) {
    const HttpResult r = Post(kServer, "r=login2&u=" + UrlEncode(user) + "&p=" + UrlEncode(password), "");
    if (r.status == 0) return "Sem conexão com o RetroAchievements.";
    const auto j = nlohmann::json::parse(r.text(), nullptr, false);
    if (!j.is_object()) return "Resposta inesperada do servidor (HTTP " + std::to_string(r.status) + ").";
    if (!j.value("Success", false)) {
        const std::string e = j.value("Error", "");
        return e.empty() ? "Usuário ou senha incorretos." : e;
    }
    Settings::Get().Set("ra_user", j.value("User", user));
    Settings::Get().Set("ra_token", j.value("Token", ""));
    Settings::Get().Set("ra_score", j.value("SoftcoreScore", j.value("Score", 0)));
    return "";
}

void SignOut() {
    Settings::Get().Set("ra_user", nullptr);
    Settings::Get().Set("ra_token", nullptr);
    Settings::Get().Set("ra_score", nullptr);
}

int ConsoleId(Sys sys, const std::string& file_name) {
    const std::string f = Lower(file_name);
    switch (sys) {
        case Sys::GB: return (f.size() > 4 && (f.substr(f.size() - 4) == ".gbc" || f.substr(f.size() - 4) == ".cgb")) ? 6 : 4;
        case Sys::GBA: return 5;
        case Sys::NDS: return 18;
        case Sys::NES: return 7;
        case Sys::SNES: return 3;
        case Sys::MD: return 1;
        case Sys::SMS: return 11;
        case Sys::GG: return 15;
        case Sys::PS1: return 12;
        default: return 0;
    }
}

Pump::Pump(std::shared_ptr<Achievements> ra) : ra_(std::move(ra)) { thread_ = std::thread(&Pump::Run, this); }

Pump::~Pump() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Pump::Run() {
    while (running_) {
        Achievements::Request req;
        if (!ra_->TakeRequest(&req)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const HttpResult r = Post(req.url, req.post_data, req.content_type);
        // No connection: rcheevos tries again later.
        ra_->Respond(req.id, r.status == 0 ? -2 : r.status, r.text());
    }
}

}  // namespace RA

}  // namespace cartucho
