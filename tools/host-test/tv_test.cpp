// TV mode in miniature: two GL contexts sharing objects, like the phone's and
// the TV's. A hardware core (fake_hw_core: red top half, blue bottom half,
// like a DS/3DS frame with two screens) renders in the "phone" context; the
// "TV" context draws the shared picture. With crops, the TV must show only the
// top screen (red) and the phone only the bottom screen (blue).
//   tv_test <fake_hw_core.so>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core_host.h"
#include "gl_presenter.h"

namespace cartucho {
void PlatformLog(int level, const char* text) {
    if (level >= 2) std::fprintf(stderr, "[log] %s\n", text);
}
}  // namespace cartucho

using namespace cartucho;

static int failures = 0;
#define CHECK(cond, ...) do { std::printf((cond) ? "ok    " : "FAIL  "); std::printf(__VA_ARGS__); \
    std::printf("\n"); if (!(cond)) failures++; } while (0)

static EGLDisplay dpy;

static EGLDisplay OpenDisplay() {
    auto get = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay d = get ? get(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
    if (d == EGL_NO_DISPLAY) d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return eglInitialize(d, nullptr, nullptr) ? d : EGL_NO_DISPLAY;
}

struct Ctx {
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    int w = 0, h = 0;
    bool Make(EGLConfig cfg, EGLContext share, int width, int height) {
        const EGLint pb[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        surf = eglCreatePbufferSurface(dpy, cfg, pb);
        const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        ctx = eglCreateContext(dpy, cfg, share, attrs);
        w = width;
        h = height;
        return surf != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT;
    }
    void Current() { eglMakeCurrent(dpy, surf, surf, ctx); }
    std::vector<uint8_t> Read() {
        std::vector<uint8_t> px(w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        return px;
    }
    // Share of drawn pixels that are mostly red / mostly blue.
    void Count(const std::vector<uint8_t>& px, int* red, int* blue, int* lit) {
        *red = *blue = *lit = 0;
        for (size_t i = 0; i < px.size(); i += 4) {
            if (px[i] > 200 && px[i + 2] < 60) (*red)++;
            if (px[i + 2] > 200 && px[i] < 60) (*blue)++;
            if (px[i] + px[i + 1] + px[i + 2] > 60) (*lit)++;
        }
    }
};

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    dpy = OpenDisplay();
    if (dpy == EGL_NO_DISPLAY) { std::printf("skip  no EGL\n"); return 0; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n == 0) { std::printf("skip  no config\n"); return 0; }

    Ctx phone, tv;
    CHECK(phone.Make(cfg, EGL_NO_CONTEXT, 400, 700), "phone context");
    CHECK(tv.Make(cfg, phone.ctx, 1280, 720), "TV context shares the phone's objects");

    phone.Current();
    GlPresenter phone_p;
    phone_p.Init();
    CoreHost host;
    host.SetHwHooks({[&] { return static_cast<uintptr_t>(phone_p.hw_framebuffer()); },
                     [](const char* s) { return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(s)); }});
    std::string err;
    CHECK(host.LoadCore(argv[1], &err) && host.LoadGame("x.fake", "", &err), "hardware core loaded %s", err.c_str());
    const auto& g = host.av_info().geometry;
    phone_p.CreateHwFramebuffer(g.max_width, g.max_height, true, true);
    host.ContextReset();
    host.RunFrame();

    // Phone: bottom screen only (blue).
    phone_p.set_crop(Crop{0.f, 0.5f, 1.f, 1.f});
    const FrameSource src = phone_p.Prepare(host);
    phone_p.DrawSource(src, phone.w, phone.h);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    int red, blue, lit;
    phone.Count(phone.Read(), &red, &blue, &lit);
    CHECK(blue > 0 && red == 0, "phone shows only the bottom screen (%d blue, %d red px)", blue, red);
    const Rect pv = phone_p.viewport();
    CHECK(pv.w == 400 && pv.h == 150, "bottom screen keeps its 320x120 shape: %dx%d", pv.w, pv.h);

    // TV: other context, same texture, top screen only (red).
    tv.Current();
    GlPresenter tv_p;
    CHECK(tv_p.Init(), "TV presenter in the shared context");
    glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
    tv_p.set_crop(Crop{0.f, 0.f, 1.f, 0.5f});
    tv_p.DrawSource(src, tv.w, tv.h);
    tv.Count(tv.Read(), &red, &blue, &lit);
    CHECK(glGetError() == GL_NO_ERROR, "TV drew the phone's texture without GL errors");
    CHECK(red > 0 && blue == 0, "TV shows only the top screen (%d red, %d blue px)", red, blue);
    const Rect tvv = tv_p.viewport();
    CHECK(tvv.w == 1280 && tvv.h == 480 && tvv.y == 120,
          "top screen fitted to the TV: %dx%d at %d,%d", tvv.w, tvv.h, tvv.x, tvv.y);

    // Whole picture on the TV (GB/GBA in TV mode).
    tv_p.set_crop(Crop{});
    tv_p.DrawSource(src, tv.w, tv.h);
    tv.Count(tv.Read(), &red, &blue, &lit);
    CHECK(red > 0 && blue > 0, "whole picture on the TV (%d red, %d blue px)", red, blue);

    // Phone with no picture (GB/GBA in TV mode): just black.
    phone.Current();
    phone_p.set_show_picture(false);
    phone_p.DrawSource(src, phone.w, phone.h);
    phone.Count(phone.Read(), &red, &blue, &lit);
    CHECK(lit == 0, "phone without picture is black");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
