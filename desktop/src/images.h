// Cartucho PC - pictures loaded in the background (covers, photos, slot
// pictures): decoded on a worker thread, uploaded as textures on the UI
// thread. Also fetches covers and game pictures from the internet.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gfx.h"
#include "library.h"

namespace cartucho {

class Images {
public:
    Images();
    ~Images();
    // The texture of a file, or an invalid one while it loads (or if it
    // cannot). `max_side` limits the decoded size.
    const Texture& Get(const std::string& path, int max_side = 512);
    void Forget(const std::string& path);
    // UI thread, once per frame: uploads what finished decoding.
    void Update();

    // Covers: the known path now, and a background fetch when missing.
    std::string Cover(const Game& g);
    // Screenshot and title screen for the slideshow ("" until fetched).
    std::vector<std::string> Pictures(const Game& g);

private:
    struct Job { std::string path; int max_side; };
    struct Done { std::string path; std::vector<uint8_t> rgba; int w, h; };
    void DecodeLoop();
    void FetchLoop();
    std::map<std::string, Texture> tex_;
    std::map<std::string, bool> pending_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::deque<Done> done_;
    bool stop_ = false;
    std::thread decoder_;

    // Covers and pictures: game path -> found file ("" = none).
    std::map<std::string, std::string> covers_;
    std::map<std::string, std::vector<std::string>> pictures_;
    std::deque<std::pair<Game, int>> fetch_;   // 0 cover, 1 pictures
    std::mutex fetch_mutex_;
    std::condition_variable fetch_cv_;
    std::thread fetcher_;
    Texture none_;
};

}  // namespace cartucho
