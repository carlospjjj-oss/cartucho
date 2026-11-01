// Cartucho PC - the game screens' shared state (game_ui.cpp, game_menus.cpp).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cheats.h"
#include "discovery.h"
#include "game_ui.h"
#include "gfx.h"
#include "video.h"

namespace cartucho {

class Recorder;

struct MenuItem {
    std::string label, value;
    std::function<void()> act;   // null: a line of text, not selectable
};

struct GameUi::Impl {
    Texture photo;                 // the console, for the frame's side
    Texture slot_pics[7];          // 0 = quick slot, 1..6
    bool slot_used[7] = {};
    std::string slot_label[7];
    double started = 0;

    // Options of this game: the one being chosen.
    std::string pick_key;

    // Cheats.
    std::string cheat_file;
    std::vector<Cheat> cheats;
    std::vector<CheatDb::Source> sources;
    struct Pickable { CheatDb::Entry e; bool chosen = false; bool have = false; };
    std::vector<Pickable> entries;
    std::thread cheat_thread;
    std::atomic<int> cheat_state{0};   // 0 idle, 1 loading, 2 loaded, 3 failed
    std::mutex cheat_mutex;
    std::vector<CheatDb::Entry> cheat_loaded;
    std::string cheat_error;
    char name_buf[128] = {};
    char code_buf[4096] = {};

    // Online play and link cable.
    std::unique_ptr<Discovery::Advertiser> adv;
    std::unique_ptr<Discovery::Browser> browser;
    bool address_for_link = false;
    char addr_buf[256] = {};
    int link_mode = 0, link_players = 2;
    bool internet = false;
    int waiting = 0;               // 0 nothing, 1 online play, 2 link: shown on screen
    std::string wait_intro;
    std::mutex pub_mutex;
    std::string public_ip;
    std::thread pub_thread;
    bool net_closed_said = false;

    // Video.
    std::shared_ptr<Recorder> rec;
    VideoWriter writer;

    ~Impl() {
        if (cheat_thread.joinable()) cheat_thread.join();
        if (pub_thread.joinable()) pub_thread.join();
    }
};

// Strips emoji the fonts do not have (the engine's messages use a few).
std::string PlainText(const std::string& s);

}  // namespace cartucho
