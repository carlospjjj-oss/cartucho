// Cartucho PC - a window with an OpenGL ES 3.0 context, and Dear ImGui set
// up with the app's fonts and colours.
#pragma once

#include <SDL.h>

#include <string>

struct ImFont;

namespace cartucho {

struct Fonts {
    ImFont* body = nullptr;    // 18 px
    ImFont* small = nullptr;   // 14 px
    ImFont* bold = nullptr;    // 20 px bold
    ImFont* title = nullptr;   // 30 px bold
    ImFont* huge = nullptr;    // 44 px bold
};

class Window {
public:
    bool Open(const std::string& title, bool fullscreen, std::string* error);
    void Close();
    SDL_Window* sdl() const { return window_; }
    void ToggleFullscreen();
    bool fullscreen() const;
    // Drawable size in pixels.
    void Size(int* w, int* h) const;
    float scale() const { return scale_; }   // UI scale for the display
    void BeginFrame();                       // ImGui new frame
    void EndFrame() { Render(); Swap(); }    // ImGui render + swap
    void Render();
    void Swap();
    const Fonts& fonts() const { return fonts_; }

private:
    void SetupImGui();
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    float scale_ = 1.0f;
    Fonts fonts_;
};

}  // namespace cartucho
