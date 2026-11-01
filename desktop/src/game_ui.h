// Cartucho PC - everything drawn over a running game: the quick menu
// (like the Android one) with its screens (save slots, options of this game,
// cheats, online play, link cable, achievements, discs, screen layout,
// video), messages, and the shortcuts (speed-up, rewind, quick save/load,
// screenshot).
#pragma once

#include <SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "game.h"
#include "input.h"

namespace cartucho {

class Achievements;
class Session;
class Window;
class GameSettings;
struct SystemInfo;
struct MenuItem;

struct GameContext {
    Window* window = nullptr;
    Session* session = nullptr;
    Input* input = nullptr;
    const SystemInfo* system = nullptr;
    GameSettings* settings = nullptr;
    GameArgs args;
    std::string rom;           // the file the core plays (extracted, for archives)
    std::string file_name;     // the game's own file name
    uint32_t rom_crc = 0;      // shown to link partners (0 unknown)
    std::string game_id;       // online play: the same id the phone builds
    std::shared_ptr<Achievements> ra;
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
    void ApplyCheats();
    void ResumeIfAny();
    bool HandleEvent(const SDL_Event& e);   // true when the UI used it
    void OnAction(Action a, bool pressed);
    void Draw();
    void OnQuit();                          // writes the continue point

    bool menu_open() const { return menu_ != Menu::kNone; }
    bool quit_requested() const { return quit_; }

    void Message(const std::string& text, double seconds = 2.5);
    struct Impl;

private:
    enum class Menu {
        kNone, kMain, kSave, kLoad, kFilter, kBezel, kLayout, kOptions, kOptionPick, kDisc,
        kCheats, kCheatAdd, kCheatSources, kCheatPick,
        kOnline, kOnlineRooms, kLink, kLinkType, kLinkPlayers, kLinkRooms, kAddress,
        kAchievements,
    };
    void OpenMenu(Menu m);
    void CloseMenu();
    void DrawMenu();
    bool DrawPanel();   // the screens drawn with widgets; false when not one
    void BuildItems(std::vector<MenuItem>* items);   // the list screens
    std::string MenuTitle() const;
    void DrawSlots(bool save);
    void DrawOsd();
    void DrawBezel();
    void SetFastForward(bool on);
    bool SaveSlot(int slot);
    bool LoadSlot(int slot);
    void TakeScreenshot();
    void ToggleRecording();
    void Poll();        // network, recorder and core messages, every frame
    std::string StatePath(int slot) const;

    // Online play and link cable.
    void NetHost(bool internet);
    void NetJoin(const std::string& address, int port);
    void NetStop();
    void LinkHost(int mode, int players, bool internet);
    void LinkJoin(int mode, int players, const std::string& address, int port);
    void LinkStop();
    void FetchPublicAddress();

    // Cheats.
    void LoadCheats();
    void SaveCheats();

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
