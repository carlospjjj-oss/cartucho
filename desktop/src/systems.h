// Cartucho PC - the consoles, as in the Android app (GameSystem.kt).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cartucho {

enum class Sys { GBA, GB, NDS, N3DS, NES, SNES, MD, SMS, GG, PS1, kCount };

struct SystemInfo {
    Sys id;
    const char* key;        // folder names: saves/<key>, states/<key> (same as Android)
    const char* short_name;
    const char* full_name;
    std::vector<std::string> extensions;
    const char* core;       // core library without "lib" and extension: "vbam_libretro"
    uint32_t color;         // 0xRRGGBB
    float cover_aspect;
    bool needs_path;        // core opens the game itself (3DS, PS1)
    const char* photo;      // assets/photos/photo_<photo>.png
};

const std::vector<SystemInfo>& Systems();
const SystemInfo& Info(Sys s);
const SystemInfo* SystemByKey(const std::string& key);
// By file name (and size: big .bin files are disc tracks, not Mega Drive).
const SystemInfo* SystemForFile(const std::string& name, uint64_t size);
// Full path of a system's core next to the program.
std::string CorePath(const SystemInfo& s);

}  // namespace cartucho
