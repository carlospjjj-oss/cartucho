// Cartucho PC - where things live, and small file helpers (UTF-8 paths).
//
// The data folder mirrors the Android app's: saves/<SYS>, states/<SYS>,
// system/ (BIOS), so saves move between phone and PC unchanged.
//   Windows: %APPDATA%\Cartucho   (or .\data next to the program when a
//            file "portable.txt" is there)
//   Linux:   ~/.local/share/cartucho
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cartucho {

struct Paths {
    std::string exe_dir;   // the program's folder
    std::string data;      // the user's data
    std::string cores;     // emulator cores (exe_dir/cores)
    std::string assets;    // exe_dir/assets
    std::string cache;     // data/cache

    static const Paths& Get();
    static void Init(const char* argv0);
};

std::string JoinPath(const std::string& a, const std::string& b);
std::string FileName(const std::string& path);
std::string ParentDir(const std::string& path);
bool FileExists(const std::string& path);
bool IsDir(const std::string& path);
uint64_t FileSize(const std::string& path);
int64_t FileTime(const std::string& path);   // modification, ms since epoch
bool MakeDirs(const std::string& dir);
bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out);
bool ReadText(const std::string& path, std::string* out);
bool WriteFileBytes(const std::string& path, const void* data, size_t size);  // atomic
bool WriteText(const std::string& path, const std::string& text);
bool RemoveFile(const std::string& path);
bool RenameFile(const std::string& from, const std::string& to);

struct DirEntry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};
std::vector<DirEntry> ListDir(const std::string& dir);

int64_t NowMs();
std::string Lower(std::string s);

}  // namespace cartucho
