// Cartucho PC - keyboard and controllers to the console's buttons
// (RetroPad), plus the frontend's own shortcuts.
#pragma once

#include <SDL.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cartucho {

// RetroPad button bits (RETRO_DEVICE_ID_JOYPAD_*).
enum Pad : int {
    kB = 0, kY = 1, kSelect = 2, kStart = 3, kUp = 4, kDown = 5, kLeft = 6, kRight = 7,
    kA = 8, kX = 9, kL = 10, kR = 11, kL2 = 12, kR2 = 13, kL3 = 14, kR3 = 15, kPadCount = 16,
};
const char* PadName(int id);

// Frontend actions.
enum class Action { kNone, kMenu, kFastForward, kRewind, kQuickSave, kQuickLoad, kFullscreen, kScreenshot, kCount };
const char* ActionName(Action a);

class Input {
public:
    void Load();   // bindings from the settings
    void Save();

    // Feed every SDL event; returns an action pressed (or released for
    // rewind: kRewind with `pressed` false) if any.
    struct Event { Action action = Action::kNone; bool pressed = false; };
    Event Handle(const SDL_Event& e);

    // Current RetroPad mask and left stick (-32768..32767).
    uint32_t buttons() const;
    void stick(int16_t* x, int16_t* y) const;
    // Left stick also moves the D-pad (consoles without analog sticks).
    bool stick_as_dpad = true;
    // A controller was used lately (hide on-screen hints for the keyboard).
    bool pad_active() const { return pad_active_; }
    std::string pad_name() const;

    // Rebinding: the next key/button pressed becomes `id`'s.
    std::map<int, SDL_Scancode> keys;                       // pad id -> key
    std::map<Action, SDL_Scancode> action_keys;
    std::map<int, SDL_GameControllerButton> pad_buttons;    // pad id -> controller button
    std::map<Action, SDL_GameControllerButton> action_buttons;
    void ResetKeys();
    void ResetPad();

private:
    void OpenControllers();
    std::vector<SDL_GameController*> pads_;
    uint32_t key_mask_ = 0;
    uint32_t pad_mask_ = 0;
    int16_t pad_lx_ = 0, pad_ly_ = 0;
    bool pad_active_ = false;
    bool l2_down_ = false, r2_down_ = false;
};

}  // namespace cartucho
