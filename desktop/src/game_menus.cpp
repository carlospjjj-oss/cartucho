// Cartucho PC - the quick menu's screens: what each list shows and does,
// the screens with text fields (cheats, an address), online play, link
// cable, achievements, recording, and what is polled every frame.
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "achievements.h"
#include "game.h"
#include "game_ui_impl.h"
#include "os.h"
#include "paths.h"
#include "platform.h"
#include "playstats.h"
#include "recorder.h"
#include "session.h"
#include "settings.h"
#include "systems.h"
#include "window.h"

namespace cartucho {

namespace {

double NowS() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Choice {
    const char* name;
    const char* value;
};

const char* kFilters[] = {"Nítido", "Suave", "Nítido suave", "Tela LCD", "Linhas de TV", "Bordas arredondadas 2x",
                          "Bordas arredondadas 3x", "TV de tubo (CRT)", "TV de tubo curva", "TV antiga (cabo composto)"};
const std::vector<Choice> kBezelChoices = {{"Sem moldura", "off"}, {"Automática (do console)", "auto"}, {"Portátil", "handheld"},
                                           {"TV de tubo", "tv"}, {"Fliperama", "arcade"}};
const std::vector<Choice> kLcd = {{"Desligado (cores fortes do emulador)", "off"}, {"GBA / GBC original", "GBA"},
                                  {"GBA SP com luz", "GBASP Backlit"}, {"GBA Micro", "Micro"}, {"DS Lite", "DS-Lite"},
                                  {"Nintendo Switch Online", "NSO"}};
const std::vector<Choice> kPalettes = {{"Preto e branco", "black and white"}, {"Game Boy original (verde)", "original gameboy"},
                                       {"Game Boy Pocket / GBA SP", "gba sp"}, {"Mar azul", "blue sea"},
                                       {"Cavaleiro sombrio", "dark knight"}, {"Floresta verde", "green forest"},
                                       {"Deserto quente", "hot desert"}, {"Sonho rosa", "pink dreams"}};
const std::vector<Choice> kBlend = {{"Desligada", "disabled"}, {"Inteligente (só onde pisca)", "smart"},
                                    {"Rastro de LCD", "motion blur"}};
const std::vector<Choice> kRunAhead = {{"Desligado", "0"}, {"1 quadro (recomendado)", "1"}, {"2 quadros", "2"}};
const std::vector<Choice> kFf = {{"2x", "2"}, {"3x", "3"}, {"4x", "4"}, {"6x", "6"}, {"8x", "8"}};
const std::vector<Choice> kNdsLayouts = {{"Lado a lado", "left-right"}, {"Uma em cima da outra", "top-bottom"},
                                         {"Tela de cima grande + de baixo pequena", "largescreen-top"},
                                         {"Tela de baixo grande + de cima pequena", "largescreen-bottom"},
                                         {"Só a tela de cima", "top"}, {"Só a tela de baixo", "bottom"}};
const std::vector<Choice> k3dsLayouts = {{"Tela grande + pequena", "large_screen"}, {"Lado a lado", "side_by_side"},
                                         {"Uma em cima da outra", "default"}, {"Só uma tela", "single_screen"}};

std::vector<Choice> FilterChoices() {
    std::vector<Choice> out;
    static const char* values[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    for (int i = 0; i < 10; i++) out.push_back({kFilters[i], values[i]});
    return out;
}

std::string NameOf(const std::vector<Choice>& list, const std::string& value) {
    for (const auto& c : list)
        if (value == c.value) return c.name;
    return value;
}

struct OptionDef {
    const char* key;
    const char* title;
    std::vector<Choice> choices;
};

std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (const auto& x : v) out += (out.empty() ? "" : sep) + x;
    return out;
}

std::vector<std::string> Recent(const char* key) {
    std::vector<std::string> out;
    const auto j = Settings::Get().Raw(key);
    if (j.is_array())
        for (const auto& x : j)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

void Remember(const char* key, const std::string& address) {
    std::vector<std::string> list = {address};
    for (const auto& r : Recent(key))
        if (r != address && list.size() < 5) list.push_back(r);
    Settings::Get().Set(key, list);
}

// "host:port" -> host and port (default when missing).
void SplitAddress(const std::string& text, int def_port, std::string* host, int* port) {
    const size_t colon = text.find(':');
    *host = text.substr(0, colon);
    *port = def_port;
    if (colon != std::string::npos) {
        const int p = std::atoi(text.c_str() + colon + 1);
        if (p > 0 && p < 65536) *port = p;
    }
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) a++;
    return s.substr(a);
}

}  // namespace

// --- Options of this game --------------------------------------------------------

namespace {

std::vector<OptionDef> GameOptions(const SystemInfo& sys, bool run_ahead) {
    std::vector<OptionDef> out;
    out.push_back({"filter", "Filtro de imagem", FilterChoices()});
    if (sys.id != Sys::NDS && sys.id != Sys::N3DS) out.push_back({"bezel", "Moldura", kBezelChoices});
    if (sys.id == Sys::GB || sys.id == Sys::GBA) out.push_back({"lcd_color", "Cores de tela real", kLcd});
    if (sys.id == Sys::GB) out.push_back({"gb_palette", "Paleta do Game Boy clássico", kPalettes});
    if (sys.id == Sys::GB || sys.id == Sys::GBA) out.push_back({"frame_blend", "Mistura de quadros", kBlend});
    if (run_ahead) out.push_back({"run_ahead", "Run-ahead (menos atraso)", kRunAhead});
    out.push_back({"ff_speed", "Velocidade do acelerar", kFf});
    return out;
}

}  // namespace

std::string GameUi::MenuTitle() const {
    switch (menu_) {
        case Menu::kFilter: return "Filtro de imagem";
        case Menu::kBezel: return "Moldura";
        case Menu::kLayout: return "Telas";
        case Menu::kOptions: return "Opções deste jogo";
        case Menu::kDisc: return "Trocar disco";
        case Menu::kOnline: return "Jogar online";
        case Menu::kOnlineRooms: return "Entrar numa sala";
        case Menu::kLink: return "Link cable";
        case Menu::kLinkType: return "Tipo de link";
        case Menu::kLinkPlayers: return "Quantos jogadores?";
        case Menu::kLinkRooms: return "Entrar numa partida";
        case Menu::kCheatSources: return "Qual lista?";
        case Menu::kOptionPick:
            for (const auto& o : GameOptions(*ctx_.system, true))
                if (impl_->pick_key == o.key) return o.title;
            return "";
        default: return "";
    }
}

void GameUi::BuildItems(std::vector<MenuItem>* items) {
    auto add = [&](std::string l, std::string v, std::function<void()> f) {
        items->push_back({std::move(l), std::move(v), std::move(f)});
    };
    auto text = [&](std::string l) { items->push_back({std::move(l), "", nullptr}); };
    Session& session = *ctx_.session;
    const Sys sys = ctx_.system->id;
    // Global setting, or this game's own when it has one.
    auto set_display = [this](const char* key, const std::string& value) {
        if (ctx_.settings->Has(key)) ctx_.settings->Set(key, value);
        else Settings::Get().Set(key, value);
    };

    switch (menu_) {
    case Menu::kMain: {
        add("Continuar", "", [this] { CloseMenu(); });
        add("Salvar estado", "6 slots", [this] { OpenMenu(Menu::kSave); });
        add("Carregar estado", "", [this] {
            if (ctx_.session->net().active()) Message("Carregar estados fica desligado no jogo online");
            else OpenMenu(Menu::kLoad);
        });
        add("Acelerar", ctx_.settings->Str("ff_speed", "3") + "x", [this] { CloseMenu(); SetFastForward(true); });
        const int f = std::atoi(ctx_.settings->Str("filter", "2").c_str());
        add("Filtro de imagem", (f >= 0 && f < 10) ? kFilters[f] : "", [this] { OpenMenu(Menu::kFilter); });
        if (sys != Sys::NDS && sys != Sys::N3DS)
            add("Moldura", NameOf(kBezelChoices, ctx_.settings->Str("bezel", "auto")), [this] { OpenMenu(Menu::kBezel); });
        if (sys == Sys::NDS || sys == Sys::N3DS) add("Telas", "", [this] { OpenMenu(Menu::kLayout); });
        if ((sys == Sys::GB || sys == Sys::GBA) && session.link_supported())
            add("Link cable", session.link_state() == 0 ? "conectado" : "", [this] { OpenMenu(Menu::kLink); });
        if (sys != Sys::NDS && sys != Sys::N3DS)
            add("Jogar online", session.net().playing() ? "conectado" : session.net().active() ? "esperando" : "",
                [this] { OpenMenu(Menu::kOnline); });
        if (sys != Sys::N3DS) {
            const bool any = std::any_of(impl_->cheats.begin(), impl_->cheats.end(), [](const Cheat& c) { return c.enabled; });
            add("Trapaças", any ? "ativas" : "", [this] { OpenMenu(Menu::kCheats); });
        }
        if (sys == Sys::PS1) add("Trocar disco", "", [this] { OpenMenu(Menu::kDisc); });
        if (ctx_.ra) add("Conquistas", "", [this] { OpenMenu(Menu::kAchievements); });
        add(impl_->writer.running() ? "Parar gravação" : "Gravar vídeo", impl_->writer.running() ? "● REC" : "",
            [this] { ToggleRecording(); CloseMenu(); });
        add("Captura de tela", "F12", [this] { TakeScreenshot(); });
        add("Opções deste jogo", "", [this] { OpenMenu(Menu::kOptions); });
        add("Tela cheia", ctx_.window->fullscreen() ? "ligada" : "desligada", [this] {
            ctx_.window->ToggleFullscreen();
            Settings::Get().Set("fullscreen", ctx_.window->fullscreen());
        });
        add("Reiniciar o jogo", "", [this] {
            if (ctx_.session->net().active()) {
                Message("Reiniciar fica desligado no jogo online");
                return;
            }
            ctx_.session->host().Reset();
            ctx_.session->ClearRewind();
            CloseMenu();
            Message("Jogo reiniciado");
        });
        add("Sair do jogo", "", [this] { quit_ = true; });
        break;
    }
    case Menu::kFilter:
        for (int i = 0; i < 10; i++)
            add(kFilters[i], std::to_string(i) == ctx_.settings->Str("filter", "2") ? "✓" : "", [this, i, set_display] {
                set_display("filter", std::to_string(i));
                ApplyDisplay();
                OpenMenu(Menu::kMain);
            });
        break;
    case Menu::kBezel:
        for (const auto& c : kBezelChoices) {
            const std::string v = c.value;
            add(c.name, ctx_.settings->Str("bezel", "auto") == v ? "✓" : "", [this, v, set_display] {
                set_display("bezel", v);
                OpenMenu(Menu::kMain);
            });
        }
        break;
    case Menu::kLayout: {
        const bool nds = sys == Sys::NDS;
        const char* key = nds ? "nds_layout_pc" : "n3ds_layout_pc";
        const std::string cur = Settings::Get().Str(key, nds ? "left-right" : "large_screen");
        for (const auto& c : nds ? kNdsLayouts : k3dsLayouts) {
            const std::string v = c.value;
            add(c.name, cur == v ? "✓" : "", [this, key, v] {
                Settings::Get().Set(key, v);
                ApplyCoreOptions(ctx_.session->host(), *ctx_.system, *ctx_.settings);
                CloseMenu();
            });
        }
        if (!nds)
            add("Trocar telas de lugar", Settings::Get().Bool("n3ds_swap", false) ? "ligado" : "desligado", [this] {
                Settings::Get().Set("n3ds_swap", !Settings::Get().Bool("n3ds_swap", false));
                ApplyCoreOptions(ctx_.session->host(), *ctx_.system, *ctx_.settings);
            });
        break;
    }
    case Menu::kOptions: {
        text("O que for escolhido aqui vale só para este jogo; o resto segue as Configurações.");
        for (const auto& o : GameOptions(*ctx_.system, session.run_ahead_possible())) {
            const auto& own = ctx_.settings->json();
            const auto it = own.find(o.key);
            const std::string v = (it != own.end() && it->is_string()) ? it->get<std::string>() : "";
            const std::string key = o.key;
            add(o.title, v.empty() ? "padrão do app" : NameOf(o.choices, v), [this, key] {
                impl_->pick_key = key;
                OpenMenu(Menu::kOptionPick);
            });
        }
        break;
    }
    case Menu::kOptionPick: {
        for (const auto& o : GameOptions(*ctx_.system, true)) {
            if (impl_->pick_key != o.key) continue;
            const auto& own = ctx_.settings->json();
            const auto it = own.find(o.key);
            const std::string cur = (it != own.end() && it->is_string()) ? it->get<std::string>() : "";
            const std::string key = o.key;
            auto apply = [this, key](const std::string& v) {
                ctx_.settings->Set(key, v);
                if (key == "filter") ApplyDisplay();
                else if (key == "run_ahead") ctx_.session->set_run_ahead(std::atoi(ctx_.settings->Str("run_ahead", "0").c_str()));
                else if (key != "bezel" && key != "ff_speed") ApplyCoreOptions(ctx_.session->host(), *ctx_.system, *ctx_.settings);
                OpenMenu(Menu::kOptions);
            };
            add("Padrão do app", cur.empty() ? "✓" : "", [apply] { apply(""); });
            for (const auto& c : o.choices) {
                const std::string v = c.value;
                add(c.name, cur == v ? "✓" : "", [apply, v] { apply(v); });
            }
        }
        break;
    }
    case Menu::kDisc: {
        CoreHost& host = session.host();
        const unsigned n = host.DiskCount();
        if (n <= 1) {
            text("Este jogo tem um disco só. Para jogos com vários CDs, crie um arquivo .m3u listando os discos "
                 "(um nome de arquivo por linha) e abra o .m3u, ou use um .pbp com todos os discos.");
            break;
        }
        const int current = host.DiskIndex();
        for (unsigned i = 0; i < n; i++) {
            std::string label = host.DiskLabel(i);
            if (label.empty()) label = "Disco " + std::to_string(i + 1);
            add(label, static_cast<int>(i) == current ? "✓" : "", [this, i, label] {
                Message(ctx_.session->host().SetDisk(i) ? "Disco: " + label : "Não deu para trocar o disco");
                CloseMenu();
            });
        }
        break;
    }
    case Menu::kOnline: {
        auto& net = session.net();
        if (net.active()) {
            if (net.playing())
                text("Jogando online. Você é o jogador " + std::to_string(net.local_player() + 1) + ". Atraso da conexão: " +
                     (net.rtt_ms() >= 0 ? std::to_string(net.rtt_ms()) + " ms" : "medindo…"));
            else
                text(PlainText(impl_->wait_intro + "\n" + net.message()));
            add(net.playing() ? "Sair da partida" : "Cancelar", "", [this] {
                NetStop();
                CloseMenu();
            });
            break;
        }
        text("Dois aparelhos jogam o mesmo jogo juntos, cada um com um controle. Funciona entre PC e celular; "
             "os dois precisam do mesmo arquivo de jogo.");
        add("Criar sala (mesma rede)", "", [this] { NetHost(false); });
        add("Entrar numa sala (mesma rede)", "", [this] { OpenMenu(Menu::kOnlineRooms); });
        text("Pela internet: os dois aparelhos precisam se enxergar. O jeito mais fácil é instalar o Tailscale (grátis) "
             "nos dois, na mesma conta; também funciona com ZeroTier ou Radmin VPN. Sem eles, quem cria a sala abre a "
             "porta 5740 (TCP) no roteador.");
        add("Criar sala pela internet", "", [this] { NetHost(true); });
        add("Entrar por endereço", "", [this] {
            impl_->address_for_link = false;
            OpenMenu(Menu::kAddress);
        });
        break;
    }
    case Menu::kOnlineRooms: {
        const uint32_t mine = Discovery::JavaHash(ctx_.game_id);
        int shown = 0;
        for (const auto& h : impl_->browser ? impl_->browser->hosts() : std::vector<Discovery::Host>{}) {
            if (h.mode != Discovery::kModeNetplay) continue;
            shown++;
            const bool same = h.crc == mine;
            const std::string label = (h.game.empty() ? "(sem nome)" : h.game) + " — " + (h.name.empty() ? h.address : h.name);
            const std::string addr = h.address, game = h.game;
            const int port = h.port;
            add(label, same ? h.address : "outro jogo", [this, same, addr, port, game] {
                if (!same) {
                    Message("Essa sala é de outro jogo (" + game + "). Abra o mesmo arquivo de jogo.", 4);
                    return;
                }
                NetJoin(addr, port);
            });
        }
        if (shown == 0) text("Procurando salas na rede… (no outro aparelho: Menu → Jogar online → Criar sala)");
        add("Digitar endereço", "", [this] {
            impl_->address_for_link = false;
            OpenMenu(Menu::kAddress);
        });
        break;
    }
    case Menu::kLink: {
        const int st = session.link_state();
        if (st == 0 || st == 2) {
            text(PlainText(session.link_message().empty() ? (st == 0 ? "Conectado." : "Conectando…") : session.link_message()));
            add(st == 0 ? "Desconectar" : "Cancelar", "", [this] {
                LinkStop();
                Message("Link desconectado");
                CloseMenu();
            });
            break;
        }
        text("Troque e batalhe com outro aparelho rodando um jogo compatível: celular com Cartucho, outro PC ou o "
             "VBA-M de computador.");
        add("Hospedar partida (mesma rede)", "", [this] {
            impl_->internet = false;
            if (ctx_.system->id == Sys::GB) LinkHost(2, 2, false);
            else OpenMenu(Menu::kLinkType);
        });
        add("Entrar numa partida (mesma rede)", "", [this] { OpenMenu(Menu::kLinkRooms); });
        text("Pela internet: os dois aparelhos precisam se enxergar (Tailscale, ZeroTier ou Radmin VPN; ou a porta 5738 "
             "TCP e UDP aberta no roteador de quem hospeda). Tem mais atraso que na mesma rede.");
        add("Hospedar pela internet", "", [this] {
            impl_->internet = true;
            if (ctx_.system->id == Sys::GB) LinkHost(2, 2, true);
            else OpenMenu(Menu::kLinkType);
        });
        add("Entrar por endereço", "", [this] {
            impl_->address_for_link = true;
            OpenMenu(Menu::kAddress);
        });
        break;
    }
    case Menu::kLinkType:
        add("Cabo de GBA — trocas e batalhas", "", [this] { OpenMenu(Menu::kLinkPlayers); });
        add("Adaptador sem fio — Union Room", "Pokémon FR/LG/Emerald", [this] { LinkHost(1, 2, impl_->internet); });
        break;
    case Menu::kLinkPlayers:
        for (int p = 2; p <= 4; p++) add(std::to_string(p) + " jogadores", "", [this, p] { LinkHost(0, p, impl_->internet); });
        break;
    case Menu::kLinkRooms: {
        const bool gb = sys == Sys::GB;
        int shown = 0;
        for (const auto& h : impl_->browser ? impl_->browser->hosts() : std::vector<Discovery::Host>{}) {
            if (h.mode == Discovery::kModeNetplay) continue;
            shown++;
            const int mode = h.mode >= 0 ? h.mode : gb ? 2 : 0;
            std::string details = Discovery::ModeName(mode) + " · " + std::to_string(h.players) + " jog.";
            if (h.crc != 0 && ctx_.rom_crc != 0 && h.crc != ctx_.rom_crc) details += " · outro jogo";
            const std::string addr = h.address;
            const int port = h.port, players = h.players;
            add((h.game.empty() ? "(sem jogo)" : h.game) + " — " + (h.name.empty() ? h.address : h.name), details,
                [this, gb, mode, players, addr, port] {
                    if (gb != (mode == 2)) {
                        Message(std::string("Essa partida é de ") + (mode == 2 ? "Game Boy" : "GBA") +
                                    "; abra um jogo do mesmo console.", 4);
                        return;
                    }
                    LinkJoin(mode, players, addr, port);
                });
        }
        if (shown == 0) text("Procurando partidas na rede… (no outro aparelho: Link cable → Hospedar partida)");
        add("Digitar endereço", "", [this] {
            impl_->address_for_link = true;
            OpenMenu(Menu::kAddress);
        });
        break;
    }
    case Menu::kCheatSources:
        for (size_t i = 0; i < impl_->sources.size(); i++) {
            const CheatDb::Source src = impl_->sources[i];
            add(src.Label(), "", [this, src] {
                if (impl_->cheat_thread.joinable()) impl_->cheat_thread.join();
                impl_->cheat_state = 1;
                impl_->entries.clear();
                impl_->cheat_thread = std::thread([this, src] {
                    std::vector<CheatDb::Entry> got;
                    std::string err;
                    const bool ok = CheatDb::Load(src, &got, &err);
                    std::lock_guard<std::mutex> lock(impl_->cheat_mutex);
                    impl_->cheat_loaded = got;
                    impl_->cheat_error = err;
                    impl_->cheat_state = ok ? 2 : 3;
                });
                OpenMenu(Menu::kCheatPick);
            });
        }
        break;
    default:
        break;
    }
}

// --- Screens drawn with widgets ----------------------------------------------------

bool GameUi::DrawPanel() {
    const bool panel = menu_ == Menu::kCheats || menu_ == Menu::kCheatAdd || menu_ == Menu::kCheatPick ||
                       menu_ == Menu::kAddress || menu_ == Menu::kAchievements;
    if (!panel) return false;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = ctx_.window->scale();
    const float width = std::min(620.0f * s, ds.x * 0.94f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, ds.y));
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    const bool first = focus_ != 0;
    if (focus_) {
        ImGui::SetNextWindowFocus();
        focus_ = 0;
    }
    ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    // Keyboard and controller move through the widgets here, from the start.
    if (first) ImGui::SetNavCursorVisible(true);
    move_ = move_x_ = 0;
    activate_ = false;
    auto title = [&](const char* t) {
        ImGui::PushFont(ctx_.window->fonts().title);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        ImGui::Spacing();
    };
    const ImVec2 button(-1, 46 * s);
    const Menu before = menu_;

    if (menu_ == Menu::kCheats) {
        title("Trapaças");
        if (impl_->cheats.empty())
            ImGui::TextWrapped("Nenhuma trapaça ainda. Busque trapaças prontas deste jogo ou digite um código "
                               "(GameShark, Action Replay, CodeBreaker, Game Genie…).");
        bool changed = false;
        int remove = -1;
        for (size_t i = 0; i < impl_->cheats.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            auto& c = impl_->cheats[i];
            if (ImGui::Checkbox(c.name.c_str(), &c.enabled)) changed = true;
            if (first && i == 0) ImGui::SetKeyboardFocusHere(-1);
            const float bw = ImGui::CalcTextSize("Apagar").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
            if (ImGui::SmallButton("Apagar")) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) {
            impl_->cheats.erase(impl_->cheats.begin() + remove);
            changed = true;
        }
        if (changed) {
            SaveCheats();
            ApplyCheats();
        }
        ImGui::Spacing();
        if (CheatDb::Supported(ctx_.system->id) && ImGui::Button("Buscar trapaças prontas deste jogo", button)) {
            impl_->sources = CheatDb::Find(ctx_.system->id, ctx_.file_name);
            if (impl_->sources.empty()) Message("A base de trapaças não tem este jogo. Você ainda pode digitar um código.", 4);
            else OpenMenu(Menu::kCheatSources);
        }
        if (first && impl_->cheats.empty()) ImGui::SetKeyboardFocusHere(-1);
        if (ImGui::Button("Digitar um código", button)) {
            impl_->name_buf[0] = impl_->code_buf[0] = 0;
            OpenMenu(Menu::kCheatAdd);
        }
        if (ImGui::Button("Voltar", button)) OpenMenu(Menu::kMain);
    } else if (menu_ == Menu::kCheatAdd) {
        title("Nova trapaça");
        ImGui::TextUnformatted("Nome");
        ImGui::SetNextItemWidth(-1);
        if (first) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##name", "Ex.: Dinheiro infinito", impl_->name_buf, sizeof(impl_->name_buf));
        ImGui::TextUnformatted("Código (um por linha)");
        ImGui::InputTextMultiline("##code", impl_->code_buf, sizeof(impl_->code_buf), ImVec2(-1, 200 * s),
                                  ImGuiInputTextFlags_CharsUppercase);
        if (ImGui::Button("Salvar e ativar", button)) {
            const std::string code = Trim(impl_->code_buf);
            if (!code.empty()) {
                std::string name = Trim(impl_->name_buf);
                if (name.empty()) name = "Trapaça " + std::to_string(impl_->cheats.size() + 1);
                impl_->cheats.push_back({name, code, true});
                SaveCheats();
                ApplyCheats();
                Message("Trapaça ativada");
            }
            OpenMenu(Menu::kCheats);
        }
        if (ImGui::Button("Cancelar", button)) OpenMenu(Menu::kCheats);
    } else if (menu_ == Menu::kCheatPick) {
        const int st = impl_->cheat_state;
        if (st == 1) {
            title("Buscando trapaças…");
            ImGui::TextDisabled("Baixando a lista da base do libretro.");
        } else if (st == 3) {
            title("Não deu para baixar");
            std::lock_guard<std::mutex> lock(impl_->cheat_mutex);
            ImGui::TextWrapped("Confira a internet e tente de novo. (%s)", impl_->cheat_error.c_str());
            if (ImGui::Button("Voltar", button)) OpenMenu(Menu::kCheats);
        } else {
            if (impl_->entries.empty() && st == 2) {
                std::lock_guard<std::mutex> lock(impl_->cheat_mutex);
                for (const auto& e : impl_->cheat_loaded) {
                    Impl::Pickable p;
                    p.e = e;
                    for (const auto& c : impl_->cheats) p.have = p.have || c.code == e.code;
                    impl_->entries.push_back(p);
                }
            }
            const std::string t = "Escolha as trapaças (" + std::to_string(impl_->entries.size()) + ")";
            title(t.c_str());
            if (ImGui::Button("Adicionar e ativar as marcadas", button)) {
                int added = 0;
                for (const auto& p : impl_->entries)
                    if (p.chosen && !p.have) {
                        impl_->cheats.push_back({p.e.name, p.e.code, true});
                        added++;
                    }
                if (added > 0) {
                    SaveCheats();
                    ApplyCheats();
                    Message(std::to_string(added) + (added == 1 ? " trapaça ativada" : " trapaças ativadas"));
                }
                OpenMenu(Menu::kCheats);
            }
            if (first) ImGui::SetKeyboardFocusHere(-1);
            ImGui::BeginChild("##entries", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, 0);
            for (size_t i = 0; i < impl_->entries.size(); i++) {
                ImGui::PushID(static_cast<int>(i));
                auto& p = impl_->entries[i];
                if (p.have) {
                    ImGui::BeginDisabled();
                    bool on = true;
                    ImGui::Checkbox((p.e.name + "  (já tem)").c_str(), &on);
                    ImGui::EndDisabled();
                } else {
                    ImGui::Checkbox(p.e.name.c_str(), &p.chosen);
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    } else if (menu_ == Menu::kAddress) {
        const bool link = impl_->address_for_link;
        title(link ? "Endereço de quem está hospedando" : "Endereço de quem criou a sala");
        ImGui::TextWrapped("Na mesma rede: o IP que aparece no outro aparelho. Pela internet: o endereço do Tailscale/ZeroTier "
                           "ou o IP público de quem hospeda.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1);
        if (first) {
            const auto recent = Recent(link ? "link_recent" : "net_recent");
            std::snprintf(impl_->addr_buf, sizeof(impl_->addr_buf), "%s", recent.empty() ? "" : recent[0].c_str());
            ImGui::SetKeyboardFocusHere();
        }
        const bool enter = ImGui::InputTextWithHint("##addr", "192.168.0.10, 100.101.1.2 ou nome", impl_->addr_buf,
                                                    sizeof(impl_->addr_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        for (const auto& r : Recent(link ? "link_recent" : "net_recent")) {
            if (ImGui::Selectable(("↺  " + r).c_str())) std::snprintf(impl_->addr_buf, sizeof(impl_->addr_buf), "%s", r.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("Conectar", button) || enter) {
            const std::string t = Trim(impl_->addr_buf);
            if (!t.empty()) {
                Remember(link ? "link_recent" : "net_recent", t);
                std::string host;
                int port;
                SplitAddress(t, link ? Discovery::kLinkPort : Discovery::kNetplayPort, &host, &port);
                if (link) {
                    const std::string ip = Discovery::Resolve(host);
                    if (ip.empty()) Message("Não encontrei \"" + host + "\".", 4);
                    else LinkJoin(ctx_.system->id == Sys::GB ? 2 : 0, 2, ip, port);
                } else {
                    NetJoin(host, port);
                }
            }
        }
        if (ImGui::Button("Cancelar", button)) OpenMenu(link ? Menu::kLink : Menu::kOnline);
    } else if (menu_ == Menu::kAchievements) {
        const auto lines = ctx_.ra ? ctx_.ra->List() : std::vector<std::string>{};
        int earned = 0;
        for (const auto& l : lines) earned += l.rfind("1|", 0) == 0;
        const std::string t = lines.empty() ? std::string("Conquistas")
                                            : "Conquistas: " + std::to_string(earned) + " de " + std::to_string(lines.size());
        title(t.c_str());
        if (ImGui::Button("Voltar", button)) OpenMenu(Menu::kMain);
        if (first) ImGui::SetKeyboardFocusHere(-1);
        if (lines.empty())
            ImGui::TextWrapped("Ainda carregando, ou este jogo não tem conquistas no RetroAchievements.");
        ImGui::BeginChild("##ach", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, 0);
        for (const auto& l : lines) {
            std::vector<std::string> p;
            size_t a = 0;
            for (;;) {
                const size_t b = l.find('|', a);
                p.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            const bool got = !p.empty() && p[0] == "1";
            ImGui::PushStyleColor(ImGuiCol_Text, got ? IM_COL32(255, 205, 90, 255) : IM_COL32(200, 196, 220, 255));
            ImGui::PushFont(ctx_.window->fonts().bold);
            ImGui::TextWrapped("%s %s · %s pts%s", got ? "★" : "○", p.size() > 1 ? PlainText(p[1]).c_str() : "",
                               p.size() > 3 ? p[3].c_str() : "0", p.size() > 4 ? ("  (" + p[4] + ")").c_str() : "");
            ImGui::PopFont();
            ImGui::PopStyleColor();
            if (p.size() > 2) ImGui::TextWrapped("%s", PlainText(p[2]).c_str());
            ImGui::Spacing();
        }
        ImGui::EndChild();
    }
    if (menu_ == before) {
        ImGui::PushFont(ctx_.window->fonts().small);
        ImGui::TextDisabled("Esc / B: voltar");
        ImGui::PopFont();
    }
    ImGui::End();
    return true;
}

// --- Cheats ----------------------------------------------------------------------------

void GameUi::LoadCheats() {
    impl_->cheat_file = CheatFile(ctx_.system->key, ctx_.args.save_name);
    impl_->cheats = LoadCheatList(impl_->cheat_file);
}

void GameUi::SaveCheats() { SaveCheatList(impl_->cheat_file, impl_->cheats); }

void GameUi::ApplyCheats() {
    CoreHost& host = ctx_.session->host();
    host.CheatReset();
    for (size_t i = 0; i < impl_->cheats.size(); i++) {
        const auto& c = impl_->cheats[i];
        if (!c.enabled) continue;
        // One code per line; cores take them joined by '+'.
        std::string joined, line;
        for (char ch : c.code + "\n") {
            if (ch == '\n') {
                line = Trim(line);
                if (!line.empty()) joined += (joined.empty() ? "" : "+") + line;
                line.clear();
            } else {
                line += ch;
            }
        }
        if (!joined.empty()) host.CheatSet(static_cast<unsigned>(i), true, joined);
    }
}

// --- Online play ---------------------------------------------------------------------

void GameUi::FetchPublicAddress() {
    {
        std::lock_guard<std::mutex> lock(impl_->pub_mutex);
        if (!impl_->public_ip.empty()) return;
    }
    if (impl_->pub_thread.joinable()) return;
    impl_->pub_thread = std::thread([this] {
        const std::string ip = Discovery::PublicAddress();
        std::lock_guard<std::mutex> lock(impl_->pub_mutex);
        impl_->public_ip = ip.empty() ? "não foi possível descobrir (sem internet?)" : ip + " (porta aberta no roteador)";
    });
}

void GameUi::NetHost(bool internet) {
    LinkStop();
    impl_->internet = internet;
    if (!ctx_.session->NetHost(Discovery::kNetplayPort, ctx_.game_id)) {
        Message("Não foi possível criar a sala: " + ctx_.session->net().message(), 5);
        return;
    }
    impl_->adv = std::make_unique<Discovery::Advertiser>(Discovery::Announcement{
        Discovery::kNetplayPort, Discovery::kModeNetplay, 2, ctx_.args.title, Discovery::JavaHash(ctx_.game_id)});
    const std::string ips = Join(Discovery::LocalAddresses(), ", ");
    if (!internet) {
        impl_->wait_intro = "Esperando o jogador 2… No outro aparelho, abra o mesmo jogo e entre em Menu → Jogar online → "
                            "Entrar numa sala.\nSeu endereço: " + (ips.empty() ? std::string("sem rede") : ips) +
                            "\nVocê é o jogador 1; o jogo continua enquanto espera.";
    } else {
        const auto vpn = Discovery::VpnAddresses();
        impl_->wait_intro = "Esperando pela internet… No outro aparelho: Menu → Jogar online → Entrar por endereço, e digite ";
        if (!vpn.empty()) {
            impl_->wait_intro += "o endereço da rede virtual: " + Join(vpn, "  ou  ");
        } else {
            impl_->wait_intro += "seu endereço da internet (com a porta 5740 TCP aberta no roteador).";
            FetchPublicAddress();
        }
    }
    impl_->waiting = 1;
    impl_->net_closed_said = false;
    CloseMenu();
}

void GameUi::NetJoin(const std::string& address, int port) {
    LinkStop();
    impl_->internet = false;
    // Names (a Tailscale machine, say) are looked up on the network thread.
    if (!ctx_.session->NetJoin(address, port, ctx_.game_id)) {
        Message("Não foi possível entrar: " + ctx_.session->net().message(), 5);
        return;
    }
    impl_->wait_intro = "Conectando a " + address + ". O jogo de quem criou a sala vai aparecer aqui; seus saves deste "
                        "jogo não são alterados.";
    impl_->waiting = 1;
    impl_->net_closed_said = false;
    CloseMenu();
}

void GameUi::NetStop() {
    impl_->adv.reset();
    impl_->waiting = impl_->waiting == 1 ? 0 : impl_->waiting;
    if (ctx_.session->net().active() || ctx_.session->net().state() != Netplay::State::kIdle) ctx_.session->NetStop();
}

// --- Link cable ------------------------------------------------------------------------

void GameUi::LinkHost(int mode, int players, bool internet) {
    NetStop();
    impl_->internet = internet;
    impl_->adv = std::make_unique<Discovery::Advertiser>(
        Discovery::Announcement{Discovery::kLinkPort, mode, players, ctx_.args.title, ctx_.rom_crc});
    ctx_.session->LinkStart(mode, true, players, "", Discovery::kLinkPort);
    const std::string ips = Join(Discovery::LocalAddresses(), ", ");
    const std::string what = Discovery::ModeName(mode) + ", " + std::to_string(players) + " jogadores. ";
    if (!internet) {
        impl_->wait_intro = what + "No outro aparelho (mesma rede), abra o jogo e entre em Link cable → Entrar numa "
                                   "partida.\nSeu IP: " + (ips.empty() ? std::string("sem rede") : ips);
    } else {
        const auto vpn = Discovery::VpnAddresses();
        impl_->wait_intro = what + "No outro aparelho: Link cable → Entrar por endereço, e digite ";
        if (!vpn.empty()) {
            impl_->wait_intro += "o endereço da rede virtual: " + Join(vpn, "  ou  ");
        } else {
            impl_->wait_intro += "seu endereço da internet (porta 5738 TCP e UDP aberta no roteador).";
            FetchPublicAddress();
        }
    }
    impl_->waiting = 2;
    CloseMenu();
}

void GameUi::LinkJoin(int mode, int players, const std::string& address, int port) {
    NetStop();
    impl_->internet = false;
    ctx_.session->LinkStart(mode, false, players, address, port);
    impl_->wait_intro = "Entrando na partida em " + address + ".";
    impl_->waiting = 2;
    CloseMenu();
}

void GameUi::LinkStop() {
    if (impl_->waiting == 2) {
        impl_->adv.reset();
        impl_->waiting = 0;
    }
    const int st = ctx_.session->link_state();
    if (st == 0 || st == 2) ctx_.session->LinkStop();
}

// --- Video ------------------------------------------------------------------------------

void GameUi::ToggleRecording() {
    if (impl_->writer.running()) {
        impl_->writer.Stop();
        ctx_.session->set_recorder(nullptr);
        Message("Finalizando o vídeo…");
        return;
    }
    CoreHost& host = ctx_.session->host();
    impl_->rec = std::make_shared<Recorder>();
    ctx_.session->set_recorder(impl_->rec);
    impl_->rec->Start(host.fps(), host.sample_rate());
    impl_->writer.Start(impl_->rec, host.fps(), VideosFolder(), ctx_.args.title);
    Message("Gravando… Menu → Parar gravação");
}

// --- Every frame ------------------------------------------------------------------------

void GameUi::Poll() {
    const std::string video = impl_->writer.TakeMessage();
    if (!video.empty()) Message(video, 6);
    const std::string core = ctx_.session->host().TakeMessage();
    if (!core.empty()) Message(core, 4);

    auto& net = ctx_.session->net();
    const auto st = net.state();
    if (impl_->waiting == 1) {
        if (st == Netplay::State::kPlaying) {
            impl_->adv.reset();
            impl_->waiting = 0;
            Message("Conectado! Jogando online como jogador " + std::to_string(net.local_player() + 1) + ".", 4);
        } else if (st == Netplay::State::kFailed || st == Netplay::State::kClosed) {
            const std::string m = net.message();
            impl_->adv.reset();
            impl_->waiting = 0;
            impl_->net_closed_said = true;
            Message("Jogo online não conectou: " + (m.empty() ? std::string("não foi possível conectar.") : m), 6);
            ctx_.session->NetStop();
        }
    } else if (st == Netplay::State::kClosed && !impl_->net_closed_said) {
        impl_->net_closed_said = true;
        const std::string m = net.message();
        Message((m.empty() ? std::string("Partida online encerrada") : m) +
                    (ctx_.session->net_guest() ? ". Você continua no jogo do outro jogador; seus saves não mudam." : "."),
                6);
    }
    if (impl_->waiting == 2) {
        const int ls = ctx_.session->link_state();
        if (ls == 0) {
            impl_->adv.reset();
            impl_->waiting = 0;
            Message("Link conectado! " + ctx_.session->link_message(), 4);
        } else if (ls == 1 || ls == -1) {
            impl_->adv.reset();
            impl_->waiting = 0;
            const std::string m = ctx_.session->link_message();
            Message("Link não conectou: " + (m.empty() ? std::string("não foi possível conectar.") : m) +
                        " Confira se os dois estão na mesma rede e com o mesmo jogo.",
                    7);
        }
    }
}

}  // namespace cartucho
