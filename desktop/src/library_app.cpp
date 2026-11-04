#include "library_app.h"

#include "cloud.h"
#include "updater.h"

#include <SDL.h>
#include <glad/gles2.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <thread>
#include <tuple>

#include "backdrop.h"
#include "images.h"
#include "library.h"
#include "os.h"
#include "paths.h"
#include "playstats.h"
#include "settings.h"
#include "settings_ui.h"
#include "sounds.h"
#include "systems.h"
#include "testhook.h"
#include "window.h"

namespace cartucho {

void OpenLog(const std::string& name);

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

ImU32 Rgb(uint32_t rgb, int a = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }
uint32_t Blend(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int s) { return static_cast<uint32_t>(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t) << s; };
    return ch(16) | ch(8) | ch(0);
}

// Buttons of the hint bar (A red, B orange, Y green, X blue as on Android).
const uint32_t kHintA = 0xEF4444, kHintB = 0xF59E0B, kHintY = 0x22C55E, kHintX = 0x3B82F6;

struct Collection {
    std::string key;     // "recent", "fav", "all", "search" or a system key
    std::string title;
    uint32_t color = 0x6B4EFF;
    std::vector<Game> games;
    std::string subtitle;
    const SystemInfo* sys = nullptr;
};

enum class Key { kNone, kLeft, kRight, kUp, kDown, kA, kB, kX, kY, kL, kR, kStart };

using Hint = std::tuple<std::string, uint32_t, std::string, std::function<void()>>;

Texture MakeGradient() {
    // Diagonal light-to-dark, multiplied by the card's colour when drawn.
    const int n = 64;
    std::vector<uint8_t> px(n * n * 4);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const float t = (x + y) / (2.0f * (n - 1));
            const uint8_t v = static_cast<uint8_t>(255 * (1.0f - 0.55f * t));
            uint8_t* p = &px[(y * n + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    return TextureFromRgba(px.data(), n, n);
}

Texture MakeGlow() {
    const int n = 64;
    std::vector<uint8_t> px(n * n * 4);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const float dx = (x - n / 2.0f) / (n / 2.0f), dy = (y - n / 2.0f) / (n / 2.0f);
            const float d = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy));
            uint8_t* p = &px[(y * n + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(255 * d * d);
        }
    return TextureFromRgba(px.data(), n, n);
}

void Fit(const Texture& t, ImVec2 box0, ImVec2 box1, ImVec2* p0, ImVec2* p1) {
    const float bw = box1.x - box0.x, bh = box1.y - box0.y;
    const float s = std::min(bw / t.w, bh / t.h);
    const float w = t.w * s, h = t.h * s;
    *p0 = ImVec2(box0.x + (bw - w) / 2, box0.y + (bh - h) / 2);
    *p1 = ImVec2(p0->x + w, p0->y + h);
}

ImVec2 TextSize(ImFont* f, const std::string& s, float wrap = 0) {
    return f->CalcTextSizeA(f->FontSize, FLT_MAX, wrap, s.c_str());
}

}  // namespace

class LibraryUi {
public:
    int Run();

private:
    void Refresh();
    void StartScan();
    void DrawHome();
    void DrawList();
    void DrawHints(const std::vector<Hint>& hints);
    void DrawTopBar();
    void DrawCard(ImDrawList* dl, const Collection& c, ImVec2 center, float w, float h, float alpha, bool focused);
    void DrawCollectionArt(ImDrawList* dl, const Collection& c, ImVec2 center, float size, float alpha);
    void DrawPanel(ImVec2 p0, ImVec2 p1);
    void DrawModals();
    void DrawEmpty();
    void HandleKey(Key k);
    void MoveCarousel(int step);
    void OpenCollection();
    void BackHome();
    void SelectGame(int i);
    void Play(const Game& g, bool resume = true);
    void ToggleFavorite();
    void AddFolder();
    void ApplyTab();
    void WaitForGame(struct Process* p);
    void Notice(const std::string& text, double secs = 2.5) { notice_ = text; notice_until_ = Now() + secs; }

    Window win_;
    Backdrop backdrop_;
    Sounds sounds_;
    std::unique_ptr<Images> images_;
    SettingsUi settings_ui_;
    std::vector<Collection> cols_;
    int centered_ = 0;
    float carousel_x_ = 0;     // animated position, in cards
    bool in_list_ = false;
    Collection current_;
    std::vector<std::pair<std::string, std::function<bool(const Game&)>>> tabs_;
    int tab_ = 0;
    std::vector<Game> shown_;
    int selected_ = 0;
    float list_scroll_ = 0;    // animated
    double selected_at_ = 0;
    std::atomic<bool> scan_running_{false};
    std::thread scan_thread_;
    std::vector<Texture> photos_;
    Texture grad_, glow_;
    enum class Modal { kNone, kMainMenu, kGameMenu, kSearch, kDetails, kSettings } modal_ = Modal::kNone;
    int modal_sel_ = 0;
    bool modal_activate_ = false;
    int modal_move_ = 0;
    char search_[128] = {0};
    std::string notice_;
    double notice_until_ = 0;
    bool quit_ = false;
    // Background jobs (cloud copy after playing, update check).
    void Background(std::function<std::string()> job);
    std::thread bg_;
    std::mutex bg_mutex_;
    std::string bg_notice_;
};

void LibraryUi::Background(std::function<std::string()> job) {
    if (bg_.joinable()) bg_.join();
    bg_ = std::thread([this, job] {
        const std::string m = job();
        std::lock_guard<std::mutex> lock(bg_mutex_);
        if (!m.empty()) bg_notice_ = m;
    });
}

int RunLibrary() {
    LibraryUi ui;
    return ui.Run();
}

// --- Data ----------------------------------------------------------------------------

void LibraryUi::Refresh() {
    auto& lib = Library::Get();
    const auto games = lib.games();
    const std::string keep = cols_.empty() ? "" : cols_[static_cast<size_t>(std::clamp(centered_, 0, static_cast<int>(cols_.size()) - 1))].key;
    std::vector<Collection> out;
    std::vector<Game> recent;
    for (auto& g : games) if (lib.LastPlayed(g) > 0) recent.push_back(g);
    std::sort(recent.begin(), recent.end(), [&](const Game& a, const Game& b) { return lib.LastPlayed(a) > lib.LastPlayed(b); });
    if (recent.size() > 30) recent.resize(30);
    if (!recent.empty())
        out.push_back({"recent", "Continuar jogando", 0xFF8C42, recent,
                       "Último: " + recent[0].title + " · " + PlayStats::FormatAgo(lib.LastPlayed(recent[0]))});
    std::vector<Game> favs;
    for (auto& g : games) if (lib.IsFavorite(g)) favs.push_back(g);
    if (!favs.empty()) out.push_back({"fav", "Favoritos", 0xFFB547, favs, std::to_string(favs.size()) + (favs.size() == 1 ? " jogo" : " jogos")});
    std::set<Sys> systems;
    for (const auto& s : Systems()) {
        std::vector<Game> g;
        for (auto& x : games) if (x.sys == s.id) g.push_back(x);
        if (g.empty()) continue;
        systems.insert(s.id);
        int64_t time = 0;
        for (auto& x : g) time += lib.PlayTime(x);
        std::string sub = std::to_string(g.size()) + (g.size() == 1 ? " jogo" : " jogos");
        if (time > 0) sub += " · " + PlayStats::FormatTime(time) + " jogadas";
        out.push_back({s.key, s.full_name, s.color, g, sub, &s});
    }
    if (systems.size() > 1) out.push_back({"all", "Todos os jogos", 0x7C5CFF, games, std::to_string(games.size()) + " jogos"});
    cols_ = std::move(out);
    centered_ = 0;
    for (size_t i = 0; i < cols_.size(); i++) if (cols_[i].key == keep) centered_ = static_cast<int>(i);
    carousel_x_ = static_cast<float>(centered_);
    if (in_list_ && current_.key != "search") {
        bool found = false;
        for (auto& c : cols_)
            if (c.key == current_.key) { current_ = c; ApplyTab(); found = true; break; }
        if (!found) { in_list_ = false; }
    }
    if (!in_list_ && !cols_.empty()) backdrop_.SetTint(cols_[static_cast<size_t>(centered_)].color, static_cast<float>(Now()));
}

void LibraryUi::StartScan() {
    if (scan_running_) return;
    if (scan_thread_.joinable()) scan_thread_.join();
    scan_running_ = true;
    scan_thread_ = std::thread([this] {
        Library::Get().Scan();
        scan_running_ = false;
    });
}

// --- Actions -----------------------------------------------------------------------------

void LibraryUi::MoveCarousel(int step) {
    if (cols_.empty()) return;
    const int to = std::clamp(centered_ + step, 0, static_cast<int>(cols_.size()) - 1);
    if (to == centered_) return;
    centered_ = to;
    sounds_.Play(Sounds::kMove);
    backdrop_.SetTint(cols_[static_cast<size_t>(to)].color, static_cast<float>(Now()));
}

void LibraryUi::OpenCollection() {
    if (cols_.empty()) return;
    current_ = cols_[static_cast<size_t>(centered_)];
    sounds_.Play(Sounds::kSelect);
    in_list_ = true;
    tab_ = 0;
    // Tabs: all, favourites, recent, the most common genres.
    tabs_.clear();
    tabs_.push_back({"Todos", [](const Game&) { return true; }});
    if (current_.key != "fav") tabs_.push_back({"Favoritos", [](const Game& g) { return Library::Get().IsFavorite(g); }});
    if (current_.key != "recent") tabs_.push_back({"Recentes", [](const Game& g) { return Library::Get().LastPlayed(g) > 0; }});
    std::map<std::string, int> genres;
    for (auto& g : current_.games)
        if (const Facts* f = Library::Get().FactsFor(g)) {
            std::string ge = f->genre.substr(0, f->genre.find_first_of(",(/"));
            while (!ge.empty() && ge.back() == ' ') ge.pop_back();
            if (!ge.empty()) genres[ge]++;
        }
    std::vector<std::pair<int, std::string>> top;
    for (auto& [k, v] : genres) if (v >= 2) top.push_back({-v, k});
    std::sort(top.begin(), top.end());
    for (size_t i = 0; i < top.size() && i < 4; i++) {
        const std::string ge = top[i].second;
        tabs_.push_back({ge, [ge](const Game& g) {
                             const Facts* f = Library::Get().FactsFor(g);
                             return f && f->genre.rfind(ge, 0) == 0;
                         }});
    }
    selected_ = 0;
    shown_.clear();
    ApplyTab();
    list_scroll_ = 0;
    selected_at_ = Now();
    backdrop_.SetTint(current_.color, static_cast<float>(Now()));
}

void LibraryUi::ApplyTab() {
    const std::string keep = (selected_ >= 0 && selected_ < static_cast<int>(shown_.size())) ? shown_[static_cast<size_t>(selected_)].path : "";
    tab_ = std::clamp(tab_, 0, static_cast<int>(tabs_.size()) - 1);
    shown_.clear();
    const auto& f = tabs_[static_cast<size_t>(tab_)].second;
    for (auto& g : current_.games) if (f(g)) shown_.push_back(g);
    if (tabs_[static_cast<size_t>(tab_)].first == "Recentes")
        std::sort(shown_.begin(), shown_.end(), [](const Game& a, const Game& b) { return Library::Get().LastPlayed(a) > Library::Get().LastPlayed(b); });
    selected_ = 0;
    for (size_t i = 0; i < shown_.size(); i++) if (shown_[i].path == keep) selected_ = static_cast<int>(i);
    selected_at_ = Now();
}

void LibraryUi::BackHome() {
    if (!in_list_) return;
    sounds_.Play(Sounds::kBack);
    in_list_ = false;
    if (!cols_.empty()) backdrop_.SetTint(cols_[static_cast<size_t>(centered_)].color, static_cast<float>(Now()));
}

void LibraryUi::SelectGame(int i) {
    if (shown_.empty()) return;
    i = std::clamp(i, 0, static_cast<int>(shown_.size()) - 1);
    if (i == selected_) return;
    selected_ = i;
    selected_at_ = Now();
    sounds_.Play(Sounds::kMove);
}

void LibraryUi::ToggleFavorite() {
    if (selected_ < 0 || selected_ >= static_cast<int>(shown_.size())) return;
    const Game g = shown_[static_cast<size_t>(selected_)];
    const bool on = !Library::Get().IsFavorite(g);
    Library::Get().SetFavorite(g, on);
    sounds_.Play(Sounds::kSelect);
    Notice(on ? "★ Nos favoritos" : "Fora dos favoritos");
    Refresh();
}

void LibraryUi::Play(const Game& g, bool resume) {
    if (!FileExists(CorePath(Info(g.sys)))) {
        Notice(std::string("O emulador de ") + Info(g.sys).full_name + " não foi encontrado na pasta cores", 5);
        return;
    }
    sounds_.Play(Sounds::kLaunch);
    SDL_Delay(220);
    std::vector<std::string> args = {"--play", g.path, "--system", Info(g.sys).key, "--save-name", g.save_name, "--title", g.title};
    if (!resume) args.push_back("--no-resume");
    if (!g.entry.empty()) {
        args.push_back("--entry");
        args.push_back(g.entry);
    }
    Process* p = StartSelf(args);
    if (!p) {
        Notice("Não foi possível abrir o jogo", 4);
        return;
    }
    WaitForGame(p);
}

// The game runs in its own window and process; the library waits hidden.
void LibraryUi::WaitForGame(Process* p) {
    sounds_.Pause(true);
    SDL_HideWindow(win_.sdl());
    while (ProcessRunning(p)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(e.cdevice.which);
        }
        SDL_Delay(100);
    }
    const int code = ProcessExitCode(p);
    ProcessFree(p);
    SDL_ShowWindow(win_.sdl());
    SDL_RaiseWindow(win_.sdl());
    sounds_.Pause(false);
    if (code != 0) Notice("O jogo fechou com um problema (veja game.log na pasta de dados)", 6);
    // Saves in the cloud: the copy is rewritten after playing.
    if (!Cloud::File().empty() && Cloud::Auto())
        Background([] {
            if (Backup::LastChange() <= Cloud::LastSent()) return std::string();
            const auto r = Cloud::Send();
            return r.ok ? std::string("Saves enviados para a nuvem") : r.message;
        });
    // Drop the old "continue" picture so the new one shows.
    if (images_) images_->Forget(PlayStats::ThumbPath(selected_ < static_cast<int>(shown_.size()) ? shown_[static_cast<size_t>(selected_)].path : ""));
    Refresh();
    selected_at_ = Now();
}

void LibraryUi::AddFolder() {
    std::string dir = PickFolder("Escolha a pasta dos seus jogos");
    if (dir.empty())
        if (const char* env = std::getenv("CARTUCHO_ADD_FOLDER")) dir = env;   // tests
    if (dir.empty()) return;
    Library::Get().AddFolder(dir);
    Notice("Procurando jogos em " + FileName(dir) + "…");
    StartScan();
}

void LibraryUi::HandleKey(Key k) {
    if (modal_ != Modal::kNone) {
        switch (k) {
            case Key::kUp: modal_move_ = -1; break;
            case Key::kDown: modal_move_ = 1; break;
            case Key::kA: modal_activate_ = true; break;
            case Key::kB:
            case Key::kStart:
                if (modal_ == Modal::kSettings) settings_ui_.Back();
                else { modal_ = Modal::kNone; sounds_.Play(Sounds::kBack); }
                break;
            default: break;
        }
        return;
    }
    if (!in_list_) {
        switch (k) {
            case Key::kLeft: MoveCarousel(-1); break;
            case Key::kRight: MoveCarousel(1); break;
            case Key::kL: MoveCarousel(-3); break;
            case Key::kR: MoveCarousel(3); break;
            case Key::kA:
                if (cols_.empty()) AddFolder();
                else OpenCollection();
                break;
            case Key::kY:
                if (!cols_.empty() && cols_[0].key == "recent") Play(cols_[0].games[0]);
                break;
            case Key::kX: if (!cols_.empty()) { modal_ = Modal::kSearch; search_[0] = 0; } break;
            case Key::kStart: modal_ = Modal::kMainMenu; modal_sel_ = 0; sounds_.Play(Sounds::kSelect); break;
            default: break;
        }
        return;
    }
    switch (k) {
        case Key::kUp: SelectGame(selected_ - 1); break;
        case Key::kDown: SelectGame(selected_ + 1); break;
        case Key::kLeft: SelectGame(selected_ - 10); break;
        case Key::kRight: SelectGame(selected_ + 10); break;
        case Key::kL: if (tab_ > 0) { tab_--; ApplyTab(); sounds_.Play(Sounds::kMove); } break;
        case Key::kR: if (tab_ + 1 < static_cast<int>(tabs_.size())) { tab_++; ApplyTab(); sounds_.Play(Sounds::kMove); } break;
        case Key::kA: if (!shown_.empty()) Play(shown_[static_cast<size_t>(selected_)]); break;
        case Key::kB: BackHome(); break;
        case Key::kY: ToggleFavorite(); break;
        case Key::kX: if (!shown_.empty()) { modal_ = Modal::kGameMenu; modal_sel_ = 0; sounds_.Play(Sounds::kSelect); } break;
        case Key::kStart: modal_ = Modal::kMainMenu; modal_sel_ = 0; sounds_.Play(Sounds::kSelect); break;
        default: break;
    }
}

// --- Drawing ---------------------------------------------------------------------------

void LibraryUi::DrawCollectionArt(ImDrawList* dl, const Collection& c, ImVec2 ctr, float size, float alpha) {
    const int a = static_cast<int>(alpha * 255);
    const float r = size * 0.36f;
    if (c.sys) {
        const Texture& photo = photos_[static_cast<size_t>(c.sys->id)];
        if (photo.valid()) {
            dl->AddImage(glow_.im(), ImVec2(ctr.x - size * 0.55f, ctr.y - size * 0.55f), ImVec2(ctr.x + size * 0.55f, ctr.y + size * 0.55f),
                         ImVec2(0, 0), ImVec2(1, 1), Rgb(Blend(c.color, 0xFFFFFF, 0.6f), a * 150 / 255));
            ImVec2 p0, p1;
            Fit(photo, ImVec2(ctr.x - size * 0.45f, ctr.y - size * 0.4f), ImVec2(ctr.x + size * 0.45f, ctr.y + size * 0.4f), &p0, &p1);
            dl->AddImage(photo.im(), ImVec2(p0.x + 4, p0.y + 9), ImVec2(p1.x + 4, p1.y + 9), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0, 0, 0, a * 90 / 255));
            dl->AddImage(photo.im(), p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
        }
        return;
    }
    // Collections: drawn icons (play, star, grid, magnifier).
    if (c.key == "all") {
        const float q = r * 0.8f, g = r * 0.1f;
        const uint32_t cs[4] = {0xB9A2FF, 0x5DA9FF, 0x3FD69B, 0xFF8A5B};
        for (int i = 0; i < 4; i++) {
            const float x = ctr.x + (i % 2 ? g : -g - q), y = ctr.y + (i / 2 ? g : -g - q);
            dl->AddRectFilled(ImVec2(x + 3, y + 6), ImVec2(x + q + 3, y + q + 6), IM_COL32(0, 0, 0, a / 4), q * 0.22f);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + q, y + q), Rgb(cs[i], a), q * 0.22f);
        }
        return;
    }
    dl->AddImage(glow_.im(), ImVec2(ctr.x - size * 0.55f, ctr.y - size * 0.55f), ImVec2(ctr.x + size * 0.55f, ctr.y + size * 0.55f),
                 ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a / 3));
    dl->AddCircleFilled(ImVec2(ctr.x + 3, ctr.y + 7), r, IM_COL32(0, 0, 0, a / 4), 64);
    dl->AddCircleFilled(ctr, r, IM_COL32(255, 255, 255, a), 64);
    const ImU32 ink = Rgb(Blend(c.color, 0x000000, 0.1f), a);
    if (c.key == "fav") {
        ImVec2 pts[10];
        for (int i = 0; i < 10; i++) {
            const float ang = -3.14159265f / 2 + i * 3.14159265f / 5;
            const float rr = (i % 2) ? r * 0.27f : r * 0.62f;
            pts[i] = ImVec2(ctr.x + std::cos(ang) * rr, ctr.y + std::sin(ang) * rr + r * 0.04f);
        }
        dl->AddConcavePolyFilled(pts, 10, ink);
    } else if (c.key == "search") {
        dl->AddCircle(ImVec2(ctr.x - r * 0.1f, ctr.y - r * 0.1f), r * 0.32f, ink, 32, r * 0.12f);
        dl->AddLine(ImVec2(ctr.x + r * 0.13f, ctr.y + r * 0.13f), ImVec2(ctr.x + r * 0.42f, ctr.y + r * 0.42f), ink, r * 0.14f);
    } else {
        dl->AddTriangleFilled(ImVec2(ctr.x - r * 0.22f, ctr.y - r * 0.4f), ImVec2(ctr.x + r * 0.45f, ctr.y),
                              ImVec2(ctr.x - r * 0.22f, ctr.y + r * 0.4f), ink);
    }
}

void LibraryUi::DrawCard(ImDrawList* dl, const Collection& c, ImVec2 ctr, float w, float h, float alpha, bool focused) {
    const float s = win_.scale();
    const ImVec2 p0(ctr.x - w / 2, ctr.y - h / 2), p1(ctr.x + w / 2, ctr.y + h / 2);
    const float r = w * 0.085f;
    const int a = static_cast<int>(alpha * 255);
    if (focused)
        dl->AddImage(glow_.im(), ImVec2(p0.x - w * 0.35f, p0.y - h * 0.3f), ImVec2(p1.x + w * 0.35f, p1.y + h * 0.3f), ImVec2(0, 0), ImVec2(1, 1),
                     Rgb(c.color, 110));
    dl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 14), ImVec2(p1.x + 6, p1.y + 14), IM_COL32(0, 0, 0, a / 3), r);
    dl->AddImageRounded(grad_.im(), p0, p1, ImVec2(0, 0), ImVec2(1, 1), Rgb(Blend(c.color, 0xFFFFFF, 0.12f), a), r);
    // A soft shine on the top half.
    dl->AddRectFilledMultiColor(ImVec2(p0.x + r * 0.3f, p0.y + 2), ImVec2(p1.x - r * 0.3f, p0.y + h * 0.35f),
                                IM_COL32(255, 255, 255, a / 7), IM_COL32(255, 255, 255, a / 7), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    DrawCollectionArt(dl, c, ImVec2(ctr.x, p0.y + h * 0.42f), w * 0.82f, alpha);
    if (focused) dl->AddRect(ImVec2(p0.x - 4, p0.y - 4), ImVec2(p1.x + 4, p1.y + 4), IM_COL32(255, 255, 255, a), r + 4, 0, 4 * s);
    const float k = w / (260 * s);
    ImFont* bold = win_.fonts().bold;
    ImFont* small = win_.fonts().small;
    const float fs = bold->FontSize * std::clamp(k * 1.05f, 0.6f, 1.15f);
    const float ss = small->FontSize * std::clamp(k * 1.05f, 0.6f, 1.15f);
    std::string title = c.sys ? c.sys->short_name : c.title;
    dl->AddText(bold, fs, ImVec2(p0.x + 18 * k * s, p1.y - fs - ss - 26 * k * s), IM_COL32(255, 255, 255, a), title.c_str(), nullptr, w - 30 * s);
    const std::string n = std::to_string(c.games.size()) + (c.games.size() == 1 ? " jogo" : " jogos");
    dl->AddText(small, ss, ImVec2(p0.x + 18 * k * s, p1.y - ss - 18 * k * s), IM_COL32(255, 255, 255, a * 215 / 255), n.c_str());
}

void LibraryUi::DrawTopBar() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = win_.scale();
    char clock[16];
    const std::time_t t = std::time(nullptr);
    std::strftime(clock, sizeof(clock), "%H:%M", std::localtime(&t));
    std::string text = clock;
    int secs = 0, pct = -1;
    const SDL_PowerState ps = SDL_GetPowerInfo(&secs, &pct);
    if (pct >= 0 && ps != SDL_POWERSTATE_NO_BATTERY && ps != SDL_POWERSTATE_UNKNOWN)
        text += std::string("   ") + (ps == SDL_POWERSTATE_CHARGING || ps == SDL_POWERSTATE_CHARGED ? "⚡" : "") + std::to_string(pct) + "%";
    if (scan_running_) text = "Procurando jogos…     " + text;
    const ImVec2 ts = TextSize(win_.fonts().body, text);
    dl->AddText(win_.fonts().body, win_.fonts().body->FontSize, ImVec2(ds.x - ts.x - 34 * s, 30 * s), IM_COL32(230, 226, 250, 230), text.c_str());
}

void LibraryUi::DrawHints(const std::vector<Hint>& hints) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = win_.scale();
    const float h = 66 * s;
    dl->AddRectFilledMultiColor(ImVec2(0, ds.y - h * 1.6f), ds, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
    float x = 34 * s;
    const float cy = ds.y - h / 2;
    ImFont* body = win_.fonts().body;
    ImFont* bold = win_.fonts().bold;
    for (const auto& [key, color, label, act] : hints) {
        const float r = 15 * s;
        const bool round = key.size() == 1;
        const float kfs = bold->FontSize * (round ? 0.8f : 0.6f);
        const ImVec2 ks = bold->CalcTextSizeA(kfs, FLT_MAX, 0, key.c_str());
        const float kw = round ? r * 2 : ks.x + 20 * s;
        const ImVec2 lt = TextSize(body, label);
        if (act) {
            ImGui::SetCursorScreenPos(ImVec2(x - 6 * s, cy - r - 6 * s));
            ImGui::PushID(label.c_str());
            if (ImGui::InvisibleButton("##hint", ImVec2(kw + lt.x + 24 * s, r * 2 + 12 * s)) && modal_ == Modal::kNone) act();
            ImGui::PopID();
        }
        if (round) dl->AddCircleFilled(ImVec2(x + r, cy), r, Rgb(color));
        else dl->AddRectFilled(ImVec2(x, cy - r), ImVec2(x + kw, cy + r), Rgb(color ? color : 0x3A4254), r);
        dl->AddText(bold, kfs, ImVec2(x + (kw - ks.x) / 2, cy - ks.y / 2), IM_COL32(255, 255, 255, 255), key.c_str());
        dl->AddText(body, body->FontSize, ImVec2(x + kw + 10 * s, cy - lt.y / 2), IM_COL32(225, 221, 240, 240), label.c_str());
        x += kw + lt.x + 42 * s;
    }
}

// --- Home --------------------------------------------------------------------------------

void LibraryUi::DrawHome() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = win_.scale();
    dl->AddText(win_.fonts().title, win_.fonts().title->FontSize, ImVec2(34 * s, 24 * s), IM_COL32(255, 255, 255, 255), "Cartucho");
    if (cols_.empty()) return;   // the welcome window is drawn after the screen's (DrawEmpty)
    carousel_x_ += (centered_ - carousel_x_) * std::min(1.0f, ImGui::GetIO().DeltaTime * 12);
    if (std::fabs(centered_ - carousel_x_) < 0.001f) carousel_x_ = static_cast<float>(centered_);
    const float cw = std::min({ds.x * 0.2f, 300 * s, ds.y * 0.46f / 1.18f});
    const float ch = cw * 1.18f;
    const float gap = cw * 1.08f;
    const float cy = ds.y * 0.43f;
    std::vector<int> order;
    for (int i = 0; i < static_cast<int>(cols_.size()); i++) order.push_back(i);
    // Far cards first so the centred one is on top.
    std::sort(order.begin(), order.end(), [&](int a, int b) { return std::fabs(a - carousel_x_) > std::fabs(b - carousel_x_); });
    for (int i : order) {
        const float d = i - carousel_x_;
        const float x = ds.x / 2 + d * gap + (d > 0 ? 1 : -1) * std::min(1.0f, std::fabs(d)) * cw * 0.1f;
        if (x < -cw || x > ds.x + cw) continue;
        const float k = std::min(1.0f, std::fabs(d));
        const float sc = 1.0f - 0.2f * k;
        const float alpha = 1.0f - 0.4f * std::min(1.5f, std::fabs(d)) / 1.5f;
        const bool focused = i == centered_ && std::fabs(d) < 0.3f;
        DrawCard(dl, cols_[static_cast<size_t>(i)], ImVec2(x, cy + 18 * s * k), cw * sc, ch * sc, alpha, focused);
        // Mouse: click a card to centre it, click the centred one to open it.
        ImGui::SetCursorScreenPos(ImVec2(x - cw * sc / 2, cy - ch * sc / 2));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##card", ImVec2(cw * sc, ch * sc)) && modal_ == Modal::kNone) {
            if (i == centered_) OpenCollection();
            else MoveCarousel(i - centered_);
        }
        ImGui::PopID();
    }
    if (ImGui::GetIO().MouseWheel != 0 && modal_ == Modal::kNone) MoveCarousel(ImGui::GetIO().MouseWheel > 0 ? -1 : 1);
    const Collection& c = cols_[static_cast<size_t>(centered_)];
    ImFont* huge = win_.fonts().huge;
    ImFont* body = win_.fonts().body;
    const ImVec2 ts = TextSize(huge, c.title);
    const float ty = cy + ch / 2 + 44 * s;
    dl->AddText(huge, huge->FontSize, ImVec2((ds.x - ts.x) / 2, ty), IM_COL32(255, 255, 255, 255), c.title.c_str());
    const ImVec2 ss = TextSize(body, c.subtitle);
    dl->AddText(body, body->FontSize, ImVec2((ds.x - ss.x) / 2, ty + ts.y + 8 * s), IM_COL32(230, 226, 250, 200), c.subtitle.c_str());
    // Dots.
    const float dy = ty + ts.y + ss.y + 34 * s;
    const float dw = 9 * s, dsp = 18 * s;
    const float total = (cols_.size() - 1) * dsp + dw * 3;
    float dx = (ds.x - total) / 2;
    for (int i = 0; i < static_cast<int>(cols_.size()); i++) {
        if (i == centered_) {
            dl->AddRectFilled(ImVec2(dx, dy), ImVec2(dx + dw * 3, dy + dw), IM_COL32(255, 255, 255, 255), dw);
            dx += dw * 3 + (dsp - dw);
        } else {
            dl->AddCircleFilled(ImVec2(dx + dw / 2, dy + dw / 2), dw / 2, IM_COL32(255, 255, 255, 90));
            dx += dsp;
        }
    }
    std::vector<Hint> hints = {{"A", kHintA, "Abrir", [this] { HandleKey(Key::kA); }}};
    if (cols_[0].key == "recent") hints.push_back({"Y", kHintY, "Continuar", [this] { HandleKey(Key::kY); }});
    hints.push_back({"X", kHintX, "Buscar", [this] { HandleKey(Key::kX); }});
    hints.push_back({"START", 0, "Menu", [this] { HandleKey(Key::kStart); }});
    DrawHints(hints);
}

void LibraryUi::DrawEmpty() {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = win_.scale();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // The consoles' photos in a row, as a welcome.
    const float ph = std::min(ds.y * 0.16f, 130 * s);
    const int n = static_cast<int>(photos_.size());
    const float pw = std::min(ds.x * 0.9f / n, ph * 1.3f);
    for (int i = 0; i < n; i++) {
        if (!photos_[static_cast<size_t>(i)].valid()) continue;
        const float x = ds.x / 2 + (i - (n - 1) / 2.0f) * pw;
        const float y = ds.y * 0.27f + std::sin(static_cast<float>(Now()) * 1.3f + i * 0.7f) * 6 * s;
        ImVec2 p0, p1;
        Fit(photos_[static_cast<size_t>(i)], ImVec2(x - pw * 0.45f, y - ph / 2), ImVec2(x + pw * 0.45f, y + ph / 2), &p0, &p1);
        dl->AddImage(photos_[static_cast<size_t>(i)].im(), p0, p1);
    }
    ImGui::SetNextWindowPos(ImVec2(ds.x / 2, ds.y * 0.62f), 0, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(ds.x - 40, 680 * s), 0));
    ImGui::Begin("##empty", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(win_.fonts().title);
    ImGui::TextUnformatted(scan_running_ ? "Procurando jogos…" : "Bem-vindo ao Cartucho");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("Escolha a pasta onde estão os seus jogos: Game Boy, GBA, DS, 3DS, NES, Super Nintendo, Mega Drive, "
                       "Master System, Game Gear e PlayStation. As subpastas também contam.");
    ImGui::Spacing();
    ImGui::PushFont(win_.fonts().bold);
    if (ImGui::Button("   Adicionar pasta de jogos   ") && modal_ == Modal::kNone) AddFolder();
    ImGui::PopFont();
    ImGui::SameLine();
    if (ImGui::Button("  Configurações  ") && modal_ == Modal::kNone) { modal_ = Modal::kSettings; settings_ui_.Open(); }
    ImGui::Spacing();
    ImGui::TextDisabled("Seus saves do celular? Copie as pastas saves e states para %s", Paths::Get().data.c_str());
    ImGui::End();
}

// --- List ---------------------------------------------------------------------------------

void LibraryUi::DrawList() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = win_.scale();
    ImFont* body = win_.fonts().body;
    ImFont* bold = win_.fonts().bold;
    ImFont* small = win_.fonts().small;
    // Header: console photo, badge, title.
    float hx = 34 * s;
    const float hy = 24 * s;
    if (current_.sys && photos_[static_cast<size_t>(current_.sys->id)].valid()) {
        ImVec2 p0, p1;
        Fit(photos_[static_cast<size_t>(current_.sys->id)], ImVec2(hx, hy - 8 * s), ImVec2(hx + 64 * s, hy + 46 * s), &p0, &p1);
        dl->AddImage(photos_[static_cast<size_t>(current_.sys->id)].im(), p0, p1);
        hx = p1.x + 16 * s;
    }
    const std::string badge = current_.sys ? current_.sys->short_name : current_.key == "fav" ? "★" : current_.key == "recent" ? "▶" : current_.key == "search" ? "Busca" : "Todos";
    const ImVec2 bs = TextSize(bold, badge);
    dl->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + bs.x + 24 * s, hy + bs.y + 14 * s), Rgb(current_.color), 10 * s);
    dl->AddText(bold, bold->FontSize, ImVec2(hx + 12 * s, hy + 7 * s), IM_COL32(255, 255, 255, 255), badge.c_str());
    const std::string head = current_.title + "  ·  " + std::to_string(current_.games.size()) + (current_.games.size() == 1 ? " jogo" : " jogos");
    dl->AddText(body, body->FontSize * 1.1f, ImVec2(hx + bs.x + 40 * s, hy + 6 * s), IM_COL32(235, 231, 250, 235), head.c_str());
    // Tabs.
    float tx = 34 * s;
    const float ty = 92 * s;
    for (int i = 0; i < static_cast<int>(tabs_.size()); i++) {
        const std::string& name = tabs_[static_cast<size_t>(i)].first;
        const ImVec2 ts = TextSize(body, name);
        const ImVec2 t0(tx, ty), t1(tx + ts.x + 32 * s, ty + ts.y + 16 * s);
        const bool on = i == tab_;
        dl->AddRectFilled(t0, t1, on ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 28), (t1.y - t0.y) / 2);
        dl->AddText(body, body->FontSize, ImVec2(tx + 16 * s, ty + 8 * s), on ? IM_COL32(24, 22, 36, 255) : IM_COL32(230, 226, 250, 230), name.c_str());
        ImGui::SetCursorScreenPos(t0);
        ImGui::PushID(1000 + i);
        if (ImGui::InvisibleButton("##tab", ImVec2(t1.x - t0.x, t1.y - t0.y)) && modal_ == Modal::kNone) { tab_ = i; ApplyTab(); sounds_.Play(Sounds::kMove); }
        ImGui::PopID();
        tx = t1.x + 10 * s;
    }
    // The list (left).
    const float lx0 = 26 * s, lx1 = ds.x * 0.42f, ly0 = 150 * s, ly1 = ds.y - 74 * s;
    const float row = 54 * s;
    const float visible = ly1 - ly0;
    const float target = std::clamp(selected_ * row - visible / 2 + row / 2, 0.0f, std::max(0.0f, shown_.size() * row - visible));
    list_scroll_ += (target - list_scroll_) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14);
    dl->PushClipRect(ImVec2(lx0 - 10, ly0), ImVec2(lx1 + 10, ly1), true);
    const bool by_time = current_.key == "recent" || tabs_[static_cast<size_t>(tab_)].first == "Recentes";
    for (int i = 0; i < static_cast<int>(shown_.size()); i++) {
        const float y = ly0 + i * row - list_scroll_;
        if (y < ly0 - row || y > ly1) continue;
        const Game& g = shown_[static_cast<size_t>(i)];
        const bool on = i == selected_;
        if (on) {
            dl->AddRectFilled(ImVec2(lx0 + 4, y + 9), ImVec2(lx1 + 4, y + row + 3), IM_COL32(0, 0, 0, 70), 16 * s);
            dl->AddImageRounded(grad_.im(), ImVec2(lx0, y + 2), ImVec2(lx1, y + row - 2), ImVec2(0, 0), ImVec2(1, 1),
                                Rgb(Blend(current_.color, 0xFFFFFF, 0.1f)), 16 * s);
        }
        std::string right;
        const int64_t time = Library::Get().PlayTime(g);
        if (by_time) right = PlayStats::FormatAgo(Library::Get().LastPlayed(g));
        else if (time > 0) right = PlayStats::FormatTime(time);
        else if (const Facts* fa = Library::Get().FactsFor(g)) right = fa->year;
        if (!current_.sys) right = right.empty() ? std::string(Info(g.sys).short_name) : right + "  " + Info(g.sys).short_name;
        if (Library::Get().IsFavorite(g)) right = "★ " + right;
        const ImVec2 rs = TextSize(small, right);
        ImFont* f = on ? bold : body;
        dl->PushClipRect(ImVec2(lx0, y), ImVec2(lx1 - rs.x - 30 * s, y + row), true);
        dl->AddText(f, f->FontSize, ImVec2(lx0 + 20 * s, y + (row - f->FontSize) / 2), on ? IM_COL32(255, 255, 255, 255) : IM_COL32(225, 221, 240, 215), g.title.c_str());
        dl->PopClipRect();
        dl->AddText(small, small->FontSize, ImVec2(lx1 - rs.x - 18 * s, y + (row - rs.y) / 2), on ? IM_COL32(255, 255, 255, 230) : IM_COL32(180, 176, 200, 200), right.c_str());
        if (y >= ly0 && y + row <= ly1 + row / 2) {
            ImGui::SetCursorScreenPos(ImVec2(lx0, y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##row", ImVec2(lx1 - lx0, row)) && modal_ == Modal::kNone) {
                if (i == selected_) Play(g);
                else SelectGame(i);
            }
            ImGui::PopID();
        }
    }
    dl->PopClipRect();
    if (ImGui::GetIO().MouseWheel != 0 && modal_ == Modal::kNone && ImGui::GetIO().MousePos.x < lx1)
        SelectGame(selected_ - static_cast<int>(ImGui::GetIO().MouseWheel * 3));
    if (shown_.empty())
        dl->AddText(body, body->FontSize, ImVec2(lx0 + 20 * s, ly0 + 20 * s), IM_COL32(190, 186, 210, 220), "Nenhum jogo aqui");
    if (!in_list_) return;   // a game was played and the collection is gone
    DrawPanel(ImVec2(lx1 + 30 * s, 92 * s), ImVec2(ds.x - 34 * s, ds.y - 82 * s));
    DrawHints({{"A", kHintA, "Jogar", [this] { HandleKey(Key::kA); }},
               {"B", kHintB, "Voltar", [this] { HandleKey(Key::kB); }},
               {"Y", kHintY, "Favorito", [this] { HandleKey(Key::kY); }},
               {"X", kHintX, "Opções", [this] { HandleKey(Key::kX); }},
               {"L R", 0, "Abas", nullptr}});
}

void LibraryUi::DrawPanel(ImVec2 p0, ImVec2 p1) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float s = win_.scale();
    dl->AddRectFilled(p0, p1, IM_COL32(16, 14, 26, 200), 24 * s);
    if (shown_.empty()) return;
    const Game g = shown_[static_cast<size_t>(selected_)];
    ImFont* body = win_.fonts().body;
    ImFont* bold = win_.fonts().bold;
    ImFont* small = win_.fonts().small;
    ImFont* title = win_.fonts().title;
    const float pad = 24 * s;
    const float pic = std::min((p1.x - p0.x) * 0.44f, (p1.y - p0.y) * 0.56f);
    const ImVec2 c0(p0.x + pad, p0.y + pad), c1(c0.x + pic, c0.y + pic);
    dl->AddRectFilled(c0, c1, IM_COL32(6, 5, 10, 255), 16 * s);
    // Cover, then the title screen and screenshots taking turns.
    std::vector<std::string> pics;
    const bool media = Settings::Get().Bool("download_covers", true);
    const std::string cover = !g.cover.empty() ? g.cover : media ? images_->Cover(g) : "";
    if (!cover.empty()) pics.push_back(cover);
    if (media && Settings::Get().Bool("game_media", true) && Now() - selected_at_ > 0.4)
        for (auto& p : images_->Pictures(g)) pics.push_back(p);
    const Texture* show = nullptr;
    float fade = 1;
    if (!pics.empty()) {
        const double since = Now() - selected_at_;
        const size_t idx = static_cast<size_t>(since / 4.0) % pics.size();
        const Texture& tex = images_->Get(pics[idx]);
        if (tex.valid()) {
            show = &tex;
            if (pics.size() > 1) fade = static_cast<float>(std::min(1.0, std::fmod(since, 4.0) / 0.35));
        } else {
            const Texture& first = images_->Get(pics[0]);
            if (first.valid()) show = &first;
        }
    }
    if (show) {
        ImVec2 q0, q1;
        Fit(*show, ImVec2(c0.x + 8, c0.y + 8), ImVec2(c1.x - 8, c1.y - 8), &q0, &q1);
        dl->AddImageRounded(show->im(), q0, q1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, static_cast<int>(255 * fade)), 8 * s);
    } else {
        const Texture& photo = photos_[static_cast<size_t>(g.sys)];
        if (photo.valid()) {
            ImVec2 q0, q1;
            Fit(photo, ImVec2(c0.x + pic * 0.14f, c0.y + pic * 0.14f), ImVec2(c1.x - pic * 0.14f, c1.y - pic * 0.14f), &q0, &q1);
            dl->AddImage(photo.im(), q0, q1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 200));
        }
    }
    // Text beside the picture.
    const float tx = c1.x + pad;
    const float tw = p1.x - pad - tx;
    float y = c0.y;
    dl->AddText(title, title->FontSize, ImVec2(tx, y), IM_COL32(255, 255, 255, 255), g.title.c_str(), nullptr, tw);
    y += TextSize(title, g.title, tw).y + 10 * s;
    std::string meta;
    if (const Facts* f = Library::Get().FactsFor(g)) {
        std::string l1 = f->year;
        if (!f->genre.empty()) l1 += (l1.empty() ? "" : " · ") + f->genre;
        if (!f->players.empty() && f->players != "0")
            l1 += (l1.empty() ? "" : " · ") + (f->players == "1" ? std::string("1 jogador") : "1–" + f->players + " jogadores");
        std::string l2 = f->developer;
        if (!f->publisher.empty() && f->publisher != f->developer) l2 += (l2.empty() ? "" : " / ") + f->publisher;
        meta = l1 + (l2.empty() ? "" : "\n" + l2);
    }
    if (meta.empty()) meta = Info(g.sys).full_name;
    dl->AddText(body, body->FontSize, ImVec2(tx, y), IM_COL32(190, 186, 210, 255), meta.c_str(), nullptr, tw);
    y += TextSize(body, meta, tw).y + 16 * s;
    // Chips: play time, saves, favourite.
    std::vector<std::string> chips;
    const int64_t time = Library::Get().PlayTime(g);
    if (time > 0) chips.push_back("Jogado " + PlayStats::FormatTime(time));
    int slots = 0;
    const std::string state_dir = JoinPath(JoinPath(Paths::Get().data, "states"), Info(g.sys).key);
    for (int i = 1; i <= 6; i++) if (FileExists(JoinPath(state_dir, g.save_name + ".state" + std::to_string(i)))) slots++;
    if (slots) chips.push_back(std::to_string(slots) + (slots == 1 ? " estado salvo" : " estados salvos"));
    if (FileExists(JoinPath(JoinPath(JoinPath(Paths::Get().data, "saves"), Info(g.sys).key), g.save_name + ".srm")) ||
        FileExists(JoinPath(JoinPath(JoinPath(Paths::Get().data, "saves"), Info(g.sys).key), g.save_name + ".sav")))
        chips.push_back("Save do jogo");
    if (Library::Get().IsFavorite(g)) chips.push_back("★ Favorito");
    float cx = tx;
    for (auto& c : chips) {
        const ImVec2 cs = TextSize(small, c);
        if (cx + cs.x + 22 * s > p1.x - pad) { cx = tx; y += cs.y + 18 * s; }
        dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + cs.x + 22 * s, y + cs.y + 12 * s), IM_COL32(255, 255, 255, 26), 9 * s);
        dl->AddText(small, small->FontSize, ImVec2(cx + 11 * s, y + 6 * s), IM_COL32(235, 231, 250, 235), c.c_str());
        cx += cs.x + 32 * s;
    }
    if (!chips.empty()) y += small->FontSize + 30 * s;
    // Play button.
    const bool resume = Settings::Get().Bool("resume", true) && FileExists(PlayStats::ResumeState(state_dir, g.save_name));
    const ImVec2 b0(tx, std::max(y, c1.y - 56 * s)), b1(b0.x + std::min(tw, 220 * s), b0.y + 56 * s);
    dl->AddRectFilled(ImVec2(b0.x + 3, b0.y + 7), ImVec2(b1.x + 3, b1.y + 7), IM_COL32(0, 0, 0, 70), 28 * s);
    dl->AddImageRounded(grad_.im(), b0, b1, ImVec2(0, 0), ImVec2(1, 1), Rgb(Blend(current_.color, 0xFFFFFF, 0.1f)), 28 * s);
    const std::string label = resume ? "▶  Continuar" : "▶  Jogar";
    const ImVec2 ls = TextSize(bold, label);
    dl->AddText(bold, bold->FontSize, ImVec2((b0.x + b1.x - ls.x) / 2, (b0.y + b1.y - ls.y) / 2), IM_COL32(255, 255, 255, 255), label.c_str());
    ImGui::SetCursorScreenPos(b0);
    if (ImGui::InvisibleButton("##play", ImVec2(b1.x - b0.x, b1.y - b0.y)) && modal_ == Modal::kNone) { Play(g); return; }
    // Pictures of the continue point and the save slots.
    std::vector<std::pair<std::string, std::string>> shots;
    const std::string thumb = PlayStats::ThumbPath(g.path);
    if (FileExists(thumb)) shots.push_back({thumb, "Continuar · " + PlayStats::FormatAgo(Library::Get().LastPlayed(g))});
    for (int i = 1; i <= 6 && shots.size() < 4; i++) {
        const std::string f = JoinPath(state_dir, g.save_name + ".state" + std::to_string(i) + ".png");
        if (FileExists(f)) shots.push_back({f, "Slot " + std::to_string(i)});
    }
    const float sy0 = c1.y + pad;
    if (!shots.empty() && p1.y - pad - sy0 > 60 * s) {
        dl->AddText(small, small->FontSize, ImVec2(p0.x + pad, sy0), IM_COL32(190, 186, 210, 255), "SEUS PONTOS SALVOS");
        const float top = sy0 + small->FontSize + 10 * s;
        const float sw = (p1.x - p0.x - pad * 2 - 3 * 14 * s) / 4;
        const float sh = std::min(p1.y - pad - top, sw * 0.75f);
        for (size_t i = 0; i < shots.size(); i++) {
            const ImVec2 s0(p0.x + pad + i * (sw + 14 * s), top), s1(s0.x + sw, top + sh);
            dl->AddRectFilled(s0, s1, IM_COL32(6, 5, 10, 255), 10 * s);
            const Texture& tex = images_->Get(shots[i].first, 400);
            if (tex.valid()) {
                ImVec2 q0, q1;
                Fit(tex, s0, s1, &q0, &q1);
                dl->AddImageRounded(tex.im(), q0, q1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), 8 * s);
            }
            const ImVec2 l2 = TextSize(small, shots[i].second);
            dl->AddRectFilled(ImVec2(s0.x + 8 * s, s1.y - l2.y - 18 * s), ImVec2(s0.x + l2.x + 24 * s, s1.y - 6 * s), IM_COL32(0, 0, 0, 170), 6 * s);
            dl->AddText(small, small->FontSize, ImVec2(s0.x + 16 * s, s1.y - l2.y - 12 * s), IM_COL32(255, 255, 255, 255), shots[i].second.c_str());
        }
    }
}

// --- Modals -----------------------------------------------------------------------------

void LibraryUi::DrawModals() {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = win_.scale();
    if (modal_ == Modal::kNone) return;
    if (modal_ == Modal::kSettings) {
        if (!settings_ui_.Draw(win_, sounds_)) {
            modal_ = Modal::kNone;
            sounds_.SetEffects(Settings::Get().Bool("ui_sounds", true));
            sounds_.SetMusic(Settings::Get().Bool("ui_music", true));
            if (settings_ui_.rescan_requested()) StartScan();
        }
        return;
    }
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(5, 5, 12, 130));
    struct Item { std::string label; std::function<void()> act; };
    std::vector<Item> items;
    std::string title;
    const bool have_game = selected_ >= 0 && selected_ < static_cast<int>(shown_.size());
    const Game game = have_game ? shown_[static_cast<size_t>(selected_)] : Game{};
    if (modal_ == Modal::kMainMenu) {
        title = "Menu";
        items = {
            {"Adicionar pasta de jogos", [this] { modal_ = Modal::kNone; AddFolder(); }},
            {"Procurar jogos novos", [this] { modal_ = Modal::kNone; StartScan(); Notice("Procurando jogos…"); }},
            {"Configurações", [this] { modal_ = Modal::kSettings; settings_ui_.Open(); }},
            {win_.fullscreen() ? "Sair da tela cheia" : "Tela cheia", [this] {
                 win_.ToggleFullscreen();
                 Settings::Get().Set("fullscreen_library", win_.fullscreen());
                 modal_ = Modal::kNone;
             }},
            {"Abrir a pasta de dados (saves)", [this] { OpenUrl(Paths::Get().data); modal_ = Modal::kNone; }},
            {"Sair do Cartucho", [this] { quit_ = true; }},
        };
    } else if (modal_ == Modal::kGameMenu && have_game) {
        title = game.title;
        items = {
            {"Jogar", [this, game] { modal_ = Modal::kNone; Play(game); }},
            {"Começar do zero (sem continuar)", [this, game] { modal_ = Modal::kNone; Play(game, false); }},
            {Library::Get().IsFavorite(game) ? "Tirar dos favoritos" : "Favoritar", [this] { modal_ = Modal::kNone; ToggleFavorite(); }},
            {"Detalhes do jogo", [this] { modal_ = Modal::kDetails; }},
            {"Abrir a pasta do jogo", [this, game] { OpenUrl(ParentDir(game.path)); modal_ = Modal::kNone; }},
        };
    } else if (modal_ == Modal::kDetails && have_game) {
        ImGui::SetNextWindowPos(ImVec2(ds.x / 2, ds.y / 2), 0, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(ds.x - 40, 760 * s), 0));
        ImGui::Begin("##details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PushFont(win_.fonts().title);
        ImGui::TextWrapped("%s", game.title.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
        if (const Facts* f = Library::Get().FactsFor(game)) {
            if (!f->year.empty()) ImGui::Text("Ano: %s", f->year.c_str());
            if (!f->genre.empty()) ImGui::Text("Gênero: %s", f->genre.c_str());
            if (!f->developer.empty()) ImGui::Text("Desenvolvedora: %s", f->developer.c_str());
            if (!f->publisher.empty()) ImGui::Text("Distribuidora: %s", f->publisher.c_str());
            if (!f->players.empty()) ImGui::Text("Jogadores: %s", f->players.c_str());
        }
        ImGui::Text("Console: %s", Info(game.sys).full_name);
        ImGui::TextWrapped("Arquivo: %s", game.path.c_str());
        ImGui::Text("Tamanho: %.1f MB", game.size / 1048576.0);
        const int64_t time = Library::Get().PlayTime(game);
        if (time > 0) ImGui::Text("Tempo jogado: %s", PlayStats::FormatTime(time).c_str());
        ImGui::TextDisabled("Nome dos saves: %s", game.save_name.c_str());
        ImGui::Spacing();
        if (ImGui::Button("  Fechar  ") || modal_activate_) modal_ = Modal::kNone;
        modal_activate_ = false;
        ImGui::End();
        return;
    } else if (modal_ == Modal::kSearch) {
        ImGui::SetNextWindowPos(ImVec2(ds.x / 2, ds.y * 0.3f), 0, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(ds.x - 40, 720 * s), 0));
        ImGui::Begin("##search", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PushFont(win_.fonts().title);
        ImGui::TextUnformatted("Buscar jogo");
        ImGui::PopFont();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        const bool enter = ImGui::InputText("##q", search_, sizeof(search_), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Digite parte do nome e aperte Enter. Esc fecha.");
        if (enter && search_[0]) {
            Collection c{"search", std::string("“") + search_ + "”", 0x3B82F6, {}, ""};
            const std::string q = Lower(search_);
            for (auto& x : Library::Get().games())
                if (Lower(x.title).find(q) != std::string::npos || Lower(x.file_name).find(q) != std::string::npos) c.games.push_back(x);
            modal_ = Modal::kNone;
            if (c.games.empty()) {
                Notice("Nenhum jogo com “" + std::string(search_) + "”");
            } else {
                const int keep = centered_;
                cols_.push_back(c);
                centered_ = static_cast<int>(cols_.size()) - 1;
                OpenCollection();
                cols_.pop_back();
                centered_ = keep;
            }
        }
        ImGui::End();
        return;
    }
    if (items.empty()) { modal_ = Modal::kNone; return; }
    const int n = static_cast<int>(items.size());
    modal_sel_ = ((modal_sel_ + modal_move_) % n + n) % n;
    if (modal_move_) sounds_.Play(Sounds::kMove);
    modal_move_ = 0;
    int run = modal_activate_ ? modal_sel_ : -1;
    modal_activate_ = false;
    ImGui::SetNextWindowPos(ImVec2(ds.x / 2, ds.y / 2), 0, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(ds.x - 40, 560 * s), 0));
    ImGui::Begin("##modal", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(win_.fonts().title);
    ImGui::TextWrapped("%s", title.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    for (int i = 0; i < n; i++) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 50 * s;
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##mi", ImVec2(w, h))) run = i;
        if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) modal_sel_ = i;
        ImGui::PopID();
        const bool on = i == modal_sel_;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (on) dl->AddImageRounded(grad_.im(), p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1), Rgb(0x8B6CFF), 12 * s);
        ImFont* f = on ? win_.fonts().bold : win_.fonts().body;
        dl->AddText(f, f->FontSize, ImVec2(p.x + 18 * s, p.y + (h - f->FontSize) / 2), IM_COL32(255, 255, 255, on ? 255 : 220), items[static_cast<size_t>(i)].label.c_str());
    }
    ImGui::Spacing();
    ImGui::TextDisabled("B / Esc: voltar");
    ImGui::End();
    if (run >= 0) {
        sounds_.Play(Sounds::kSelect);
        items[static_cast<size_t>(run)].act();
    }
}

// --- Main loop ----------------------------------------------------------------------------

int LibraryUi::Run() {
    OpenLog("cartucho.log");
    Settings::Get().Load();
    Library::Get().Load();
    std::string error;
    if (!win_.Open("Cartucho", Settings::Get().Bool("fullscreen_library", false), &error)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cartucho", error.c_str(), nullptr);
        return 3;
    }
    SDL_SetWindowMinimumSize(win_.sdl(), 960, 600);
    if (!win_.fullscreen() && !std::getenv("CARTUCHO_SHOT")) SDL_MaximizeWindow(win_.sdl());
    images_ = std::make_unique<Images>();
    grad_ = MakeGradient();
    glow_ = MakeGlow();
    for (const auto& sys : Systems())
        photos_.push_back(LoadTexture(JoinPath(JoinPath(Paths::Get().assets, "photos"), std::string("photo_") + sys.photo + ".png")));
    sounds_.Open();
    sounds_.SetEffects(Settings::Get().Bool("ui_sounds", true));
    const std::string music = Settings::Get().Str("ui_music_file", "");
    if (!music.empty()) sounds_.LoadMusic(music);
    sounds_.SetMusic(Settings::Get().Bool("ui_music", true));
    Refresh();
    if (!Library::Get().folders().empty()) StartScan();
    if (Settings::Get().Bool("auto_update", true) && Updater::CurrentBuild() > 0 && !std::getenv("CARTUCHO_SHOT"))
        Background([] {
            const auto u = Updater::Check();
            return u.newer ? "Versão nova do Cartucho disponível: Configurações → Sobre → Atualizar agora" : std::string();
        });
    bool was_scanning = scan_running_;
    TestHook test;
    const double t0 = Now();
    ImGuiIO& io = ImGui::GetIO();
    while (!quit_) {
        {
            std::lock_guard<std::mutex> lock(bg_mutex_);
            if (!bg_notice_.empty()) {
                Notice(bg_notice_, 6);
                bg_notice_.clear();
            }
        }
        if (settings_ui_.quit_requested()) quit_ = true;
        // ImGui's own keyboard navigation only in the settings (elsewhere
        // the arrows drive the console-style screens).
        if (modal_ == Modal::kSettings) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        test.BeforeFrame();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (modal_ == Modal::kSettings && settings_ui_.HandleEvent(e)) continue;
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) quit_ = true;
            Key k = Key::kNone;
            const bool typing = io.WantTextInput;
            if (e.type == SDL_KEYDOWN && !typing) {
                switch (e.key.keysym.sym) {
                    case SDLK_LEFT: k = Key::kLeft; break;
                    case SDLK_RIGHT: k = Key::kRight; break;
                    case SDLK_UP: k = Key::kUp; break;
                    case SDLK_DOWN: k = Key::kDown; break;
                    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_z: k = Key::kA; break;
                    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_x: k = Key::kB; break;
                    case SDLK_f: case SDLK_s: k = Key::kY; break;
                    case SDLK_TAB: case SDLK_a: k = Key::kX; break;
                    case SDLK_PAGEUP: case SDLK_q: k = Key::kL; break;
                    case SDLK_PAGEDOWN: case SDLK_w: k = Key::kR; break;
                    case SDLK_F1: case SDLK_m: k = Key::kStart; break;
                    case SDLK_F11:
                        win_.ToggleFullscreen();
                        Settings::Get().Set("fullscreen_library", win_.fullscreen());
                        break;
                    default: break;
                }
                if (modal_ == Modal::kSettings && k != Key::kB) k = Key::kNone;   // the settings use ImGui's navigation
            } else if (e.type == SDL_KEYDOWN && typing && e.key.keysym.sym == SDLK_ESCAPE) {
                k = Key::kB;
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: k = Key::kLeft; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: k = Key::kRight; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP: k = Key::kUp; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: k = Key::kDown; break;
                    case SDL_CONTROLLER_BUTTON_A: k = Key::kA; break;      // bottom
                    case SDL_CONTROLLER_BUTTON_B: k = Key::kB; break;      // right
                    case SDL_CONTROLLER_BUTTON_Y: k = Key::kY; break;      // top
                    case SDL_CONTROLLER_BUTTON_X: k = Key::kX; break;      // left
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: k = Key::kL; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: k = Key::kR; break;
                    case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_GUIDE: k = Key::kStart; break;
                    default: break;
                }
                if (modal_ == Modal::kSettings && k != Key::kB) k = Key::kNone;
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                SDL_GameControllerOpen(e.cdevice.which);
            }
            if (k != Key::kNone) HandleKey(k);
        }
        if (was_scanning && !scan_running_) Refresh();
        was_scanning = scan_running_;
        images_->Update();
        int w, h;
        win_.Size(&w, &h);
        const float t = static_cast<float>(Now() - t0);
        glViewport(0, 0, w, h);
        backdrop_.Draw(w, h, backdrop_.Current(t), Settings::Get().Bool("ui_animated_bg", true) ? t : 0.0f);
        win_.BeginFrame();
        // One invisible full-screen window holds the screens' clickable areas.
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##screen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                              ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        if (in_list_) DrawList();
        else DrawHome();
        ImGui::End();
        // Its own window, after the full-screen one, so it gets the clicks.
        if (!in_list_ && cols_.empty()) DrawEmpty();
        DrawTopBar();
        DrawModals();
        if (Now() < notice_until_) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 ds = io.DisplaySize;
            const float s = win_.scale();
            const ImVec2 ts = TextSize(win_.fonts().body, notice_);
            const ImVec2 p((ds.x - ts.x) / 2, ds.y - 130 * s);
            dl->AddRectFilled(ImVec2(p.x - 22 * s, p.y - 14 * s), ImVec2(p.x + ts.x + 22 * s, p.y + ts.y + 14 * s), IM_COL32(15, 13, 26, 235), 24 * s);
            dl->AddText(win_.fonts().body, win_.fonts().body->FontSize, p, IM_COL32(255, 255, 255, 255), notice_.c_str());
        }
        win_.Render();
        if (test.AfterDraw(w, h)) quit_ = true;
        win_.Swap();
    }
    if (scan_thread_.joinable()) scan_thread_.join();
    if (bg_.joinable()) bg_.join();
    images_.reset();
    for (auto& p : photos_) FreeTexture(&p);
    FreeTexture(&grad_);
    FreeTexture(&glow_);
    sounds_.Close();
    win_.Close();
    return 0;
}

}  // namespace cartucho
