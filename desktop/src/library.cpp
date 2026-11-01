#include "library.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <tuple>
#include <set>
#include <sstream>

#include "http.h"
#include "paths.h"
#include "playstats.h"
#include "settings.h"

namespace cartucho {

// --- Names ------------------------------------------------------------------

std::string TitleOf(const std::string& file_name) {
    std::string s = file_name;
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s.resize(dot);
    // Drop "(USA)", "[!]" and the like.
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') { depth++; continue; }
        if (c == ')' || c == ']') { if (depth) depth--; continue; }
        if (!depth) out += c == '_' ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    // Collapse double spaces left by removed tags.
    std::string t;
    for (char c : out) if (!(c == ' ' && !t.empty() && t.back() == ' ')) t += c;
    return t.empty() ? file_name : t;
}

std::string SaveNameOf(const std::string& file_name) {
    std::string s = file_name;
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s.resize(dot);
    for (char& c : s)
        if (std::strchr("\\/:*?\"<>|", c)) c = '_';
    return s;
}

namespace {

// UTF-8 Latin letters with accents -> plain ASCII ("Pokémon" == "Pokemon").
std::string Fold(const std::string& s) {
    static const std::map<std::string, char> table = {
        {"á", 'a'}, {"à", 'a'}, {"â", 'a'}, {"ã", 'a'}, {"ä", 'a'}, {"å", 'a'}, {"Á", 'a'}, {"À", 'a'}, {"Â", 'a'}, {"Ã", 'a'}, {"Ä", 'a'},
        {"é", 'e'}, {"è", 'e'}, {"ê", 'e'}, {"ë", 'e'}, {"É", 'e'}, {"È", 'e'}, {"Ê", 'e'},
        {"í", 'i'}, {"ì", 'i'}, {"î", 'i'}, {"ï", 'i'}, {"Í", 'i'},
        {"ó", 'o'}, {"ò", 'o'}, {"ô", 'o'}, {"õ", 'o'}, {"ö", 'o'}, {"Ó", 'o'}, {"Ô", 'o'}, {"Õ", 'o'}, {"Ö", 'o'},
        {"ú", 'u'}, {"ù", 'u'}, {"û", 'u'}, {"ü", 'u'}, {"Ú", 'u'}, {"Ü", 'u'},
        {"ç", 'c'}, {"Ç", 'c'}, {"ñ", 'n'}, {"Ñ", 'n'},
    };
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out += static_cast<char>(c); i++; continue; }
        const size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        auto it = table.find(s.substr(i, len));
        out += it != table.end() ? it->second : ' ';
        i += len;
    }
    return out;
}

std::string Sanitize(const std::string& name) {
    std::string s = name;
    for (char& c : s)
        if (std::strchr("&*/:`<>?\\|\"", c)) c = '_';
    return s;
}

struct Rank { int bad, region; size_t len; };
Rank RankOf(const std::string& name) {
    const std::string n = Lower(name);
    bool bad = false;
    for (const char* b : {"(beta", "(proto", "(kiosk", "(demo", "(sample", "(alternate", "hack]", "(hack"})
        if (n.find(b) != std::string::npos) bad = true;
    const int region = n.find("(usa") != std::string::npos ? 0 : n.find("europe") != std::string::npos ? 1
                       : n.find("brazil") != std::string::npos ? 2 : 3;
    return {bad ? 1 : 0, region, name.size()};
}

std::vector<std::string> CoverRepos(const Game& g) {
    const std::string ext = Lower(g.file_name.substr(g.file_name.find_last_of('.') + 1));
    switch (g.sys) {
        case Sys::GBA: return {"Nintendo_-_Game_Boy_Advance"};
        case Sys::GB: return ext == "gbc" ? std::vector<std::string>{"Nintendo_-_Game_Boy_Color", "Nintendo_-_Game_Boy"}
                                          : std::vector<std::string>{"Nintendo_-_Game_Boy", "Nintendo_-_Game_Boy_Color"};
        case Sys::NDS: return {"Nintendo_-_Nintendo_DS"};
        case Sys::N3DS: return {"Nintendo_-_Nintendo_3DS"};
        case Sys::NES: return {"Nintendo_-_Nintendo_Entertainment_System"};
        case Sys::SNES: return {"Nintendo_-_Super_Nintendo_Entertainment_System"};
        case Sys::MD: return {"Sega_-_Mega_Drive_-_Genesis"};
        case Sys::SMS: return {"Sega_-_Master_System_-_Mark_III"};
        case Sys::GG: return {"Sega_-_Game_Gear"};
        case Sys::PS1: return {"Sony_-_PlayStation"};
        default: return {};
    }
}

}  // namespace

std::vector<std::string> Tokens(const std::string& name) {
    static const std::set<std::string> stop = {"the", "version", "a", "of", "and", "ver"};
    std::string s = name;
    // Drop a file extension.
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && s.size() - dot >= 3 && s.size() - dot <= 5) s.resize(dot);
    s = Fold(s);
    std::string clean;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') { depth++; clean += ' '; continue; }
        if (c == ')' || c == ']') { if (depth) depth--; continue; }
        if (depth) continue;
        if (c == '&') { clean += " and "; continue; }
        if (c == '\'') continue;
        clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::vector<std::string> out;
    std::string cur;
    for (char c : clean + " ") {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += c;
        else if (!cur.empty()) {
            if (!stop.count(cur)) out.push_back(cur);
            cur.clear();
        }
    }
    return out;
}

std::string BestMatch(const std::string& file_name, const std::vector<std::string>& names) {
    std::string base = file_name;
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base.resize(dot);
    const std::string exact = Sanitize(base);
    if (std::binary_search(names.begin(), names.end(), exact) || std::find(names.begin(), names.end(), exact) != names.end())
        return exact;
    const auto gt = Tokens(file_name);
    if (gt.empty()) return "";
    std::string gc;
    for (auto& t : gt) gc += t;
    const std::set<std::string> gset(gt.begin(), gt.end());
    std::string best;
    int best_score = 0;
    for (const auto& n : names) {
        const auto ct = Tokens(n);
        if (ct.empty()) continue;
        std::string cc;
        for (auto& t : ct) cc += t;
        const std::set<std::string> cset(ct.begin(), ct.end());
        int score;
        if (cc == gc) score = 100;
        else if (gt.size() >= 2 && std::includes(cset.begin(), cset.end(), gset.begin(), gset.end()))
            score = 90 - static_cast<int>(cset.size() - gset.size()) * 2;
        else if (gt.size() >= 2 && cc.find(gc) != std::string::npos && cc.rfind(gt[0], 0) == 0)
            score = 85 - static_cast<int>(cc.size() - gc.size()) / 4;
        else {
            size_t inter = 0;
            for (auto& t : gset) inter += cset.count(t);
            const size_t uni = gset.size() + cset.size() - inter;
            score = static_cast<int>(100 * inter / (uni ? uni : 1));
        }
        if (score < 80) continue;
        bool better = best.empty() || score > best_score;
        if (!better && score == best_score) {
            const Rank a = RankOf(n), b = RankOf(best);
            better = std::tie(a.bad, a.region, a.len) < std::tie(b.bad, b.region, b.len);
        }
        if (better) {
            best = n;
            best_score = score;
        }
    }
    return best;
}

// --- Library -----------------------------------------------------------------

Library& Library::Get() {
    static Library l;
    return l;
}

void Library::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    if (ReadText(JoinPath(Paths::Get().data, "library.json"), &text)) {
        auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_object()) {
            for (auto& f : j.value("folders", nlohmann::json::array())) folders_.push_back(f.get<std::string>());
            for (auto& f : j.value("favorites", nlohmann::json::array())) favorites_[f.get<std::string>()] = true;
            for (auto& g : j.value("games", nlohmann::json::array())) {
                Game x;
                x.path = g.value("path", "");
                x.file_name = g.value("file", "");
                const SystemInfo* s = SystemByKey(g.value("system", ""));
                if (!s || x.path.empty()) continue;
                x.sys = s->id;
                x.size = g.value("size", uint64_t{0});
                x.cover = g.value("cover", "");
                x.title = TitleOf(x.file_name);
                x.save_name = SaveNameOf(x.file_name);
                games_.push_back(std::move(x));
            }
        }
    }
}

void Library::Save() {
    nlohmann::json j;
    j["folders"] = folders_;
    nlohmann::json favs = nlohmann::json::array();
    for (auto& [p, on] : favorites_) if (on) favs.push_back(p);
    j["favorites"] = favs;
    nlohmann::json gs = nlohmann::json::array();
    for (auto& g : games_)
        gs.push_back({{"path", g.path}, {"file", g.file_name}, {"system", Info(g.sys).key}, {"size", g.size}, {"cover", g.cover}});
    j["games"] = gs;
    WriteText(JoinPath(Paths::Get().data, "library.json"), j.dump());
}

std::vector<Game> Library::games() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return games_;
}

std::vector<std::string> Library::folders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return folders_;
}

void Library::AddFolder(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(folders_.begin(), folders_.end(), dir) == folders_.end()) folders_.push_back(dir);
    Save();
}

void Library::RemoveFolder(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    folders_.erase(std::remove(folders_.begin(), folders_.end(), dir), folders_.end());
    Save();
}

namespace {

struct Found {
    std::vector<Game> games;
    std::map<std::string, std::string> images;   // lower-case base name (with dir) -> image path
};

void Walk(const std::string& dir, int depth, Found* out) {
    if (depth > 5) return;
    for (const auto& e : ListDir(dir)) {
        const std::string path = JoinPath(dir, e.name);
        if (e.dir) {
            Walk(path, depth + 1, out);
            continue;
        }
        const std::string lower = Lower(e.name);
        const size_t dot = lower.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : lower.substr(dot + 1);
        if (ext == "png" || ext == "jpg" || ext == "jpeg") {
            out->images[Lower(JoinPath(ParentDir(path), e.name.substr(0, dot)))] = path;
            out->images[Lower(e.name.substr(0, dot))] = path;   // any folder (Skraper media/)
            continue;
        }
        const SystemInfo* s = SystemForFile(e.name, e.size);
        if (!s) continue;
        const bool sheet = ext == "cue" || ext == "m3u" || ext == "ccd";
        if (e.size < 1024 && !sheet) continue;
        Game g;
        g.path = path;
        g.file_name = e.name;
        g.sys = s->id;
        g.size = e.size;
        g.title = TitleOf(e.name);
        g.save_name = SaveNameOf(e.name);
        out->games.push_back(std::move(g));
    }
}

// Files named by .cue sheets and .m3u lists are played through them.
std::set<std::string> ListedByPlaylists(const std::vector<Game>& games) {
    std::set<std::string> out;
    for (const auto& g : games) {
        const std::string lower = Lower(g.file_name);
        const bool m3u = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".m3u") == 0;
        const bool cue = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".cue") == 0;
        if (!m3u && !cue) continue;
        std::string text;
        if (!ReadText(g.path, &text)) continue;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos || line[a] == '#') continue;
            line = line.substr(a);
            std::string name;
            if (m3u) {
                name = line;
            } else {
                if (Lower(line.substr(0, 5)) != "file ") continue;
                const size_t q1 = line.find('"');
                const size_t q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) name = line.substr(q1 + 1, q2 - q1 - 1);
                else name = line.substr(5, line.find(' ', 5) - 5);
            }
            for (char& c : name) if (c == '\\') c = '/';
            out.insert(Lower(JoinPath(ParentDir(g.path), name)));
        }
    }
    return out;
}

}  // namespace

void Library::Scan() {
    scanning_ = true;
    const auto dirs = folders();
    Found found;
    for (const auto& d : dirs) Walk(d, 0, &found);
    const auto listed = ListedByPlaylists(found.games);
    std::vector<Game> games;
    for (auto& g : found.games) {
        if (listed.count(Lower(g.path))) continue;
        const std::string base = g.path.substr(0, g.path.find_last_of('.'));
        auto it = found.images.find(Lower(base));
        if (it == found.images.end()) it = found.images.find(Lower(g.file_name.substr(0, g.file_name.find_last_of('.'))));
        if (it != found.images.end()) g.cover = it->second;
        games.push_back(std::move(g));
    }
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return Lower(a.title) < Lower(b.title); });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        games_ = std::move(games);
        Save();
    }
    scanning_ = false;
}

bool Library::IsFavorite(const Game& g) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = favorites_.find(g.path);
    return it != favorites_.end() && it->second;
}

void Library::SetFavorite(const Game& g, bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (on) favorites_[g.path] = true; else favorites_.erase(g.path);
    Save();
}

int64_t Library::LastPlayed(const Game& g) const { return PlayStats::LastPlayed(g.path); }
int64_t Library::PlayTime(const Game& g) const { return PlayStats::PlayTimeMs(g.path); }

const Facts* Library::FactsFor(const Game& g) {
    std::vector<std::string> files;
    const std::string ext = Lower(g.file_name.substr(g.file_name.find_last_of('.') + 1));
    switch (g.sys) {
        case Sys::GB: files = ext == "gbc" ? std::vector<std::string>{"Nintendo - Game Boy Color", "Nintendo - Game Boy"}
                                           : std::vector<std::string>{"Nintendo - Game Boy", "Nintendo - Game Boy Color"}; break;
        case Sys::GBA: files = {"Nintendo - Game Boy Advance"}; break;
        case Sys::NDS: files = {"Nintendo - Nintendo DS"}; break;
        case Sys::N3DS: files = {"Nintendo - Nintendo 3DS"}; break;
        case Sys::NES: files = {"Nintendo - Nintendo Entertainment System"}; break;
        case Sys::SNES: files = {"Nintendo - Super Nintendo Entertainment System"}; break;
        case Sys::MD: files = {"Sega - Mega Drive - Genesis"}; break;
        case Sys::SMS: files = {"Sega - Master System - Mark III"}; break;
        case Sys::GG: files = {"Sega - Game Gear"}; break;
        case Sys::PS1: files = {"Sony - PlayStation"}; break;
        default: break;
    }
    std::string key;
    for (auto& t : Tokens(g.file_name)) key += t;
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(meta_mutex_);
    for (const auto& f : files) {
        auto it = meta_.find(f);
        if (it == meta_.end()) {
            std::map<std::string, Facts> table;
            std::string text;
            if (ReadText(JoinPath(JoinPath(Paths::Get().assets, "meta"), f + ".tsv"), &text)) {
                std::istringstream in(text);
                std::string line;
                std::vector<Facts> all;
                while (std::getline(in, line)) {
                    std::vector<std::string> p;
                    std::string cur;
                    for (char c : line) { if (c == '\t') { p.push_back(cur); cur.clear(); } else if (c != '\r') cur += c; }
                    p.push_back(cur);
                    if (p.size() < 6) continue;
                    all.push_back({p[0], p[1], p[2], p[3], p[4], p[5]});
                }
                // USA / Europe first, so they win for a shared title.
                std::stable_sort(all.begin(), all.end(), [](const Facts& a, const Facts& b) {
                    auto r = [](const std::string& n) { return n.find("(USA") != std::string::npos ? 0 : n.find("Europe") != std::string::npos ? 1 : 2; };
                    return r(a.name) < r(b.name);
                });
                for (auto& x : all) {
                    std::string k;
                    for (auto& t : Tokens(x.name)) k += t;
                    table.emplace(k, x);
                }
            }
            it = meta_.emplace(f, std::move(table)).first;
        }
        auto hit = it->second.find(key);
        if (hit != it->second.end()) return &hit->second;
    }
    return nullptr;
}

// --- Covers --------------------------------------------------------------------

Covers& Covers::Get() {
    static Covers c;
    return c;
}

namespace {
std::string CoverDir() {
    const std::string d = JoinPath(Paths::Get().cache, "covers");
    MakeDirs(d);
    return d;
}
std::string CoverFile(const Game& g, const char* kind) {
    return JoinPath(CoverDir(), std::string(Info(g.sys).key) + "_" + Sanitize(g.save_name) + kind + ".png");
}
bool Download(const std::string& url, const std::string& to, bool* offline) {
    HttpResult r = HttpGet(url, 20000, 16u << 20);
    if (r.status == 0 || r.status >= 500 || r.status == 403 || r.status == 429) { *offline = true; return false; }
    if (!r.ok() || r.body.size() < 64) return false;
    return WriteFileBytes(to, r.body.data(), r.body.size());
}
}  // namespace

std::vector<std::string> Covers::Index(const std::string& repo) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = indexes_.find(repo);
    if (it != indexes_.end()) return it->second;
    std::vector<std::string> names;
    std::string text;
    if (ReadText(JoinPath(JoinPath(Paths::Get().assets, "covers"), repo + ".txt"), &text)) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) names.push_back(line);
        }
    }
    indexes_[repo] = names;
    return names;
}

std::string Covers::LibretroName(const Game& g, std::string* repo) {
    for (const auto& r : CoverRepos(g)) {
        const auto names = Index(r);
        const std::string n = BestMatch(g.file_name, names);
        if (!n.empty()) {
            if (repo) *repo = r;
            return n;
        }
    }
    return "";
}

std::string Covers::Cached(const Game& g) {
    if (!g.cover.empty() && FileExists(g.cover)) return g.cover;
    const std::string f = CoverFile(g, "");
    return FileExists(f) ? f : "";
}

std::string Covers::Fetch(const Game& g) {
    const std::string have = Cached(g);
    if (!have.empty()) return have;
    if (!Settings::Get().Bool("download_covers", true)) return "";
    const std::string f = CoverFile(g, "");
    const std::string miss = f + ".none";
    if (FileExists(miss) && NowMs() - FileTime(miss) < 3LL * 24 * 3600 * 1000) return "";
    bool offline = false;
    std::string repo;
    const std::string name = LibretroName(g, &repo);
    if (!name.empty()) {
        const std::string url = "https://raw.githubusercontent.com/libretro-thumbnails/" + repo + "/master/Named_Boxarts/" + UrlEncode(name) + ".png";
        if (Download(url, f, &offline)) return f;
    }
    if (!offline) WriteText(miss, "");
    return "";
}

std::string Covers::FetchKind(const Game& g, const char* folder) {
    const std::string f = CoverFile(g, (std::string("_") + folder).c_str());
    if (FileExists(f)) return f;
    const std::string miss = f + ".none";
    if (FileExists(miss) && NowMs() - FileTime(miss) < 5LL * 24 * 3600 * 1000) return "";
    if (!Settings::Get().Bool("game_media", true)) return "";
    bool offline = false;
    std::string repo;
    const std::string name = LibretroName(g, &repo);
    if (!name.empty()) {
        const std::string url = "https://raw.githubusercontent.com/libretro-thumbnails/" + repo + "/master/" + folder + "/" + UrlEncode(name) + ".png";
        if (Download(url, f, &offline)) return f;
    }
    if (!offline) WriteText(miss, "");
    return "";
}

void Covers::ForgetMisses() {
    for (const auto& e : ListDir(CoverDir()))
        if (e.name.size() > 5 && e.name.compare(e.name.size() - 5, 5, ".none") == 0)
            RemoveFile(JoinPath(CoverDir(), e.name));
}

}  // namespace cartucho
