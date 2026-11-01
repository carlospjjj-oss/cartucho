// Cartucho PC - play time, when last played, and the "continue" point
// (state and picture) of each game. data/play/<id>.json and <id>.png.
#pragma once

#include <cstdint>
#include <string>

namespace cartucho {

namespace PlayStats {

std::string Id(const std::string& game_path);
int64_t PlayTimeMs(const std::string& game_path);
int64_t LastPlayed(const std::string& game_path);   // ms since epoch, 0 never
void AddPlayTime(const std::string& game_path, int64_t ms);
std::string ThumbPath(const std::string& game_path);
// The state written when leaving a game (same name as on Android).
std::string ResumeState(const std::string& state_dir, const std::string& save_name);
bool CanResume(const std::string& system_key);

std::string FormatTime(int64_t ms);     // "3 h 20 min"
std::string FormatAgo(int64_t when);    // "há 3 h", "ontem"

}  // namespace PlayStats

}  // namespace cartucho
