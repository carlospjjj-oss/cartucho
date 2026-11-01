#include "cheats.h"

#include <json.hpp>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "http.h"
#include "library.h"
#include "paths.h"

namespace cartucho {

std::string CheatFile(const std::string& system_key, const std::string& save_name) {
    return JoinPath(JoinPath(JoinPath(Paths::Get().data, "cheats"), system_key), save_name + ".json");
}

std::vector<Cheat> LoadCheatList(const std::string& file) {
    std::vector<Cheat> out;
    std::string text;
    if (!ReadText(file, &text)) return out;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& o : j) {
        if (!o.is_object()) continue;
        Cheat c;
        c.name = o.value("name", "");
        c.code = o.value("code", "");
        c.enabled = o.value("enabled", true);
        if (!c.code.empty()) out.push_back(c);
    }
    return out;
}

void SaveCheatList(const std::string& file, const std::vector<Cheat>& list) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& c : list) j.push_back({{"name", c.name}, {"code", c.code}, {"enabled", c.enabled}});
    MakeDirs(ParentDir(file));
    WriteText(file, j.dump());
}

namespace CheatDb {

namespace {

std::vector<std::string> Folders(Sys sys) {
    switch (sys) {
        case Sys::GB: return {"Nintendo - Game Boy Color", "Nintendo - Game Boy"};
        case Sys::GBA: return {"Nintendo - Game Boy Advance"};
        case Sys::NDS: return {"Nintendo - Nintendo DS"};
        case Sys::NES: return {"Nintendo - Nintendo Entertainment System"};
        case Sys::SNES: return {"Nintendo - Super Nintendo Entertainment System"};
        case Sys::MD: return {"Sega - Mega Drive - Genesis"};
        case Sys::SMS: return {"Sega - Master System - Mark III"};
        case Sys::GG: return {"Sega - Game Gear"};
        case Sys::PS1: return {"Sony - PlayStation"};
        default: return {};
    }
}

std::vector<std::string> Names(const std::string& folder) {
    std::vector<std::string> out;
    std::string text;
    if (!ReadText(JoinPath(JoinPath(Paths::Get().assets, "cheats"), folder + ".txt"), &text)) return out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

std::string Base(const std::string& name) {
    std::string b = name.substr(0, name.find(" ("));
    while (!b.empty() && b.back() == ' ') b.pop_back();
    return b;
}

std::string Unescape(std::string s) {
    const std::pair<const char*, const char*> reps[] = {{"&amp;", "&"}, {"&quot;", "\""}, {"&lt;", "<"},
                                                        {"&gt;", ">"}, {"&#39;", "'"}, {"\\\"", "\""}};
    for (const auto& [a, b] : reps) {
        size_t p = 0;
        while ((p = s.find(a, p)) != std::string::npos) {
            s.replace(p, std::strlen(a), b);
            p += std::strlen(b);
        }
    }
    return s;
}

std::string Lowered(const std::string& s) { return Lower(s); }

}  // namespace

std::string Source::Label() const {
    std::vector<std::string> device;
    const std::regex tag("\\(([^)]*)\\)");
    for (auto it = std::sregex_iterator(name.begin(), name.end(), tag); it != std::sregex_iterator(); ++it) {
        const std::string t = (*it)[1];
        const std::string l = Lowered(t);
        for (const char* k : {"gameshark", "game genie", "action replay", "code breaker", "codebreaker", "xploder", "pro action",
                              "cheat", "diff", "gs", "ar"})
            if (l.find(k) != std::string::npos) {
                device.push_back(t);
                break;
            }
    }
    std::string out;
    for (const auto& d : device) out += (out.empty() ? "" : " · ") + d;
    if (out.empty()) out = "Padrão";
    const bool color = folder.size() >= 5 && folder.compare(folder.size() - 5, 5, "Color") == 0;
    if (!color) out += "  (" + folder.substr(folder.find(" - ") + 3) + ")";
    return out;
}

bool Supported(Sys sys) { return !Folders(sys).empty(); }

std::vector<Source> Find(Sys sys, const std::string& file_name) {
    std::vector<Source> out;
    for (const auto& folder : Folders(sys)) {
        const auto all = Names(folder);
        if (all.empty()) continue;
        std::string best = BestMatch(file_name, all);
        if (best.empty()) {
            // Matching on the title alone.
            const auto want = Tokens(file_name);
            if (want.empty()) continue;
            const std::set<std::string> wset(want.begin(), want.end());
            size_t best_score = SIZE_MAX;
            for (const auto& n : all) {
                const auto t = Tokens(n);
                const std::set<std::string> tset(t.begin(), t.end());
                if (!std::includes(tset.begin(), tset.end(), wset.begin(), wset.end())) continue;
                const size_t score = t.size() * 100 + n.size();
                if (score < best_score) {
                    best_score = score;
                    best = n;
                }
            }
        }
        if (best.empty()) continue;
        const std::string title = Base(best);
        out.push_back({folder, best});
        for (const auto& n : all)
            if (n != best && Base(n) == title) out.push_back({folder, n});
    }
    return out;
}

std::vector<Entry> Parse(const std::string& text) {
    std::map<int, std::string> desc, code;
    const std::regex re("^\\s*cheat(\\d+)_(desc|code)\\s*=\\s*\"(.*)\"\\s*$");
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (!std::regex_match(line, m, re)) continue;
        const int i = std::atoi(m[1].str().c_str());
        if (m[2] == "desc") desc[i] = Unescape(m[3]);
        else code[i] = m[3];
    }
    std::vector<Entry> out;
    for (const auto& [i, c0] : code) {
        std::string c = c0;
        while (!c.empty() && c.back() == ' ') c.pop_back();
        if (c.empty()) continue;
        std::string joined;
        std::stringstream parts(c);
        std::string p;
        while (std::getline(parts, p, '+')) {
            while (!p.empty() && p.front() == ' ') p.erase(0, 1);
            while (!p.empty() && p.back() == ' ') p.pop_back();
            if (!p.empty()) joined += (joined.empty() ? "" : "\n") + p;
        }
        std::string name = desc.count(i) ? desc[i] : "";
        if (name.find_first_not_of(' ') == std::string::npos) name = "Trapaça " + std::to_string(i + 1);
        out.push_back({name, joined});
    }
    return out;
}

bool Load(const Source& s, std::vector<Entry>* out, std::string* error) {
    std::string safe = s.folder + "__" + s.name;
    for (char& c : safe)
        if (std::strchr("\\/:*?\"<>|", c)) c = '_';
    const std::string file = JoinPath(JoinPath(Paths::Get().data, "cht"), safe + ".cht");
    std::string text;
    if (FileSize(file) == 0 || !ReadText(file, &text)) {
        auto enc = [](const std::string& x) { return UrlEncode(x); };
        const std::string url = "https://raw.githubusercontent.com/libretro/libretro-database/master/cht/" + enc(s.folder) +
                                "/" + enc(s.name) + ".cht";
        const HttpResult r = HttpGet(url, 20000, 8u * 1024 * 1024);
        if (!r.ok()) {
            *error = r.status == 0 ? "Sem conexão com a internet." : "HTTP " + std::to_string(r.status);
            return false;
        }
        text = r.text();
        MakeDirs(ParentDir(file));
        WriteText(file, text);
    }
    *out = Parse(text);
    if (out->empty()) *error = "A lista veio vazia.";
    return !out->empty();
}

}  // namespace CheatDb

}  // namespace cartucho
