#include "game.h"

#include <SDL.h>
#include <glad/gles2.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>

#include <chrono>
#include <cstdio>
#include <memory>

#include "audio.h"
#include "game_ui.h"
#include "input.h"
#include "paths.h"
#include "platform.h"
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

// Core options as the Android app sets them (GameActivity.applyCoreOptions).
void ApplyCoreOptions(CoreHost& host, Sys sys, const GameSettings& gs) {
    switch (sys) {
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
                                                  sys == Sys::GBA ? "gba_bios.bin" : "gb_bios.bin"));
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
            host.SetOption("citra_layout_option", gs.Str("n3ds_layout_pc", "large_screen"));
            break;
        default:
            break;
    }
}

}  // namespace

int RunGame(const GameArgs& args) {
    OpenLog("game.log");
    const SystemInfo* sys = SystemByKey(args.system);
    if (!sys) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cartucho", "Console desconhecido.", nullptr);
        return 2;
    }
    Settings::Get().Load();
    GameSettings gs(sys->key, args.save_name);

    Window win;
    std::string error;
    if (!win.Open(args.title + " — Cartucho", Settings::Get().Bool("fullscreen", true), &error)) {
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

    auto session = std::make_unique<Session>();
    const std::string core = CorePath(*sys);
    if (!FileExists(core)) {
        error = "O emulador de " + std::string(sys->full_name) + " não foi encontrado (" + core + ").";
    } else if (session->Open(core, system_dir, save_dir, &error)) {
        if (sys->id == Sys::PS1 && Settings::Get().Bool("ps1_dualshock", true))
            session->host().SetPortDevice((2 << 8) | 5);
        ApplyCoreOptions(session->host(), sys->id, gs);
        if (session->OnSurfaceCreated(args.rom, JoinPath(save_dir, args.save_name + ".srm"), &error))
            error.clear();
    }
    if (!error.empty()) {
        PlatformLog(3, error.c_str());
        GameUi::ShowFatal(win, error);
        win.Close();
        return 4;
    }
    ctx.session = session.get();

    Input input;
    input.Load();
    input.stick_as_dpad = sys->id != Sys::N3DS && sys->id != Sys::PS1;
    ctx.input = &input;

    Audio audio;
    audio.Start(&session->host());

    GameUi ui(ctx);
    ui.ApplyDisplay();
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
    session.reset();   // unloads the game, writing its save
    win.Close();
    return 0;
}

}  // namespace cartucho
