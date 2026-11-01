// Cartucho PC - the games: folders to look in, scanning, favourites, the
// facts about each game (libretro database) and box art (libretro
// thumbnails, the player's own pictures next to the game).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "systems.h"

namespace cartucho {

struct Game {
    std::string path;        // the file (the archive, for zipped games)
    std::string file_name;   // the game's own name (inside the archive)
    std::string entry;       // path inside a .zip/.7z, or "" for a plain file
    Sys sys = Sys::GBA;
    uint64_t size = 0;
    std::string title;       // without tags: "Pokemon - FireRed Version"
    std::string save_name;   // saves/states name (same rule as Android)
    std::string cover;       // a picture found next to the game, or ""
};

std::string TitleOf(const std::string& file_name);
std::string SaveNameOf(const std::string& file_name);

struct Facts {
    std::string name, year, genre, developer, publisher, players;
};

class Library {
public:
    static Library& Get();
    void Load();                                  // cached list
    std::vector<Game> games() const;
    std::vector<std::string> folders() const;
    void AddFolder(const std::string& dir);
    void RemoveFolder(const std::string& dir);
    // Walks the folders (a few levels deep). Slow: call off the UI thread.
    void Scan();
    bool scanning() const { return scanning_; }

    bool IsFavorite(const Game& g) const;
    void SetFavorite(const Game& g, bool on);
    int64_t LastPlayed(const Game& g) const;
    int64_t PlayTime(const Game& g) const;

    // The libretro database's facts (assets/meta).
    const Facts* FactsFor(const Game& g);

private:
    void Save();
    mutable std::mutex mutex_;
    std::vector<Game> games_;
    std::vector<std::string> folders_;
    std::map<std::string, bool> favorites_;
    std::atomic<bool> scanning_{false};
    std::map<std::string, std::map<std::string, Facts>> meta_;   // tsv file -> key -> facts
    std::mutex meta_mutex_;
};

// Box art: downloaded once into data/cache/covers. Thread-safe.
class Covers {
public:
    static Covers& Get();
    // Path of the cover image if known now (no download).
    std::string Cached(const Game& g);
    // Finds or downloads it (blocking). "" when there is none.
    std::string Fetch(const Game& g);
    // Pictures for the slideshow: screenshot and title screen (libretro).
    std::string FetchKind(const Game& g, const char* folder /* Named_Snaps, Named_Titles */);
    // Name of the game in the libretro collection, for other lookups.
    std::string LibretroName(const Game& g, std::string* repo);
    void ForgetMisses();

private:
    std::vector<std::string> Index(const std::string& repo);
    std::mutex mutex_;
    std::map<std::string, std::vector<std::string>> indexes_;
};

// Words of a title without tags, accents, punctuation and filler words.
std::vector<std::string> Tokens(const std::string& name);
// The best matching name of a list for a game file (Covers.bestMatch).
std::string BestMatch(const std::string& file_name, const std::vector<std::string>& names);

}  // namespace cartucho
