#include "paths.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace cartucho {

namespace {
Paths g_paths;

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) { return p.u8string(); }
}  // namespace

const Paths& Paths::Get() { return g_paths; }

void Paths::Init(const char*) {
    char* base = SDL_GetBasePath();
    g_paths.exe_dir = base ? base : "./";
    SDL_free(base);
    while (g_paths.exe_dir.size() > 1 && (g_paths.exe_dir.back() == '/' || g_paths.exe_dir.back() == '\\'))
        g_paths.exe_dir.pop_back();
    g_paths.cores = JoinPath(g_paths.exe_dir, "cores");
    g_paths.assets = JoinPath(g_paths.exe_dir, "assets");
    if (const char* env = std::getenv("CARTUCHO_DATA")) {
        g_paths.data = env;
    } else if (FileExists(JoinPath(g_paths.exe_dir, "portable.txt"))) {
        g_paths.data = JoinPath(g_paths.exe_dir, "data");
    } else {
#ifdef _WIN32
        char* pref = SDL_GetPrefPath("", "Cartucho");  // %APPDATA%\Cartucho
#else
        char* pref = SDL_GetPrefPath("", "cartucho");  // ~/.local/share/cartucho
#endif
        g_paths.data = pref ? pref : "data";
        SDL_free(pref);
        while (!g_paths.data.empty() && (g_paths.data.back() == '/' || g_paths.data.back() == '\\'))
            g_paths.data.pop_back();
    }
    g_paths.cache = JoinPath(g_paths.data, "cache");
    MakeDirs(g_paths.data);
    MakeDirs(g_paths.cache);
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    return S(P(a) / P(b));
}

std::string FileName(const std::string& path) { return S(P(path).filename()); }
std::string ParentDir(const std::string& path) { return S(P(path).parent_path()); }

bool FileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(P(path), ec);
}

bool IsDir(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(P(path), ec);
}

uint64_t FileSize(const std::string& path) {
    std::error_code ec;
    const auto n = fs::file_size(P(path), ec);
    return ec ? 0 : n;
}

int64_t FileTime(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(P(path), ec);
    if (ec)
        return 0;
    // file_clock -> system_clock, portable in C++17.
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

bool MakeDirs(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    return IsDir(dir);
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f)
        return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool ReadText(const std::string& path, std::string* out) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f)
        return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFileBytes(const std::string& path, const void* data, size_t size) {
    MakeDirs(ParentDir(path));
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(P(tmp), std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!f)
            return false;
    }
    return RenameFile(tmp, path);
}

bool WriteText(const std::string& path, const std::string& text) {
    return WriteFileBytes(path, text.data(), text.size());
}

bool RemoveFile(const std::string& path) {
    std::error_code ec;
    return fs::remove(P(path), ec);
}

bool RenameFile(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(P(from), P(to), ec);
    if (ec) {
        // Windows: rename over an existing file can fail; replace it.
        fs::remove(P(to), ec);
        fs::rename(P(from), P(to), ec);
    }
    return !ec;
}

std::vector<DirEntry> ListDir(const std::string& dir) {
    std::vector<DirEntry> out;
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        DirEntry e;
        e.name = S(it->path().filename());
        std::error_code ec2;
        e.dir = it->is_directory(ec2);
        if (!e.dir)
            e.size = it->file_size(ec2);
        out.push_back(std::move(e));
    }
    return out;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace cartucho
