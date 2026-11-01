// Cartucho PC - automated screenshots for tests: CARTUCHO_SHOT=<file.png>
// and CARTUCHO_SHOT_FRAME=<n> save the window after n frames and quit;
// CARTUCHO_KEYS="<frame>:<key>[+],..." presses keys at given frames.
#pragma once

#include <SDL.h>

namespace cartucho {

class TestHook {
public:
    TestHook();
    // Call once per frame before drawing: injects scripted keys.
    void BeforeFrame();
    // After drawing, before swap. Returns true when the program should end.
    bool AfterDraw(int w, int h);

private:
    int frame_ = 0;
    int shot_frame_ = -1;
    const char* shot_ = nullptr;
    const char* keys_ = nullptr;
};

}  // namespace cartucho
