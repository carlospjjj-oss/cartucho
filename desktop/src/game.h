// Cartucho PC - one game being played, in its own process (like on
// Android: cores keep global state, a fresh process is the clean reset).
#pragma once

#include <string>

namespace cartucho {

struct GameArgs {
    std::string rom;          // path of the game file (or the archive holding it)
    std::string entry;        // the game inside a .zip/.7z, or ""
    std::string system;       // SYS key ("GBA", "PS1"...)
    std::string save_name;    // file name without extension (saves/states)
    std::string title;
    bool resume = true;       // continue where it was left
};

// Runs the game until the player leaves. Returns the process exit code.
int RunGame(const GameArgs& args);

class CoreHost;
class GameSettings;
struct SystemInfo;
// Core options as the Android app sets them, from the settings.
void ApplyCoreOptions(CoreHost& host, const SystemInfo& sys, const GameSettings& gs);

}  // namespace cartucho
