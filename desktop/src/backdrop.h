// Cartucho PC - the console look's background: a dark base with soft glows
// drifting slowly, tinted with the selected console's colour (as
// Backdrop.kt / AnimatedBackground on Android), drawn with one shader.
#pragma once

#include <cstdint>

namespace cartucho {

class Backdrop {
public:
    ~Backdrop();
    void Draw(int w, int h, uint32_t tint_rgb, float seconds);
    // Fades to a new tint over 0.6 s.
    void SetTint(uint32_t rgb, float now);
    uint32_t Current(float now) const;

private:
    unsigned program_ = 0, vao_ = 0, vbo_ = 0;
    int u_tint_ = -1, u_time_ = -1, u_size_ = -1;
    uint32_t from_ = 0x6B4EFF, to_ = 0x6B4EFF;
    float since_ = -10.f;
};

}  // namespace cartucho
