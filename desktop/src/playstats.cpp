#include "playstats.h"

#include <json.hpp>

#include <cstdio>

#include "paths.h"

namespace cartucho {
namespace PlayStats {

namespace {
std::string Dir() {
    const std::string d = JoinPath(Paths::Get().data, "play");
    MakeDirs(d);
    return d;
}
nlohmann::json Read(const std::string& p) {
    std::string t;
    if (!ReadText(JoinPath(Dir(), Id(p) + ".json"), &t))
        return nlohmann::json::object();
    auto j = nlohmann::json::parse(t, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}
}  // namespace

std::string Id(const std::string& game_path) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : game_path) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

int64_t PlayTimeMs(const std::string& p) { return Read(p).value("ms", int64_t{0}); }
int64_t LastPlayed(const std::string& p) { return Read(p).value("last", int64_t{0}); }

void AddPlayTime(const std::string& p, int64_t ms) {
    auto j = Read(p);
    j["ms"] = j.value("ms", int64_t{0}) + (ms > 0 ? ms : 0);
    j["last"] = NowMs();
    WriteText(JoinPath(Dir(), Id(p) + ".json"), j.dump());
}

std::string ThumbPath(const std::string& p) { return JoinPath(Dir(), Id(p) + ".png"); }

std::string ResumeState(const std::string& state_dir, const std::string& save_name) {
    return JoinPath(state_dir, save_name + ".stateAuto");
}

bool CanResume(const std::string& key) { return key != "N3DS"; }

std::string FormatTime(int64_t ms) {
    const int64_t m = ms / 60000;
    if (m < 1) return "menos de 1 min";
    if (m < 60) return std::to_string(m) + " min";
    if (m % 60 == 0) return std::to_string(m / 60) + " h";
    return std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min";
}

std::string FormatAgo(int64_t when) {
    if (when <= 0) return "";
    const int64_t min = (NowMs() - when) / 60000;
    if (min < 5) return "agora há pouco";
    if (min < 60) return "há " + std::to_string(min) + " min";
    if (min < 24 * 60) return "há " + std::to_string(min / 60) + " h";
    const int64_t days = min / (24 * 60);
    if (days == 1) return "ontem";
    if (days < 30) return "há " + std::to_string(days) + " dias";
    if (days < 365) return "há " + std::to_string(days / 30) + (days / 30 == 1 ? " mês" : " meses");
    return "há mais de um ano";
}

}  // namespace PlayStats
}  // namespace cartucho
