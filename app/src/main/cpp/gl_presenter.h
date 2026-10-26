// Cartucho - draws the core's picture on screen with OpenGL ES 3.
//
// Software-rendered cores (GBA) hand over pixels, which are uploaded to a
// texture. Hardware-rendered cores (3DS) draw into a framebuffer object owned
// here (CreateHwFramebuffer), which is then sampled the same way. Either way
// the picture is fitted into the surface keeping its aspect ratio and drawn
// through one of the display filters.
#pragma once

#include <GLES3/gl3.h>

#include <cstdint>
#include <vector>

#include "core_host.h"

namespace cartucho {

enum class Filter : int {
    kSharp = 0,         // nearest neighbour: crisp pixels
    kSmooth = 1,        // bilinear
    kSharpSmooth = 2,   // sharp bilinear: crisp, but even at odd scales
    kLcd = 3,           // handheld LCD pixel grid
    kScanlines = 4,     // CRT-like scanlines
    kScale2x = 5,       // EPX / Scale2x: rounds diagonal edges, stays crisp
    kScale3x = 6,       // Scale3x (AdvMAME3x): the same at 3x, smoother
    kCount
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;  // top-left origin, surface pixels
};

// Part of the core's picture to show, as fractions of it (top-left origin):
// all of it, or one screen of the DS / 3DS.
struct Crop {
    float x0 = 0.f, y0 = 0.f, x1 = 1.f, y1 = 1.f;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

// Where the picture lives, so another GL context sharing objects with this
// one (the TV) can draw it too.
struct FrameSource {
    GLuint texture = 0;
    unsigned frame_w = 0, frame_h = 0;  // picture size in texels
    float uv_w = 1.f, uv_h = 1.f;       // part of the texture holding it
    bool flip = false;                  // stored bottom-up
    float aspect = 4.f / 3.f;           // display aspect of the whole picture
    bool valid() const { return texture != 0 && frame_w && frame_h; }
};

class GlPresenter {
public:
    GlPresenter() = default;
    ~GlPresenter();
    GlPresenter(const GlPresenter&) = delete;
    GlPresenter& operator=(const GlPresenter&) = delete;

    // Needs a current GLES 3 context. Safe to call again after the context
    // was lost (old object names are simply forgotten).
    bool Init();

    // Framebuffer the hardware core renders into, sized for its largest
    // frame. Recreated if the size changes.
    bool CreateHwFramebuffer(unsigned width, unsigned height, bool depth, bool stencil);
    GLuint hw_framebuffer() const { return hw_fbo_; }
    // Reads the hardware core's last picture (top row first).
    bool ReadHwFrame(unsigned width, unsigned height, bool bottom_up, std::vector<uint8_t>* rgba);

    void set_filter(Filter f) { filter_ = f; }
    Filter filter() const { return filter_; }
    void set_integer_scale(bool on) { integer_scale_ = on; }
    // 0 = centre the picture vertically, 1 = stick it to the top (portrait,
    // leaving the lower half to the on-screen controls).
    void set_vertical_align(int align) { vertical_align_ = align; }
    // Fraction of the surface height reserved at the bottom (portrait
    // controls); the picture is fitted into the rest.
    void set_bottom_reserve(float fraction) { bottom_reserve_ = fraction; }

    // Draws the latest frame. Call every vsync even without a new frame.
    void Draw(const CoreHost& host, int surface_width, int surface_height);
    // Uploads a new software frame if any and says where the picture is.
    FrameSource Prepare(const CoreHost& host);
    // Draws `crop` of `source` (a texture of this or a shared context);
    // `show_picture` false just clears (the phone in TV mode, GB/GBA).
    void DrawSource(const FrameSource& source, int surface_width, int surface_height);

    // Which part of the picture this presenter shows.
    void set_crop(const Crop& crop) { crop_ = crop; }
    const Crop& crop() const { return crop_; }
    void set_show_picture(bool on) { show_picture_ = on; }

    // Where the picture went on the last Draw(), for mapping touches.
    Rect viewport() const { return viewport_; }

private:
    void UploadSoftware(const VideoFrame& frame);
    Rect FitRect(float aspect, unsigned src_w, unsigned src_h, int sw, int sh) const;

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLint u_mode_ = -1, u_src_size_ = -1, u_out_size_ = -1, u_uv_scale_ = -1, u_flip_ = -1;
    GLint u_crop_ = -1;
    Crop crop_;
    bool show_picture_ = true;
    GLuint sw_texture_ = 0;
    unsigned sw_w_ = 0, sw_h_ = 0;
    uint64_t uploaded_serial_ = 0;

    GLuint hw_fbo_ = 0, hw_color_ = 0, hw_depth_ = 0;
    unsigned hw_w_ = 0, hw_h_ = 0;

    Filter filter_ = Filter::kSharpSmooth;
    bool integer_scale_ = false;
    int vertical_align_ = 0;
    float bottom_reserve_ = 0.0f;
    Rect viewport_;
};

}  // namespace cartucho
