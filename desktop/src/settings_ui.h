// Cartucho PC - the settings screen of the library (tabs: Geral, Jogo,
// Controles, Pastas, BIOS, Sobre). Same keys as the Android app where they
// mean the same thing.
#pragma once

#include <SDL.h>

#include <string>

#include "input.h"

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

private:
    bool open_ = false;
    bool rescan_ = false;
    Input input_;
    // Rebinding: a pad button id or an action (as -1 - action).
    int binding_ = -999;
    bool binding_pad_ = false;
};

}  // namespace cartucho
