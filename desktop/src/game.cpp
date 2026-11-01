#include "game.h"

#include <SDL.h>
#include <glad/gles2.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <zlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "achievements.h"
#include "archive.h"
#include "audio.h"
#include "game_ui.h"
#include "input.h"
#include "paths.h"
#include "platform.h"
#include "ra.h"
#include "session.h"
#include "settings.h"
#include "systems.h"
#include "testhook.h"
#include "window.h"


namespace cartucho {

void OpenLog(const std::string& name);

namespace {

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A game inside a .zip/.7z: extracted once (with a progress screen).
bool Extract(Window& win, const GameArgs& args, std::string* out, std::string* error) {
    *out = ExtractedPath(args.rom, FileSize(args.rom), args.entry);
    if (FileSize(*out) > 0) return true;
    std::atomic<float> progress{0};
    std::atomic<bool> done{false};
    bool ok = false;
    std::thread t([&] {
        ok = ExtractEntry(args.rom, args.entry, *out, [&](float p) { progress = p; }, error);
        done = true;
    });
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) ImGui_ImplSDL2_ProcessEvent(&e);
        win.BeginFrame();
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(11, 10, 20, 255));
        ImGui::SetNextWindowPos(ImVec2(ds.x / 2, ds.y / 2), 0, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(ds.x - 40, 560.0f * win.scale()), 0));
        ImGui::Begin("##extract", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::PushFont(win.fonts().bold);
        ImGui::TextUnformatted("Extraindo o jogo…");
        ImGui::PopFont();
        ImGui::TextDisabled("%s", FileName(args.entry).c_str());
        ImGui::ProgressBar(progress, ImVec2(-1, 0));
        ImGui::End();
        win.EndFrame();
        SDL_Delay(16);
    }
    t.join();
    return ok;
}

// What the phone shows link partners and uses to tell games apart online:
// the CRC of small games, else the file size (GameActivity.prepare()).
uint32_t RomCrc(const SystemInfo& sys, const std::string& rom, bool from_archive) {
    if (from_archive && sys.id != Sys::GB && sys.id != Sys::GBA) return 0;
    if (sys.needs_path || sys.id == Sys::NDS) return 0;
    const uint64_t size = FileSize(rom);
    if (size == 0 || size > 64u * 1024 * 1024) return 0;
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(rom, &bytes)) return 0;
    return static_cast<uint32_t>(crc32(0L, bytes.data(), static_cast<uInt>(bytes.size())));
}

std::string Hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x", v);
    return buf;
}

}  // namespace

// Core options as the Android app sets them (GameActivity.applyCoreOptions).
void ApplyCoreOptions(CoreHost& host, const SystemInfo& sys, const GameSettings& gs) {
    switch (sys.id) {
        case Sys::NDS:
            host.SetOption("melonds_render_mode", "software");
            host.SetOption("melonds_jit_enable", "enabled");
            host.SetOption("melonds_sysfile_mode", "builtin");
            host.SetOption("melonds_console_mode", "ds");
            host.SetOption("melonds_touch_mode", "touch");
            host.SetOption("melonds_show_cursor", "disabled");
            host.SetOption("melonds_show_bios_warnings", "disabled");
            host.SetOption("melonds_number_of_screen_layouts", "1");
            host.SetOption("melonds_screen_layout1", gs.Str("nds_layout_pc", "left-right"));
            break;
        case Sys::PS1:
            host.SetOption("pcsx_rearmed_drc", "enabled");
            host.SetOption("pcsx_rearmed_show_bios_bootlogo", "disabled");
            break;
        case Sys::GB:
        case Sys::GBA: {
            const bool bios = FileExists(JoinPath(JoinPath(Paths::Get().data, "system"),
                                                  sys.id == Sys::GBA ? "gba_bios.bin" : "gb_bios.bin"));
            host.SetOption("vbam_usebios", bios ? "enabled" : "disabled");
            const std::string lcd = gs.Str("lcd_color", "off");
            host.SetOption("vbam_lcdfilter", lcd == "off" ? "disabled" : "enabled");
            if (lcd != "off") {
                host.SetOption("vbam_lcdfilter_gba_panel", lcd);
                host.SetOption("vbam_lcdfilter_gbc_panel", lcd == "NSO" ? "NSO" : "GBC");
            }
            host.SetOption("vbam_palettes", gs.Str("gb_palette", "original gameboy"));
            host.SetOption("vbam_interframeblending", gs.Str("frame_blend", "disabled"));
            break;
        }
        case Sys::N3DS:
            host.SetOption("citra_graphics_api", "OpenGL");
            host.SetOption("citra_use_cpu_jit", "enabled");
            host.SetOption("citra_enable_touch_touchscreen", "enabled");
            host.SetOption("citra_enable_mouse_touchscreen", "enabled");
            host.SetOption("citra_analog_function", "c_stick");
            host.SetOption("citra_resolution_factor", gs.Str("n3ds_resolution", "2"));
            host.SetOption("citra_use_hw_shader", "enabled");
            host.SetOption("citra_use_disk_shader_cache", "enabled");
            host.SetOption("citra_shaders_accurate_mul", "enabled");
            host.SetOption("citra_texture_filter", gs.Str("n3ds_tex_filter", "none"));
            host.SetOption("citra_is_new_3ds", "New 3DS");
            host.SetOption("citra_swap_screen", Settings::Get().Bool("n3ds_swap", false) ? "Bottom" : "Top");
            host.SetOption("citra_layout_option", gs.Str("n3ds_layout_pc", "large_screen"));
            break;
        default:
            break;
    }
}

int RunGame(const GameArgs& args) {
    OpenLog("game.log");
    const SystemInfo* sys = SystemByKey(args.system);
    if (!sys) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cartucho", "Console desconhecido.", nullptr);
        return 2;
    }
    Settings::Get().Load();
    GameSettings gs(sys->key, args.save_name);

    // The 3DS core draws with desktop OpenGL on the PC.
    Window win;
    std::string error;
    if (!win.Open(args.title + " — Cartucho", Settings::Get().Bool("fullscreen", true), &error, sys->id == Sys::N3DS)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cartucho", error.c_str(), nullptr);
        return 3;
    }

    const std::string data = Paths::Get().data;
    const std::string system_dir = JoinPath(data, "system");
    const std::string save_dir = JoinPath(JoinPath(data, "saves"), sys->key);
    const std::string state_dir = JoinPath(JoinPath(data, "states"), sys->key);
    MakeDirs(system_dir);
    MakeDirs(save_dir);
    MakeDirs(state_dir);

    GameContext ctx;
    ctx.window = &win;
    ctx.system = sys;
    ctx.args = args;
    ctx.settings = &gs;
    ctx.state_dir = state_dir;
    ctx.rom = args.rom;
    ctx.file_name = args.entry.empty() ? FileName(args.rom) : FileName(args.entry);
    if (!args.entry.empty() && !Extract(win, args, &ctx.rom, &error)) {
        GameUi::ShowFatal(win, error);
        win.Close();
        return 4;
    }
    // Online play: the id the phone builds (GameActivity: core library | CRC or size).
    ctx.rom_crc = RomCrc(*sys, ctx.rom, !args.entry.empty());
    ctx.game_id = std::string("lib") + sys->core + ".so|" + (ctx.rom_crc ? Hex(ctx.rom_crc) : std::to_string(FileSize(args.rom)));

    auto session = std::make_unique<Session>();
    if (win.desktop_gl()) {
        session->host().SetDesktopGl(true);
        session->host().SetPreferredHwContext(RETRO_HW_CONTEXT_OPENGL_CORE);
    }
    const std::string core = CorePath(*sys);
    if (!FileExists(core)) {
        error = "O emulador de " + std::string(sys->full_name) + " não foi encontrado (" + core + ").";
    } else if (session->Open(core, system_dir, save_dir, &error)) {
        if (sys->id == Sys::PS1 && Settings::Get().Bool("ps1_dualshock", true))
            session->host().SetPortDevice((2 << 8) | 5);
        ApplyCoreOptions(session->host(), *sys, gs);
        if (session->OnSurfaceCreated(ctx.rom, JoinPath(save_dir, args.save_name + ".srm"), &error))
            error.clear();
    }
    if (!error.empty()) {
        PlatformLog(3, error.c_str());
        GameUi::ShowFatal(win, error);
        win.Close();
        return 4;
    }
    ctx.session = session.get();
    if (session->run_ahead_possible())
        session->set_run_ahead(std::atoi(gs.Str("run_ahead", "0").c_str()));

    // RetroAchievements, when signed in.
    std::unique_ptr<RA::Pump> pump;
    const int console = RA::ConsoleId(sys->id, ctx.file_name);
    if (RA::Enabled() && console != 0) {
        ctx.ra = std::make_shared<Achievements>(&session->host());
        session->set_achievements(ctx.ra);
        ctx.ra->Start(RA::User(), Settings::Get().Str("ra_token", ""), console);
        pump = std::make_unique<RA::Pump>(ctx.ra);
    }

    Input input;
    input.Load();
    input.stick_as_dpad = sys->id != Sys::N3DS && sys->id != Sys::PS1;
    ctx.input = &input;

    Audio audio;
    audio.Start(&session->host());

    GameUi ui(ctx);
    ui.ApplyDisplay();
    ui.ApplyCheats();
    if (args.resume)
        ui.ResumeIfAny();

    bool running = true;
    int w = 0, h = 0;
    TestHook test;
    while (running) {
        test.BeforeFrame();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                running = false;
                continue;
            }
            if (ui.HandleEvent(e))
                continue;
            const Input::Event ev = input.Handle(e);
            if (ev.action != Action::kNone)
                ui.OnAction(ev.action, ev.pressed);
        }
        if (ui.quit_requested())
            running = false;
        win.Size(&w, &h);
        if (w != ctx.width || h != ctx.height) {
            ctx.width = w;
            ctx.height = h;
            session->OnSurfaceChanged(w, h);
        }
        // Buttons go to the game unless a menu has them.
        if (ui.menu_open()) {
            session->host().SetButtons(0);
            session->host().SetAnalog(0, 0, 0);
        } else {
            session->host().SetButtons(input.buttons());
            int16_t x, y;
            input.stick(&x, &y);
            session->host().SetAnalog(0, x, y);
        }
        if (audio.running() && audio.target_queue_frames() > 0)
            session->set_audio_fill(static_cast<double>(session->host().QueuedAudioFrames()) / audio.target_queue_frames());
        else
            session->set_audio_fill(-1.0);
        session->Frame(NowSeconds());
        win.BeginFrame();
        ui.Draw();
        win.Render();
        if (test.AfterDraw(w, h))
            running = false;
        win.Swap();
        if (session->host().shutdown_requested())
            running = false;
    }
    ui.OnQuit();
    audio.Stop();
    pump.reset();
    session->set_achievements(nullptr);
    ctx.ra.reset();
    session.reset();   // unloads the game, writing its save
    win.Close();
    return 0;
}

}  // namespace cartucho
