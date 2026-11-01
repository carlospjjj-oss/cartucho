// Cartucho - the few POSIX calls the engine uses, also on Windows.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <string>

namespace cartucho {

// UTF-8 path -> wide string for the Windows API.
inline std::wstring Widen(const std::string& s) {
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline void* LibOpen(const std::string& path) { return LoadLibraryW(Widen(path).c_str()); }
inline void* LibSym(void* lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
}
inline std::string LibError() {
    return "Windows error " + std::to_string(GetLastError());
}
inline int MakeDir(const std::string& path) { return _wmkdir(Widen(path).c_str()); }
inline long long PRead(int fd, void* buf, size_t n, long long offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0)
        return -1;
    return _read(fd, buf, static_cast<unsigned>(n));
}

}  // namespace cartucho

#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

namespace cartucho {

inline void* LibOpen(const std::string& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
inline void* LibSym(void* lib, const char* name) { return dlsym(lib, name); }
inline std::string LibError() {
    const char* e = dlerror();
    return e ? e : "unknown error";
}
inline int MakeDir(const std::string& path) { return mkdir(path.c_str(), 0755); }
inline long long PRead(int fd, void* buf, size_t n, long long offset) {
    return pread(fd, buf, n, static_cast<off_t>(offset));
}

}  // namespace cartucho
#endif
