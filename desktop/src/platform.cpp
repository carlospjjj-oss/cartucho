// The engine's platform hooks on PC: log to stderr and data/cartucho.log,
// OpenGL ES functions from SDL.
#include <SDL.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "paths.h"
#include "platform.h"

namespace cartucho {

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}  // namespace

void OpenLog(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log)
        return;
    const std::string path = JoinPath(Paths::Get().data, name);
    g_log = std::fopen(path.c_str(), "w");
}

void PlatformLog(int level, const char* text) {
    static const char* names[] = {"D", "I", "W", "E"};
    const char* tag = (level >= 0 && level <= 3) ? names[level] : "?";
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fprintf(stderr, "[%s] %s\n", tag, text);
    if (g_log) {
        std::fprintf(g_log, "[%s] %s\n", tag, text);
        std::fflush(g_log);
    }
}

retro_proc_address_t PlatformGetProcAddress(const char* sym) {
    return reinterpret_cast<retro_proc_address_t>(SDL_GL_GetProcAddress(sym));
}

}  // namespace cartucho
