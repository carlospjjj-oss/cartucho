#include "testhook.h"

#include <glad/gles2.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gfx.h"

namespace cartucho {

TestHook::TestHook() {
    shot_ = std::getenv("CARTUCHO_SHOT");
    if (const char* f = std::getenv("CARTUCHO_SHOT_FRAME"))
        shot_frame_ = std::atoi(f);
    keys_ = std::getenv("CARTUCHO_KEYS");
}

void TestHook::BeforeFrame() {
    frame_++;
    if (!keys_)
        return;
    // "120:Return,130:Right,140:Escape" ; a trailing + holds for 10 frames.
    std::string all = keys_;
    size_t at = 0;
    while (at < all.size()) {
        size_t comma = all.find(',', at);
        if (comma == std::string::npos) comma = all.size();
        const std::string item = all.substr(at, comma - at);
        at = comma + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        const int f = std::atoi(item.c_str());
        std::string name = item.substr(colon + 1);
        const int hold = (!name.empty() && name.back() == '+') ? 10 : 2;
        if (!name.empty() && name.back() == '+') name.pop_back();
        const SDL_Keycode key = SDL_GetKeyFromName(name.c_str());
        if (key == SDLK_UNKNOWN) continue;
        if (frame_ != f && frame_ != f + hold) continue;
        SDL_Event e{};
        e.type = frame_ == f ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.state = frame_ == f ? SDL_PRESSED : SDL_RELEASED;
        e.key.keysym.sym = key;
        e.key.keysym.scancode = SDL_GetScancodeFromKey(key);
        if (SDL_Window* w = SDL_GL_GetCurrentWindow()) e.key.windowID = SDL_GetWindowID(w);
        SDL_PushEvent(&e);
    }
}

bool TestHook::AfterDraw(int w, int h) {
    if (!shot_ || shot_frame_ < 0 || frame_ < shot_frame_)
        return false;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4), flip(px.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++)
        std::memcpy(&flip[static_cast<size_t>(y) * w * 4], &px[static_cast<size_t>(h - 1 - y) * w * 4], static_cast<size_t>(w) * 4);
    WritePng(shot_, flip.data(), w, h);
    return true;
}

}  // namespace cartucho
