// Cartucho PC - cheat codes: the game's own list (data/cheats/<SYS>/<game>.json,
// the same file as on the phone) and ready-made cheats from the libretro
// database (names in assets/cheats, files downloaded when chosen).
#pragma once

#include <string>
#include <vector>

#include "systems.h"

namespace cartucho {

struct Cheat {
    std::string name, code;   // code: one per line
    bool enabled = true;
};

std::string CheatFile(const std::string& system_key, const std::string& save_name);
std::vector<Cheat> LoadCheatList(const std::string& file);
void SaveCheatList(const std::string& file, const std::vector<Cheat>& list);

namespace CheatDb {

struct Source {
    std::string folder, name;
    std::string Label() const;   // "GameShark", "Padrão (Game Boy)"...
};
struct Entry {
    std::string name, code;
};

bool Supported(Sys sys);
std::vector<Source> Find(Sys sys, const std::string& file_name);
// Downloads (or reads the kept copy of) one file. Blocking.
bool Load(const Source& s, std::vector<Entry>* out, std::string* error);
std::vector<Entry> Parse(const std::string& text);

}  // namespace CheatDb

}  // namespace cartucho
