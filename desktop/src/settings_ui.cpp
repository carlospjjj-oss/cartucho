#include "settings_ui.h"

#include <imgui.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "cloud.h"
#include "library.h"
#include "ra.h"
#include "os.h"
#include "paths.h"
#include "settings.h"
#include "sounds.h"
#include "window.h"

#ifndef CARTUCHO_VERSION
#define CARTUCHO_VERSION "dev"
#endif

namespace cartucho {

namespace {

const char* kFilters[] = {"Nítido", "Suave", "Nítido suave", "Tela LCD", "Linhas de TV", "Bordas arredondadas 2x",
                          "Bordas arredondadas 3x", "TV de tubo (CRT)", "TV de tubo curva", "TV antiga (cabo composto)"};
const char* kBezels[] = {"Sem moldura", "Automática (do console)", "Portátil", "TV de tubo", "Fliperama"};
const char* kBezelKeys[] = {"off", "auto", "handheld", "tv", "arcade"};
const char* kSpeeds[] = {"2", "3", "4", "6", "8"};

struct Bios { const char* file; const char* console; const char* note; };
const Bios kBios[] = {
    {"scph5501.bin", "PlayStation", "Recomendada (os jogos rodam sem ela, mas alguns funcionam melhor com)"},
    {"scph5500.bin", "PlayStation", "Japonesa (opcional)"},
    {"scph5502.bin", "PlayStation", "Europeia (opcional)"},
    {"gba_bios.bin", "Game Boy Advance", "Opcional: mostra a abertura original"},
    {"bios7.bin", "Nintendo DS", "Opcional"},
    {"bios9.bin", "Nintendo DS", "Opcional"},
    {"firmware.bin", "Nintendo DS", "Opcional"},
};

bool Check(const char* label, const char* key, bool def) {
    bool v = Settings::Get().Bool(key, def);
    if (ImGui::Checkbox(label, &v)) {
        Settings::Get().Set(key, v);
        return true;
    }
    return false;
}

void Combo(const char* label, const char* key, const char* def, const char* const* names, const char* const* keys, int n) {
    const std::string cur = Settings::Get().Str(key, def);
    int at = 0;
    for (int i = 0; i < n; i++) if (cur == keys[i]) at = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    if (ImGui::BeginCombo(label, names[at])) {
        for (int i = 0; i < n; i++)
            if (ImGui::Selectable(names[i], i == at)) Settings::Get().Set(key, std::string(keys[i]));
        ImGui::EndCombo();
    }
}

// Tests: CARTUCHO_SETTINGS_TAB opens that tab first.
ImGuiTabItemFlags TabFlags(const char* name) {
    static int frames = 0;
    static const char* want = std::getenv("CARTUCHO_SETTINGS_TAB");
    if (!want || frames > 30) return 0;
    if (std::strcmp(name, "Sobre") == 0) frames++;
    return std::strcmp(want, name) == 0 ? ImGuiTabItemFlags_SetSelected : 0;
}

const char* KeyName(SDL_Scancode sc) {
    const char* n = SDL_GetScancodeName(sc);
    return (n && *n) ? n : "—";
}

const char* ButtonName(SDL_GameControllerButton b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return "A (baixo)";
        case SDL_CONTROLLER_BUTTON_B: return "B (direita)";
        case SDL_CONTROLLER_BUTTON_X: return "X (esquerda)";
        case SDL_CONTROLLER_BUTTON_Y: return "Y (cima)";
        case SDL_CONTROLLER_BUTTON_BACK: return "Back/Select";
        case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
        case SDL_CONTROLLER_BUTTON_START: return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad cima";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad baixo";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad esquerda";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad direita";
        case SDL_CONTROLLER_BUTTON_INVALID: return "—";
        default: return "outro";
    }
}

// The console buttons shown for rebinding, in a friendly order.
const int kPadOrder[] = {kUp, kDown, kLeft, kRight, kA, kB, kX, kY, kL, kR, kL2, kR2, kStart, kSelect, kL3, kR3};

}  // namespace

void SettingsUi::Open() {
    open_ = true;
    rescan_ = false;
    binding_ = -999;
    input_.Load();
}

void SettingsUi::Back() {
    if (binding_ != -999) binding_ = -999;
    else open_ = false;
}

bool SettingsUi::HandleEvent(const SDL_Event& e) {
    if (binding_ == -999) return false;
    if (!binding_pad_ && e.type == SDL_KEYDOWN) {
        const SDL_Scancode sc = e.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) { binding_ = -999; return true; }
        if (binding_ >= 0) input_.keys[binding_] = sc;
        else input_.action_keys[static_cast<Action>(-1 - binding_)] = sc;
        input_.Save();
        binding_ = -999;
        return true;
    }
    if (binding_pad_ && e.type == SDL_CONTROLLERBUTTONDOWN) {
        const auto b = static_cast<SDL_GameControllerButton>(e.cbutton.button);
        if (binding_ >= 0) input_.pad_buttons[binding_] = b;
        else input_.action_buttons[static_cast<Action>(-1 - binding_)] = b;
        input_.Save();
        binding_ = -999;
        return true;
    }
    if (binding_pad_ && e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        binding_ = -999;
        return true;
    }
    // Eat other input while waiting.
    return e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN;
}

bool SettingsUi::Draw(Window& win, Sounds& sounds) {
    (void)sounds;
    if (!open_) return false;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = win.scale();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(5, 5, 12, 140));
    ImGui::SetNextWindowPos(ImVec2(ds.x / 2, ds.y / 2), 0, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(ds.x - 40, 980 * s), std::min(ds.y - 40, 720 * s)));
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::Begin("##settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImGui::PushFont(win.fonts().title);
    ImGui::TextUnformatted("Configurações");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetWindowWidth() - 140 * s);
    if (ImGui::Button("  Fechar  ")) open_ = false;
    ImGui::Spacing();
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Geral", nullptr, TabFlags("Geral"))) {
            ImGui::BeginChild("##g");
            ImGui::SeparatorText("Tela inicial");
            Check("Sons da interface", "ui_sounds", true);
            Check("Música de fundo", "ui_music", true);
            const std::string music = Settings::Get().Str("ui_music_file", "");
            ImGui::TextDisabled("Música: %s", music.empty() ? "a do Cartucho" : FileName(music).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Escolher arquivo…")) {
                const std::string f = PickFile("Música de fundo", "Música (.ogg, .wav)", "*.ogg;*.wav");
                if (!f.empty()) { Settings::Get().Set("ui_music_file", f); sounds.LoadMusic(f); }
            }
            if (!music.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Usar a do Cartucho")) { Settings::Get().Set("ui_music_file", std::string()); sounds.LoadMusic(""); }
            }
            Check("Fundo animado", "ui_animated_bg", true);
            Check("Baixar capas e imagens dos jogos (internet)", "download_covers", true);
            Check("Mostrar imagens do jogo no painel (tela-título, capturas)", "game_media", true);
            ImGui::SeparatorText("Ao abrir um jogo");
            Check("Continuar de onde parou", "resume", true);
            Check("Jogos em tela cheia", "fullscreen", true);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Jogo", nullptr, TabFlags("Jogo"))) {
            ImGui::BeginChild("##j");
            ImGui::SeparatorText("Imagem");
            const char* fkeys[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
            Combo("Filtro de imagem", "filter", "2", kFilters, fkeys, 10);
            Combo("Moldura", "bezel", "auto", kBezels, kBezelKeys, 5);
            Check("Escala em números inteiros (pixels iguais)", "integer_scale", false);
            Check("Mostrar quadros por segundo", "show_fps", false);
            ImGui::SeparatorText("Velocidade");
            const char* speed_names[] = {"2x", "3x", "4x", "6x", "8x"};
            Combo("Acelerar", "ff_speed", "3", speed_names, kSpeeds, 5);
            ImGui::SeparatorText("Consoles");
            Check("PlayStation: controle com analógico (DualShock)", "ps1_dualshock", true);
            ImGui::TextDisabled("Cada jogo também tem as próprias opções no menu dentro do jogo (Esc).");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controles", nullptr, TabFlags("Controles"))) {
            ImGui::BeginChild("##c");
            if (binding_ != -999) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 181, 71, 255));
                ImGui::TextUnformatted(binding_pad_ ? "Aperte um botão do controle… (Esc cancela)" : "Aperte uma tecla… (Esc cancela)");
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("Clique num botão para trocar. Os controles USB e Bluetooth funcionam sozinhos.");
            }
            if (ImGui::BeginTable("##binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableSetupColumn("Botão do console");
                ImGui::TableSetupColumn("Teclado");
                ImGui::TableSetupColumn("Controle");
                ImGui::TableHeadersRow();
                auto row = [&](const char* name, int id, const char* key, const char* pad) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(name);
                    ImGui::TableNextColumn();
                    ImGui::PushID(id * 2);
                    const bool wk = binding_ == id && !binding_pad_;
                    if (ImGui::Button(wk ? "…" : key, ImVec2(-1, 0))) { binding_ = id; binding_pad_ = false; }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::PushID(id * 2 + 1);
                    const bool wp = binding_ == id && binding_pad_;
                    if (ImGui::Button(wp ? "…" : pad, ImVec2(-1, 0))) { binding_ = id; binding_pad_ = true; }
                    ImGui::PopID();
                };
                for (int id : kPadOrder) {
                    auto k = input_.keys.find(id);
                    auto p = input_.pad_buttons.find(id);
                    row(PadName(id), id, k != input_.keys.end() ? KeyName(k->second) : "—",
                        p != input_.pad_buttons.end() ? ButtonName(p->second) : "—");
                }
                for (int a = 1; a < static_cast<int>(Action::kCount); a++) {
                    const Action act = static_cast<Action>(a);
                    auto k = input_.action_keys.find(act);
                    auto p = input_.action_buttons.find(act);
                    row(ActionName(act), -1 - a, k != input_.action_keys.end() ? KeyName(k->second) : "—",
                        p != input_.action_buttons.end() ? ButtonName(p->second) : "—");
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            if (ImGui::Button("Teclado: voltar ao padrão")) { input_.ResetKeys(); input_.Save(); }
            ImGui::SameLine();
            if (ImGui::Button("Controle: voltar ao padrão")) { input_.ResetPad(); input_.Save(); }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pastas", nullptr, TabFlags("Pastas"))) {
            ImGui::BeginChild("##p");
            ImGui::TextWrapped("O Cartucho procura jogos nestas pastas (e nas subpastas):");
            ImGui::Spacing();
            const auto folders = Library::Get().folders();
            if (folders.empty()) ImGui::TextDisabled("Nenhuma pasta ainda.");
            for (size_t i = 0; i < folders.size(); i++) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::BulletText("%s", folders[i].c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Abrir")) OpenUrl(folders[i]);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remover")) { Library::Get().RemoveFolder(folders[i]); rescan_ = true; }
                ImGui::PopID();
            }
            ImGui::Spacing();
            if (ImGui::Button("Adicionar pasta…")) {
                const std::string d = PickFolder("Escolha a pasta dos seus jogos");
                if (!d.empty()) { Library::Get().AddFolder(d); rescan_ = true; }
            }
            ImGui::SameLine();
            if (ImGui::Button("Procurar jogos novos")) rescan_ = true;
            ImGui::Spacing();
            ImGui::SeparatorText("Seus dados");
            ImGui::TextWrapped("Saves, estados e BIOS ficam em:\n%s", Paths::Get().data.c_str());
            ImGui::TextDisabled("As pastas saves/ e states/ são iguais às do celular: copie de um para o outro.");
            if (ImGui::Button("Abrir a pasta de dados")) OpenUrl(Paths::Get().data);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("BIOS", nullptr, TabFlags("BIOS"))) {
            ImGui::BeginChild("##b");
            const std::string sys = JoinPath(Paths::Get().data, "system");
            ImGui::TextWrapped("O Cartucho não vem com BIOS. Se você tiver as do seu próprio console, importe aqui. "
                               "Nenhuma é obrigatória.");
            ImGui::Spacing();
            if (ImGui::BeginTable("##bios", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Arquivo");
                ImGui::TableSetupColumn("Console");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (const auto& b : kBios) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool have = FileExists(JoinPath(sys, b.file));
                    ImGui::TextColored(have ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.6f, 0.6f, 0.7f, 1), "%s %s", have ? "✓" : "·", b.file);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(b.console);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", b.note);
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            if (ImGui::Button("Importar BIOS…")) {
                const std::string f = PickFile("Escolha o arquivo de BIOS", "BIOS (.bin)", "*.bin;*.rom");
                if (!f.empty()) {
                    std::vector<uint8_t> bytes;
                    MakeDirs(sys);
                    if (ReadFileBytes(f, &bytes)) WriteFileBytes(JoinPath(sys, FileName(f)), bytes.data(), bytes.size());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Abrir a pasta das BIOS")) { MakeDirs(sys); OpenUrl(sys); }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Contas e nuvem", nullptr, TabFlags("Contas e nuvem"))) {
            ImGui::BeginChild("##acc");
            DrawAccounts(win);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sobre", nullptr, TabFlags("Sobre"))) {
            ImGui::BeginChild("##s");
            DrawUpdates();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PushFont(win.fonts().bold);
            ImGui::Text("Cartucho para PC  %s", CARTUCHO_VERSION);
            ImGui::PopFont();
            ImGui::TextWrapped("Emulador de vários consoles, gratuito e de código aberto. Usa núcleos libretro: "
                               "mGBA/VBA-M, melonDS, Azahar, FCEUmm, Snes9x, Genesis Plus GX, PCSX ReARMed.");
            ImGui::Spacing();
            ImGui::TextWrapped("Snes9x e Genesis Plus GX não permitem uso comercial: o Cartucho é e sempre será gratuito.");
            ImGui::Spacing();
            if (ImGui::Button("Página do projeto")) OpenUrl("https://github.com/carlospjjj-oss/cartucho");
            ImGui::SameLine();
            if (ImGui::Button("Ver o registro (cartucho.log)")) OpenUrl(JoinPath(Paths::Get().data, "cartucho.log"));
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    return open_;
}

SettingsUi::~SettingsUi() {
    if (worker_.joinable()) worker_.join();
}

void SettingsUi::Run(std::function<std::string()> job) {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = "Aguarde…";
    }
    worker_ = std::thread([this, job] {
        const std::string r = job();
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = r;
        busy_ = false;
    });
}

void SettingsUi::DrawAccounts(Window& win) {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (!status_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 205, 120, 255));
            ImGui::TextWrapped("%s", status_.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
    }
    if (busy_) ImGui::BeginDisabled();

    // RetroAchievements.
    ImGui::PushFont(win.fonts().bold);
    ImGui::TextUnformatted("RetroAchievements (conquistas)");
    ImGui::PopFont();
    if (RA::SignedIn()) {
        ImGui::Text("Conectado como %s · %d pontos", RA::User().c_str(), RA::Score());
        Check("Ganhar conquistas nos jogos", "ra_enabled", true);
        if (ImGui::Button("Sair da conta")) RA::SignOut();
    } else {
        ImGui::TextWrapped("Ganhe conquistas jogando, a mesma conta do celular. Crie uma conta grátis em retroachievements.org.");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::InputText("Usuário", ra_user_, sizeof(ra_user_));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::InputText("Senha", ra_pass_, sizeof(ra_pass_), ImGuiInputTextFlags_Password);
        if (ImGui::Button("Entrar") && ra_user_[0] && ra_pass_[0]) {
            const std::string u = ra_user_, p = ra_pass_;
            std::memset(ra_pass_, 0, sizeof(ra_pass_));
            Run([u, p] {
                const std::string e = RA::SignIn(u, p);
                return e.empty() ? "Conectado ao RetroAchievements." : e;
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Criar conta")) OpenUrl("https://retroachievements.org/createaccount.php");
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Saves in the cloud.
    ImGui::PushFont(win.fonts().bold);
    ImGui::TextUnformatted("Saves na nuvem (PC e celular juntos)");
    ImGui::PopFont();
    ImGui::TextWrapped("Escolha um arquivo dentro de uma pasta sincronizada (Google Drive, OneDrive, Dropbox…). Depois de "
                       "jogar, o Cartucho grava nele seus saves, estados e trapaças. No celular, escolha o mesmo arquivo em "
                       "Configurações → Saves na nuvem.");
    const std::string file = Cloud::File();
    if (file.empty()) {
        ImGui::TextDisabled("Nenhum arquivo escolhido.");
    } else {
        ImGui::TextWrapped("Arquivo: %s", file.c_str());
        const int64_t last = Cloud::LastSent();
        if (last > 0) {
            const std::time_t t = static_cast<std::time_t>(last / 1000);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", std::localtime(&t));
            ImGui::TextDisabled("Última sincronização: %s", buf);
        }
    }
    if (ImGui::Button(file.empty() ? "Escolher arquivo da nuvem" : "Trocar arquivo")) {
        const std::string f = PickSaveFile("Arquivo de saves na nuvem", "Cartucho-saves.zip", "Backup do Cartucho", "*.zip");
        if (!f.empty()) {
            Cloud::SetFile(f);
            // An existing backup there (from the phone): bring it in first.
            if (FileSize(f) > 0)
                Run([] {
                    const auto r = Cloud::Fetch();
                    return r.ok ? "Saves trazidos da nuvem. " + r.message : r.message;
                });
            else
                Run([] { return Cloud::Send().message; });
        }
    }
    if (!file.empty()) {
        Check("Enviar automaticamente depois de jogar", "cloud_auto", true);
        if (ImGui::Button("Enviar agora")) Run([] { return Cloud::Send().message; });
        ImGui::SameLine();
        if (ImGui::Button("Baixar da nuvem")) Run([] { return Cloud::Fetch().message; });
        ImGui::SameLine();
        if (ImGui::Button("Esquecer arquivo")) Cloud::SetFile("");
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Manual backup.
    ImGui::PushFont(win.fonts().bold);
    ImGui::TextUnformatted("Backup");
    ImGui::PopFont();
    ImGui::TextWrapped("Um .zip com saves, estados, trapaças e BIOS. É o mesmo formato do celular: dá para levar de um para o outro.");
    if (ImGui::Button("Fazer backup…")) {
        const std::string f = PickSaveFile("Salvar backup", Backup::DefaultName(), "Backup do Cartucho", "*.zip");
        if (!f.empty())
            Run([f] {
                const auto r = Backup::Export(f, false);
                return r.ok ? "Backup salvo: " + std::to_string(r.files) + " arquivos." : r.message;
            });
    }
    ImGui::SameLine();
    if (ImGui::Button("Restaurar backup…")) {
        const std::string f = PickFile("Restaurar backup", "Backup do Cartucho", "*.zip");
        if (!f.empty())
            Run([f] {
                const auto r = Backup::Restore(f);
                return r.ok ? "Restaurado: " + std::to_string(r.files) + " arquivos." : r.message;
            });
    }
    if (busy_) ImGui::EndDisabled();
}

void SettingsUi::DrawUpdates() {
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.Size > 2 ? ImGui::GetIO().Fonts->Fonts[2] : ImGui::GetFont());
    ImGui::TextUnformatted("Atualizações");
    ImGui::PopFont();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    }
    Check("Procurar atualizações ao abrir", "auto_update", true);
    if (busy_) ImGui::BeginDisabled();
    if (ImGui::Button("Procurar agora"))
        Run([this] {
            update_ = Updater::Check();
            update_checked_ = true;
            return update_.message;
        });
    if (update_checked_ && update_.newer) {
        ImGui::SameLine();
        if (ImGui::Button("Atualizar agora"))
            Run([this] {
                std::string m;
                if (Updater::Install(update_, &m)) quit_ = true;
                return m;
            });
    }
    if (busy_) ImGui::EndDisabled();
    ImGui::Spacing();
}

}  // namespace cartucho
