// The whole Session as the app drives it (pacer, rewind, run-ahead, drawing),
// in an offscreen GLES context: with run-ahead on, the screen must show the
// game, not a blank picture.
//   session_test <core.so> <rom> <run-ahead frames>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "session.h"

namespace cartucho {
void PlatformLog(int level, const char* t) { if (level >= 2) std::fprintf(stderr, "[log] %s\n", t); }
retro_proc_address_t PlatformGetProcAddress(const char* s) {
    return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(s));
}
}  // namespace cartucho

using namespace cartucho;

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    auto get = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay dpy = get(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    eglInitialize(dpy, nullptr, nullptr);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n;
    eglChooseConfig(dpy, ca, &cfg, 1, &n);
    const EGLint pb[] = {EGL_WIDTH, 720, EGL_HEIGHT, 480, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    const EGLint xa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(dpy, surf, surf, ctx);

    Session s;
    std::string err;
    if (!s.Open(argv[1], "/tmp", "/tmp", &err) || !s.OnSurfaceCreated(argv[2], "", &err)) {
        std::printf("FAIL  %s\n", err.c_str());
        return 1;
    }
    s.OnSurfaceChanged(720, 480);
    s.set_run_ahead(std::atoi(argv[3]));
    std::set<uint32_t> colours;
    double t = 0;
    s.set_rewind_enabled(true);
    for (int i = 0; i < 600; i++, t += 1.0 / 60) {
        s.host().SetButtons((i % 40) < 3 ? (1u << RETRO_DEVICE_ID_JOYPAD_DOWN) : 0);
        // Hold rewind for a while, twice, like a player would.
        s.set_rewinding((i >= 250 && i < 330) || (i >= 450 && i < 470));
        s.Frame(t);
    }
    std::vector<uint8_t> px(720 * 480 * 4);
    glReadPixels(0, 0, 720, 480, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (size_t i = 0; i < px.size(); i += 4) colours.insert(px[i] | px[i + 1] << 8 | px[i + 2] << 16);
    const auto& f = s.host().frame();
    size_t white = 0;
    for (size_t i = 0; i < f.rgba.size(); i += 4) white += f.rgba[i] > 240 && f.rgba[i + 1] > 240 && f.rgba[i + 2] > 240;
    std::printf("run-ahead %s: %zu colours on screen, frame %ux%u, %zu%% white, %.1f fps\n", argv[3],
                colours.size(), f.width, f.height, f.rgba.empty() ? 0 : white * 100 / (f.rgba.size() / 4), s.measured_fps());
    return 0;
}
