#include "game_ui.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>

#include <functional>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

#include "gfx.h"
#include "paths.h"
#include "platform.h"
#include "playstats.h"
#include "session.h"
#include "settings.h"
#include "systems.h"
#include "window.h"

namespace cartucho {

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

ImU32 Rgb(uint32_t rgb, int a = 255) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a);
}
ImU32 Mix(uint32_t a, uint32_t b, float t, int alpha = 255) {
    auto ch = [&](int s) {
        return static_cast<int>(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t);
    };
    return IM_COL32(ch(16), ch(8), ch(0), alpha);
}

const char* kFilters[] = {"Nítido", "Suave", "Nítido suave", "Tela LCD", "Linhas de TV", "Bordas arredondadas 2x",
                          "Bordas arredondadas 3x", "TV de tubo (CRT)", "TV de tubo curva", "TV antiga (cabo composto)"};
const char* kBezels[] = {"Sem moldura", "Automática (do console)", "Portátil", "TV de tubo", "Fliperama"};
const char* kBezelKeys[] = {"off", "auto", "handheld", "tv", "arcade"};

}  // namespace

struct GameUi::Impl {
    Texture photo;                 // the console, for the frame's side
    Texture slot_pics[7];          // 0 = quick slot, 1..6
    bool slot_used[7] = {};
    std::string slot_label[7];
    double started = Now();
    int64_t play_ms_counted = 0;
};

GameUi::GameUi(GameContext& ctx) : ctx_(ctx), impl_(std::make_unique<Impl>()) {
    impl_->photo = LoadTexture(JoinPath(JoinPath(Paths::Get().assets, "photos"),
                                        std::string("photo_") + ctx_.system->photo + ".png"));
}

GameUi::~GameUi() {
    FreeTexture(&impl_->photo);
    for (auto& t : impl_->slot_pics)
        FreeTexture(&t);
}

void GameUi::ShowFatal(Window& win, const std::string& message) {
    bool done = false;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
                done = true;
        }
        win.BeginFrame();
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(11, 10, 20, 255));
        ImGui::SetNextWindowPos(ImVec2(ds.x / 2, ds.y / 2), 0, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(ds.x - 40, 720.0f * win.scale()), 0));
        ImGui::Begin("##fatal", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::PushFont(win.fonts().bold);
        ImGui::TextUnformatted("Não foi possível abrir o jogo");
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::Spacing();
        if (ImGui::Button("  OK  ") || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))
            done = true;
        ImGui::End();
        win.EndFrame();
    }
}

void GameUi::ApplyDisplay() {
    auto& p = ctx_.session->presenter();
    const int filter = std::atoi(ctx_.settings->Str("filter", "2").c_str());
    if (filter >= 0 && filter < static_cast<int>(Filter::kCount))
        p.set_filter(static_cast<Filter>(filter));
    p.set_integer_scale(Settings::Get().Bool("integer_scale", false));
    p.set_vertical_align(0);
    p.set_bottom_reserve(0.0f);
}

std::string GameUi::StatePath(int slot) const {
    return JoinPath(ctx_.state_dir, ctx_.args.save_name + ".state" + std::to_string(slot));
}

void GameUi::ResumeIfAny() {
    if (!Settings::Get().Bool("resume", true) || !PlayStats::CanResume(ctx_.system->key))
        return;
    const std::string f = PlayStats::ResumeState(ctx_.state_dir, ctx_.args.save_name);
    if (FileSize(f) == 0)
        return;
    std::string err;
    if (ctx_.session->host().LoadState(f, &err))
        Message("Continuando de onde você parou");
}

void GameUi::OnQuit() {
    // Play time and the continue point (state and picture).
    PlayStats::AddPlayTime(ctx_.args.rom, static_cast<int64_t>((Now() - impl_->started) * 1000));
    if (Settings::Get().Bool("resume", true) && PlayStats::CanResume(ctx_.system->key) && !ctx_.session->net_guest()) {
        std::string err;
        if (ctx_.session->host().SaveState(PlayStats::ResumeState(ctx_.state_dir, ctx_.args.save_name), &err)) {
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (ctx_.session->Screenshot(&rgba, &w, &h))
                WritePng(PlayStats::ThumbPath(ctx_.args.rom), rgba.data(), w, h, 480);
        }
    }
}

void GameUi::Message(const std::string& text, double seconds) {
    message_ = text;
    message_until_ = Now() + seconds;
}

bool GameUi::HandleEvent(const SDL_Event& e) {
    if (!menu_open())
        return false;
    // Back / B / Escape: one level up.
    const bool back = (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_BACKSPACE)) ||
                      (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_B) ||
                      (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE);
    if (back && Now() - opened_at_ > 0.15) {
        if (menu_ == Menu::kMain || e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE)
            CloseMenu();
        else
            OpenMenu(Menu::kMain);
        return true;
    }
    // Arrows / D-pad / Enter / A drive the menu.
    if (e.type == SDL_KEYDOWN) {
        switch (e.key.keysym.sym) {
            case SDLK_UP: move_ = -1; break;
            case SDLK_DOWN: move_ = 1; break;
            case SDLK_LEFT: move_x_ = -1; break;
            case SDLK_RIGHT: move_x_ = 1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate_ = true; break;
            default: break;
        }
    } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: move_ = -1; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move_ = 1; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: move_x_ = -1; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move_x_ = 1; break;
            case SDL_CONTROLLER_BUTTON_A: activate_ = true; break;
            default: break;
        }
    }
    // Everything else belongs to the menu while it is open.
    return e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_CONTROLLERBUTTONDOWN ||
           e.type == SDL_CONTROLLERBUTTONUP;
}

void GameUi::OnAction(Action a, bool pressed) {
    switch (a) {
        case Action::kMenu:
            if (pressed) OpenMenu(Menu::kMain);
            break;
        case Action::kFastForward:
            if (pressed) SetFastForward(!fast_);
            break;
        case Action::kRewind:
            if (ctx_.session->net().active()) {
                if (pressed) Message("Voltar no tempo fica desligado no jogo online");
                break;
            }
            rewinding_ = pressed;
            ctx_.session->set_rewinding(pressed);
            if (pressed) Message("« Voltando no tempo", 1.0);
            break;
        case Action::kQuickSave:
            if (pressed && SaveSlot(0)) Message("Salvo rápido");
            break;
        case Action::kQuickLoad:
            if (pressed) {
                if (ctx_.session->net().active()) Message("Carregar estados fica desligado no jogo online");
                else if (LoadSlot(0)) Message("Carregado o salvo rápido");
            }
            break;
        case Action::kFullscreen:
            if (pressed) {
                ctx_.window->ToggleFullscreen();
                Settings::Get().Set("fullscreen", ctx_.window->fullscreen());
            }
            break;
        case Action::kScreenshot:
            if (pressed) TakeScreenshot();
            break;
        default:
            break;
    }
}

void GameUi::SetFastForward(bool on) {
    if (on && ctx_.session->net().active()) {
        Message("Acelerar fica desligado no jogo online");
        return;
    }
    fast_ = on;
    const double speed = std::atof(ctx_.settings->Str("ff_speed", "3").c_str());
    ctx_.session->set_speed(on ? (speed > 1 ? speed : 3.0) : 1.0);
}

bool GameUi::SaveSlot(int slot) {
    std::string err;
    const std::string path = StatePath(slot);
    if (!ctx_.session->host().SaveState(path, &err)) {
        Message(err.empty() ? "Não foi possível salvar" : err);
        return false;
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (ctx_.session->Screenshot(&rgba, &w, &h))
        WritePng(path + ".png", rgba.data(), w, h, 400);
    return true;
}

bool GameUi::LoadSlot(int slot) {
    std::string err;
    const std::string path = StatePath(slot);
    if (!FileExists(path)) {
        Message("Nenhum estado salvo ainda");
        return false;
    }
    if (!ctx_.session->host().LoadState(path, &err)) {
        Message(err.empty() ? "Não foi possível carregar" : err);
        return false;
    }
    ctx_.session->ClearRewind();
    return true;
}

void GameUi::TakeScreenshot() {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!ctx_.session->Screenshot(&rgba, &w, &h))
        return;
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
    const std::string dir = JoinPath(Paths::Get().data, "capturas");
    const std::string path = JoinPath(dir, ctx_.args.save_name + "-" + stamp + ".png");
    if (WritePng(path, rgba.data(), w, h))
        Message("Captura salva em " + dir);
}

void GameUi::OpenMenu(Menu m) {
    if (menu_ == Menu::kNone) {
        ctx_.session->set_paused(true);
        if (fast_) SetFastForward(false);
        if (rewinding_) {
            rewinding_ = false;
            ctx_.session->set_rewinding(false);
        }
    }
    if (m == Menu::kSave || m == Menu::kLoad) {
        for (int i = 0; i <= 6; i++) {
            FreeTexture(&impl_->slot_pics[i]);
            const std::string p = StatePath(i);
            impl_->slot_used[i] = FileExists(p);
            impl_->slot_pics[i] = impl_->slot_used[i] ? LoadTexture(p + ".png") : Texture{};
            if (impl_->slot_used[i]) {
                const std::time_t t = static_cast<std::time_t>(FileTime(p) / 1000);
                char buf[40];
                std::strftime(buf, sizeof(buf), "%d/%m %H:%M", std::localtime(&t));
                impl_->slot_label[i] = (i == 0 ? std::string("Rápido") : "Slot " + std::to_string(i)) + " · " + buf;
            } else {
                impl_->slot_label[i] = (i == 0 ? std::string("Rápido") : "Slot " + std::to_string(i)) + " · vazio";
            }
        }
    }
    menu_ = m;
    opened_at_ = Now();
    focus_ = 1;   // focus the menu window on its first frame
    menu_frames_ = 0;
    sel_ = 0;
    activate_ = false;
    move_ = move_x_ = 0;
}

void GameUi::CloseMenu() {
    menu_ = Menu::kNone;
    ctx_.session->set_paused(false);
}

void GameUi::Draw() {
    DrawBezel();
    if (menu_ != Menu::kNone)
        DrawMenu();
    DrawOsd();
}

void GameUi::DrawOsd() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = ctx_.window->scale();
    std::string text;
    if (Now() < message_until_)
        text = message_;
    else if (fast_)
        text = "» " + ctx_.settings->Str("ff_speed", "3") + "x";
    else if (Settings::Get().Bool("show_fps", false))
        text = std::to_string(static_cast<int>(ctx_.session->measured_fps() + 0.5)) + " FPS";
    if (text.empty())
        return;
    ImGui::PushFont(ctx_.window->fonts().body);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos((ds.x - ts.x) / 2, 24 * s);
    dl->AddRectFilled(ImVec2(pos.x - 18 * s, pos.y - 10 * s), ImVec2(pos.x + ts.x + 18 * s, pos.y + ts.y + 10 * s),
                      IM_COL32(15, 13, 26, 210), 20 * s);
    dl->AddText(pos, IM_COL32(255, 255, 255, 255), text.c_str());
    ImGui::PopFont();
}

// --- Frame around the picture (as BezelView on Android) -------------------------

void GameUi::DrawBezel() {
    std::string style = ctx_.settings->Str("bezel", "auto");
    if (style == "off" || ctx_.system->id == Sys::NDS || ctx_.system->id == Sys::N3DS)
        return;
    const Rect r = ctx_.session->presenter().viewport();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (r.w <= 0 || r.h <= 0)
        return;
    const float side = std::min<float>(r.x, ds.x - (r.x + r.w));
    const float tops = std::min<float>(r.y, ds.y - (r.y + r.h));
    if (side < ds.x * 0.06f && tops < ds.y * 0.06f)
        return;
    const bool handheld = ctx_.system->id == Sys::GB || ctx_.system->id == Sys::GBA || ctx_.system->id == Sys::GG;
    if (style == "auto")
        style = handheld ? "handheld" : "tv";
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const uint32_t color = ctx_.system->color;
    const ImVec2 g0(static_cast<float>(r.x), static_cast<float>(r.y));
    const ImVec2 g1(static_cast<float>(r.x + r.w), static_cast<float>(r.y + r.h));
    // Paint everything around the picture: four bands.
    auto around = [&](auto paint) {
        dl->PushClipRect(ImVec2(0, 0), ImVec2(ds.x, g0.y), false); paint(); dl->PopClipRect();
        dl->PushClipRect(ImVec2(0, g1.y), ds, false); paint(); dl->PopClipRect();
        dl->PushClipRect(ImVec2(0, g0.y), ImVec2(g0.x, g1.y), false); paint(); dl->PopClipRect();
        dl->PushClipRect(ImVec2(g1.x, g0.y), ImVec2(ds.x, g1.y), false); paint(); dl->PopClipRect();
    };
    const float pad = std::min(r.w, r.h) * (style == "handheld" ? 0.09f : 0.07f);
    around([&] {
        // Backdrop: the console colour, darker towards the edges, fine lines.
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ds, Mix(color, 0x000000, 0.45f), Mix(color, 0x000000, 0.75f),
                                    Mix(color, 0x000000, 0.88f), Mix(color, 0x000000, 0.65f));
        for (float x = -ds.y; x < ds.x; x += 9)
            dl->AddLine(ImVec2(x, ds.y), ImVec2(x + ds.y, 0), IM_COL32(255, 255, 255, 12));
        // The console's photo, faded, in the wider side bar (when no menu is over it).
        if (impl_->photo.valid() && side > 140) {
            const bool left = g0.x >= ds.x - g1.x;
            const float ax = left ? 0 : g1.x, aw = left ? g0.x : ds.x - g1.x;
            const float sc = std::min(aw * 0.8f / impl_->photo.w, ds.y * 0.45f / impl_->photo.h);
            const float pw = impl_->photo.w * sc, ph = impl_->photo.h * sc;
            const ImVec2 p0(ax + (aw - pw) / 2, ds.y / 2 - ph / 2);
            dl->AddImage(impl_->photo.im(), p0, ImVec2(p0.x + pw, p0.y + ph), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 110));
        }
        if (style == "handheld") {
            const uint32_t body = ctx_.system->id == Sys::GB ? 0xC4C0CC : ctx_.system->id == Sys::GBA ? 0x6248C4 : 0x28282E;
            const ImVec2 gl0(g0.x - pad, g0.y - pad * 0.8f), gl1(g1.x + pad, g1.y + pad * 1.6f);
            const ImVec2 s0(gl0.x - pad * 0.9f, gl0.y - pad * 0.9f), s1(gl1.x + pad * 0.9f, gl1.y + pad * 0.9f);
            dl->AddRectFilled(ImVec2(s0.x + 6, s0.y + 10), ImVec2(s1.x + 6, s1.y + 10), IM_COL32(0, 0, 0, 120), pad * 1.4f);
            dl->AddRectFilled(s0, s1, Mix(body, 0xFFFFFF, 0.1f), pad * 1.4f);
            dl->AddRect(s0, s1, IM_COL32(255, 255, 255, 70), pad * 1.4f, 0, 2);
            dl->AddRectFilled(gl0, gl1, ctx_.system->id == Sys::GB ? Rgb(0x545066) : Rgb(0x121218), pad * 0.6f);
            dl->AddCircleFilled(ImVec2(gl0.x + pad * 0.5f, (g0.y + g1.y) / 2 - pad), pad * 0.16f, IM_COL32(255, 48, 64, 255));
            ImGui::PushFont(ctx_.window->fonts().bold);
            const std::string name = ctx_.system->full_name;
            const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
            dl->AddText(ImVec2((gl0.x + gl1.x - ts.x) / 2, gl1.y - pad * 0.45f - ts.y), IM_COL32(255, 255, 255, 170), name.c_str());
            ImGui::PopFont();
        } else if (style == "tv") {
            const ImVec2 t0(g0.x - pad * 0.35f, g0.y - pad * 0.35f), t1(g1.x + pad * 0.35f, g1.y + pad * 0.35f);
            const ImVec2 c0(t0.x - pad * 1.3f, t0.y - pad * 1.1f), c1(t1.x + pad * 1.3f, t1.y + pad * 2.1f);
            dl->AddRectFilled(ImVec2(c0.x, c0.y + 12), ImVec2(c1.x, c1.y + 12), IM_COL32(0, 0, 0, 130), pad);
            dl->AddRectFilledMultiColor(c0, c1, Mix(0x26242C, color, 0.2f), Mix(0x26242C, color, 0.2f),
                                        Mix(0x141218, color, 0.1f), Mix(0x141218, color, 0.1f));
            dl->AddRect(c0, c1, IM_COL32(255, 255, 255, 50), pad, 0, 2);
            dl->AddRectFilled(t0, t1, IM_COL32(8, 8, 10, 255), pad * 0.9f);
            dl->AddRect(ImVec2(t0.x - 4, t0.y - 4), ImVec2(t1.x + 4, t1.y + 4), Mix(color, 0xFFFFFF, 0.3f, 200), pad, 0, 2.5f);
            const float chin = t1.y + pad * 1.05f;
            for (float x = c0.x + pad * 1.4f; x < (c0.x + c1.x) / 2 - pad; x += 7)
                dl->AddRectFilled(ImVec2(x, chin - pad * 0.35f), ImVec2(x + 3, chin + pad * 0.35f), IM_COL32(0, 0, 0, 90), 1.5f);
            for (int i = 0; i < 2; i++)
                dl->AddCircleFilled(ImVec2(c1.x - pad * (1.6f + i * 1.1f), chin), pad * 0.36f, IM_COL32(20, 20, 22, 255));
        } else {  // arcade
            const uint32_t neon[] = {0xFF4081, 0xFFC400, 0x00E5FF, color};
            for (int i = 0; i < 4; i++) {
                const float o = pad * (0.6f + i * 0.55f);
                dl->AddLine(ImVec2(g0.x - o, 0), ImVec2(g0.x - o, ds.y), Rgb(neon[i], 220), pad * 0.22f);
                dl->AddLine(ImVec2(g1.x + o, 0), ImVec2(g1.x + o, ds.y), Rgb(neon[i], 220), pad * 0.22f);
            }
        }
    });
}

// --- Quick menu (as QuickMenu on Android) -------------------------------------

void GameUi::DrawMenu() {
    menu_frames_++;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = ctx_.window->scale();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(5, 5, 12, 140));
    if (menu_ == Menu::kSave || menu_ == Menu::kLoad) {
        DrawSlots(menu_ == Menu::kSave);
        return;
    }
    const float width = std::min(420.0f * s, ds.x * 0.9f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, ds.y));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    if (focus_) {
        ImGui::SetNextWindowFocus();
        focus_ = 0;
    }
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PushFont(ctx_.window->fonts().title);
    ImGui::TextWrapped("%s", ctx_.args.title.c_str());
    ImGui::PopFont();
    ImGui::PushFont(ctx_.window->fonts().small);
    ImGui::TextDisabled("%s · jogando há %s", ctx_.system->short_name,
                        PlayStats::FormatTime(static_cast<int64_t>((Now() - impl_->started) * 1000)).c_str());
    ImGui::PopFont();
    ImGui::Spacing();

    struct Item { std::string label, value; std::function<void()> act; };
    std::vector<Item> items;
    auto add = [&](std::string l, std::string v, std::function<void()> f) { items.push_back({std::move(l), std::move(v), std::move(f)}); };
    if (menu_ == Menu::kMain) {
        add("Continuar", "", [&] { CloseMenu(); });
        add("Salvar estado", "6 slots", [&] { OpenMenu(Menu::kSave); });
        add("Carregar estado", "", [&] {
            if (ctx_.session->net().active()) Message("Carregar estados fica desligado no jogo online");
            else OpenMenu(Menu::kLoad);
        });
        add("Acelerar", ctx_.settings->Str("ff_speed", "3") + "x", [&] { CloseMenu(); SetFastForward(true); });
        const int f = std::atoi(ctx_.settings->Str("filter", "2").c_str());
        add("Filtro de imagem", (f >= 0 && f < 10) ? kFilters[f] : "", [&] { OpenMenu(Menu::kFilter); });
        if (ctx_.system->id != Sys::NDS && ctx_.system->id != Sys::N3DS) {
            const std::string b = ctx_.settings->Str("bezel", "auto");
            std::string bn = kBezels[1];
            for (int i = 0; i < 5; i++) if (b == kBezelKeys[i]) bn = kBezels[i];
            add("Moldura", bn, [&] { OpenMenu(Menu::kBezel); });
        }
        add("Tela cheia", ctx_.window->fullscreen() ? "ligada" : "desligada", [&] {
            ctx_.window->ToggleFullscreen();
            Settings::Get().Set("fullscreen", ctx_.window->fullscreen());
        });
        add("Captura de tela", "F12", [&] { TakeScreenshot(); });
        add("Reiniciar o jogo", "", [&] { ctx_.session->host().Reset(); CloseMenu(); Message("Jogo reiniciado"); });
        add("Sair do jogo", "", [&] { quit_ = true; });
    } else if (menu_ == Menu::kFilter) {
        for (int i = 0; i < 10; i++)
            add(kFilters[i], std::to_string(i) == ctx_.settings->Str("filter", "2") ? "✓" : "", [this, i] {
                if (ctx_.settings->Has("filter")) ctx_.settings->Set("filter", std::to_string(i));
                else Settings::Get().Set("filter", std::to_string(i));
                ApplyDisplay();
                OpenMenu(Menu::kMain);
            });
    } else if (menu_ == Menu::kBezel) {
        for (int i = 0; i < 5; i++)
            add(kBezels[i], ctx_.settings->Str("bezel", "auto") == kBezelKeys[i] ? "✓" : "", [this, i] {
                if (ctx_.settings->Has("bezel")) ctx_.settings->Set("bezel", kBezelKeys[i]);
                else Settings::Get().Set("bezel", std::string(kBezelKeys[i]));
                OpenMenu(Menu::kMain);
            });
    }

    const float row = 50 * s;
    const int n = static_cast<int>(items.size());
    if (n > 0) sel_ = ((sel_ + move_) % n + n) % n;   // wraps around
    const bool moved = move_ != 0;
    move_ = move_x_ = 0;
    int run = activate_ ? sel_ : -1;
    activate_ = false;
    for (size_t i = 0; i < items.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const bool clicked = ImGui::InvisibleButton("##item", ImVec2(w, row));
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y > 0)
            sel_ = static_cast<int>(i);
        if (static_cast<int>(i) == sel_ && (moved || menu_frames_ <= 1))
            ImGui::SetScrollHereY();
        const bool focused = static_cast<int>(i) == sel_;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (focused)
            dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + row), IM_COL32(124, 92, 255, 255), IM_COL32(90, 63, 224, 255),
                                        IM_COL32(90, 63, 224, 255), IM_COL32(124, 92, 255, 255));
        if (focused)
            dl->AddRect(p, ImVec2(p.x + w, p.y + row), IM_COL32(124, 92, 255, 255), 12 * s, 0, 2);
        ImGui::PushFont(focused ? ctx_.window->fonts().bold : ctx_.window->fonts().body);
        const float ty = p.y + (row - ImGui::GetFontSize()) / 2;
        dl->AddText(ImVec2(p.x + 16 * s, ty), focused ? IM_COL32(255, 255, 255, 255) : IM_COL32(217, 213, 238, 235), items[i].label.c_str());
        ImGui::PopFont();
        if (!items[i].value.empty()) {
            ImGui::PushFont(ctx_.window->fonts().small);
            const ImVec2 vs = ImGui::CalcTextSize(items[i].value.c_str());
            dl->AddText(ImVec2(p.x + w - vs.x - 16 * s, p.y + (row - vs.y) / 2), IM_COL32(200, 196, 225, 220), items[i].value.c_str());
            ImGui::PopFont();
        }
        ImGui::PopID();
        if (clicked)
            run = static_cast<int>(i);
    }
    if (run >= 0 && run < n)
        items[static_cast<size_t>(run)].act();
    ImGui::PushFont(ctx_.window->fonts().small);
    ImGui::SetCursorPosY(ds.y - 40 * s);
    ImGui::TextDisabled(menu_ == Menu::kMain ? "Esc / B: voltar ao jogo" : "Esc / B: voltar");
    ImGui::PopFont();
    ImGui::End();
}

void GameUi::DrawSlots(bool save) {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = ctx_.window->scale();
    const float w = std::min(ds.x * 0.9f, 1100.0f * s);
    ImGui::SetNextWindowPos(ImVec2(ds.x / 2, ds.y / 2), 0, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    if (focus_) {
        ImGui::SetNextWindowFocus();
        focus_ = 0;
    }
    ImGui::Begin("##slots", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(ctx_.window->fonts().title);
    ImGui::TextUnformatted(save ? "Salvar em qual slot?" : "Carregar qual slot?");
    ImGui::PopFont();
    ImGui::Spacing();
    // 6 slots in rows of 3: arrows move, Enter picks.
    sel_ = std::clamp(sel_ + move_ * 3 + move_x_, 0, 5);
    move_ = move_x_ = 0;
    int run = activate_ ? sel_ + 1 : -1;
    activate_ = false;
    const float cw = (w - ImGui::GetStyle().WindowPadding.x * 2 - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
    const float ch = cw * 0.62f;
    for (int i = 1; i <= 6; i++) {
        ImGui::PushID(i);
        if (i > 1 && (i - 1) % 3 != 0)
            ImGui::SameLine();
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##slot", ImVec2(cw, ch + 34 * s));
        if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0))
            sel_ = i - 1;
        const bool focused = sel_ == i - 1;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(7, 6, 12, 255), 10 * s);
        const Texture& t = impl_->slot_pics[i];
        if (t.valid()) {
            const float sc = std::min(cw / t.w, ch / t.h);
            const float iw = t.w * sc, ih = t.h * sc;
            dl->AddImage(t.im(), ImVec2(p.x + (cw - iw) / 2, p.y + (ch - ih) / 2), ImVec2(p.x + (cw + iw) / 2, p.y + (ch + ih) / 2));
        }
        if (focused)
            dl->AddRect(ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + cw + 3, p.y + ch + 3), IM_COL32(255, 181, 71, 255), 12 * s, 0, 3 * s);
        ImGui::PushFont(ctx_.window->fonts().small);
        dl->AddText(ImVec2(p.x + 4, p.y + ch + 8 * s), IM_COL32(207, 203, 227, 230), impl_->slot_label[i].c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::PopID();
        if (clicked)
            run = i;
    }
    if (run >= 1 && run <= 6) {
        if (save) {
            if (SaveSlot(run)) Message("Salvo no slot " + std::to_string(run));
            CloseMenu();
        } else if (impl_->slot_used[run]) {
            if (LoadSlot(run)) Message("Carregado o slot " + std::to_string(run));
            CloseMenu();
        }
    }
    ImGui::End();
}

}  // namespace cartucho
