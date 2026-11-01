#include "systems.h"

#include <algorithm>

#include "paths.h"

namespace cartucho {

const std::vector<SystemInfo>& Systems() {
    static const std::vector<SystemInfo> all = {
        {Sys::GBA, "GBA", "GBA", "Game Boy Advance", {"gba", "agb"}, "vbam_libretro", 0x6B4EFF, 1.5f, false, "gba"},
        {Sys::GB, "GB", "GB", "Game Boy / Color", {"gb", "gbc", "sgb", "dmg", "cgb"}, "vbam_libretro", 0x3FB68B, 1.11f, false, "gb"},
        {Sys::NDS, "NDS", "DS", "Nintendo DS", {"nds", "dsi", "ids", "srl"}, "melondsds_libretro", 0x4A90E2, 0.75f, false, "nds"},
        {Sys::N3DS, "N3DS", "3DS", "Nintendo 3DS", {"3ds", "cci", "cxi", "3dsx", "app", "elf", "axf"}, "azahar_libretro", 0xE5484D, 1.2f, true, "n3ds"},
        {Sys::NES, "NES", "NES", "Nintendo (NES)", {"nes", "fds", "unf", "unif"}, "fceumm_libretro", 0xC62828, 1.14f, false, "nes"},
        {Sys::SNES, "SNES", "SNES", "Super Nintendo", {"sfc", "smc", "swc", "fig", "bs", "st"}, "snes9x_libretro", 0x9575CD, 1.4f, false, "snes"},
        {Sys::MD, "MD", "MD", "Mega Drive", {"md", "gen", "smd", "68k", "sgd", "bin"}, "genesis_plus_gx_libretro", 0xEF6C00, 0.72f, false, "md"},
        {Sys::SMS, "SMS", "SMS", "Master System", {"sms", "sg"}, "genesis_plus_gx_libretro", 0x00ACC1, 0.72f, false, "sms"},
        {Sys::GG, "GG", "GG", "Game Gear", {"gg"}, "genesis_plus_gx_libretro", 0x7CB342, 0.72f, false, "gg"},
        {Sys::PS1, "PS1", "PS1", "PlayStation", {"cue", "chd", "pbp", "m3u", "ccd"}, "pcsx_rearmed_libretro", 0x78909C, 1.0f, true, "ps1"},
    };
    return all;
}

const SystemInfo& Info(Sys s) { return Systems()[static_cast<size_t>(s)]; }

const SystemInfo* SystemByKey(const std::string& key) {
    for (const auto& s : Systems())
        if (key == s.key)
            return &s;
    return nullptr;
}

const SystemInfo* SystemForFile(const std::string& name, uint64_t size) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return nullptr;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == "bin" && size > 8ull * 1024 * 1024)
        return nullptr;
    for (const auto& s : Systems())
        for (const auto& e : s.extensions)
            if (e == ext)
                return &s;
    return nullptr;
}

std::string CorePath(const SystemInfo& s) {
#ifdef _WIN32
    return JoinPath(Paths::Get().cores, std::string(s.core) + ".dll");
#else
    return JoinPath(Paths::Get().cores, std::string(s.core) + ".so");
#endif
}

}  // namespace cartucho
