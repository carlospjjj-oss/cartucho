// Cartucho PC - everything drawn over a running game: the quick menu
// (like the Android one), save slots with pictures, messages, and the
// shortcuts (speed-up, rewind, quick save/load, screenshot).
#pragma once

#include <SDL.h>

#include <memory>
#include <string>
#include <vector>

#include "game.h"
#include "input.h"

namespace cartucho {

class Session;
class Window;
class GameSettings;
struct SystemInfo;

struct GameContext {
    Window* window = nullptr;
    Session* session = nullptr;
    Input* input = nullptr;
    const SystemInfo* system = nullptr;
    GameSettings* settings = nullptr;
    GameArgs args;
    std::string state_dir;
    int width = 0, height = 0;
};

class GameUi {
public:
    explicit GameUi(GameContext& ctx);
    ~GameUi();

    // A fatal error before the game could start: shown until dismissed.
    static void ShowFatal(Window& win, const std::string& message);

    void ApplyDisplay();
    void ResumeIfAny();
    bool HandleEvent(const SDL_Event& e);   // true when the UI used it
    void OnAction(Action a, bool pressed);
    void Draw();
    void OnQuit();                          // writes the continue point

    bool menu_open() const { return menu_ != Menu::kNone; }
    bool quit_requested() const { return quit_; }

    // Shared with the panels in the other files.
    void Message(const std::string& text, double seconds = 2.5);
    struct Impl;

private:
    enum class Menu { kNone, kMain, kSave, kLoad, kFilter, kBezel, kSettings, kCheats, kOnline, kLink, kAchievements, kDisc };
    void OpenMenu(Menu m);
    void CloseMenu();
    void DrawMenu();
    void DrawSlots(bool save);
    void DrawOsd();
    void DrawBezel();
    void SetFastForward(bool on);
    bool SaveSlot(int slot);
    bool LoadSlot(int slot);
    void TakeScreenshot();
    std::string StatePath(int slot) const;

    GameContext& ctx_;
    std::unique_ptr<Impl> impl_;
    Menu menu_ = Menu::kNone;
    bool quit_ = false;
    bool fast_ = false;
    bool rewinding_ = false;
    std::string message_;
    double message_until_ = 0;
    double opened_at_ = 0;
    int focus_ = 0;
    int menu_frames_ = 0;
    int sel_ = 0;            // highlighted item / slot
    bool activate_ = false;  // Enter / A pressed this frame
    int move_ = 0;           // -1 / +1 from the arrows this frame (slots: rows of 3)
    int move_x_ = 0;
};

}  // namespace cartucho
