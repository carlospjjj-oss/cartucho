// Renders through GlPresenter in an offscreen EGL context (Mesa) and checks
// the hardware path with fake_hw_core: orientation, partial framebuffer and
// letterboxing. Also renders a software core with every filter into PNG-able
// PPMs for a look.
//
//   gl_test <fake_hw_core.so> [<gba core.so> <rom> <outdir>]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core_host.h"
#include "gl_presenter.h"
#include "platform.h"

namespace cartucho {
void PlatformLog(int level, const char* text) {
    if (level >= 2 || std::getenv("CARTUCHO_DEBUG"))
        std::fprintf(stderr, "[log %d] %s\n", level, text);
}
}  // namespace cartucho

using namespace cartucho;

static int failures = 0;
#define CHECK(cond, ...)                       \
    do {                                       \
        std::printf((cond) ? "ok    " : "FAIL  "); \
        std::printf(__VA_ARGS__);              \
        std::printf("\n");                     \
        if (!(cond)) failures++;               \
    } while (0)

static const int kW = 800, kH = 600;

// Where the pictures go: $OUT (set by run.sh) or the current directory.
static std::string OutDir() {
    const char* out = std::getenv("OUT");
    return out ? out : ".";
}

static bool MakeContext() {
    // Ask Mesa for its surfaceless platform explicitly: headless CI has no
    // window system, and the default display may pick one that is missing.
    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display)
        dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
        return false;
    const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n == 0)
        return false;
    const EGLint pb[] = {EGL_WIDTH, kW, EGL_HEIGHT, kH, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    return surf != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, surf, surf, ctx);
}

static std::vector<uint8_t> ReadScreen() {
    std::vector<uint8_t> px(kW * kH * 4);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

// Pixel at top-left based coordinates.
static const uint8_t* At(const std::vector<uint8_t>& px, int x, int y) {
    return &px[((kH - 1 - y) * kW + x) * 4];
}

static void WritePpm(const std::string& path, const std::vector<uint8_t>& px) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++)
            std::fwrite(At(px, x, y), 1, 3, f);
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!MakeContext()) {
        // Not this code's fault; the machine has no usable GLES driver.
        std::printf("skip  no EGL/GLES3 context available\n");
        return 0;
    }
    std::printf("      GL: %s\n", glGetString(GL_RENDERER));

    {
        GlPresenter presenter;
        CHECK(presenter.Init(), "presenter init");
        CoreHost host;
        host.SetHwHooks({[&] { return static_cast<uintptr_t>(presenter.hw_framebuffer()); },
                         [](const char* sym) {
                             return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(sym));
                         }});
        std::string err;
        CHECK(host.LoadCore(argv[1], &err), "load fake hw core %s", err.c_str());
        CHECK(host.LoadGame("x.fake", "", &err), "load game %s", err.c_str());
        CHECK(host.uses_hw_render(), "core uses hardware rendering");
        const auto& g = host.av_info().geometry;
        CHECK(presenter.CreateHwFramebuffer(g.max_width, g.max_height, true, true),
              "framebuffer %ux%u", g.max_width, g.max_height);
        host.ContextReset();
        host.RunFrame();
        presenter.set_filter(Filter::kSharp);
        presenter.Draw(host, kW, kH);
        auto px = ReadScreen();
        const Rect r = presenter.viewport();
        CHECK(r.w == 800 && r.h == 600, "4:3 fills 800x600 (%dx%d at %d,%d)", r.w, r.h, r.x, r.y);
        const uint8_t* top = At(px, kW / 2, kH / 4);
        const uint8_t* bottom = At(px, kW / 2, kH * 3 / 4);
        CHECK(top[0] > 200 && top[2] < 50, "top half red (%d,%d,%d)", top[0], top[1], top[2]);
        CHECK(bottom[2] > 200 && bottom[0] < 50, "bottom half blue (%d,%d,%d)", bottom[0],
              bottom[1], bottom[2]);
        WritePpm(OutDir() + "/hw.ppm", px);

        // The edge filters read neighbours in screen directions; a
        // bottom-up hardware frame must still come out the right way up.
        for (Filter f : {Filter::kScale2x, Filter::kScale3x}) {
            presenter.set_filter(f);
            presenter.Draw(host, kW, kH);
            auto p2 = ReadScreen();
            const uint8_t* t = At(p2, kW / 2, kH / 4);
            const uint8_t* b = At(p2, kW / 2, kH * 3 / 4);
            CHECK(t[0] > 200 && b[2] > 200, "edge filter %d keeps orientation", static_cast<int>(f));
        }

        // Portrait surface, picture stuck to the top with room for controls.
        presenter.set_vertical_align(1);
        presenter.set_bottom_reserve(0.4f);
        glViewport(0, 0, kW, kH);
        presenter.Draw(host, 400, 600);
        const Rect p = presenter.viewport();
        CHECK(p.x == 0 && p.y == 0 && p.w == 400 && p.h == 300, "portrait top-aligned %dx%d at %d,%d",
              p.w, p.h, p.x, p.y);
    }

    if (argc >= 5) {
        GlPresenter presenter;
        presenter.Init();
        CoreHost host;
        std::string err;
        CHECK(host.LoadCore(argv[2], &err) && host.LoadGame(argv[3], "", &err), "gba core %s",
              err.c_str());
        for (int i = 0; i < 200; i++)
            host.RunFrame();
        const char* names[] = {"sharp", "smooth", "sharpsmooth", "lcd", "scanlines", "scale2x", "scale3x", "crt", "crtcurved", "composite"};
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Filter::kCount), "a name per filter");
        for (int f = 0; f < static_cast<int>(Filter::kCount); f++) {
            presenter.set_filter(static_cast<Filter>(f));
            presenter.Draw(host, kW, kH);
            auto px = ReadScreen();
            WritePpm(std::string(argv[4]) + "/filter-" + names[f] + ".ppm", px);
            const Rect r = presenter.viewport();
            CHECK(glGetError() == GL_NO_ERROR, "filter %s drawn at %dx%d", names[f], r.w, r.h);
        }
        presenter.set_integer_scale(true);
        presenter.Draw(host, kW, kH);
        const Rect r = presenter.viewport();
        CHECK(r.w == 720 && r.h == 480, "integer scale 3x = %dx%d", r.w, r.h);
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
