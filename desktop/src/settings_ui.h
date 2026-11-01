// Cartucho PC - the settings screen of the library (tabs: Geral, Jogo,
// Controles, Pastas, BIOS, Sobre). Same keys as the Android app where they
// mean the same thing.
#pragma once

#include <SDL.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "input.h"
#include "updater.h"

namespace cartucho {

class Window;
class Sounds;

class SettingsUi {
public:
    void Open();
    // Draws the window; false once it was closed.
    bool Draw(Window& win, Sounds& sounds);
    // B / Esc: cancels a rebinding, or closes.
    void Back();
    // While waiting for a key or button to bind; true when it was used.
    bool HandleEvent(const SDL_Event& e);
    bool rescan_requested() const { return rescan_; }
    // An update was installed: the program must close for it.
    bool quit_requested() const { return quit_; }
    ~SettingsUi();

private:
    void DrawAccounts(Window& win);
    void DrawUpdates();
    // Runs `job` off the UI thread; its result shows as the status line.
    void Run(std::function<std::string()> job);
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::mutex status_mutex_;
    std::string status_;
    char ra_user_[64] = {};
    char ra_pass_[128] = {};
    Updater::Info update_;
    std::atomic<bool> update_checked_{false};
    std::atomic<bool> quit_{false};
    bool open_ = false;
    bool rescan_ = false;
    Input input_;
    // Rebinding: a pad button id or an action (as -1 - action).
    int binding_ = -999;
    bool binding_pad_ = false;
};

}  // namespace cartucho
