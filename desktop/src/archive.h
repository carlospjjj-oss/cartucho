// Cartucho PC - games inside .zip and .7z files (as Archives.kt on
// Android), and writing/reading the backup .zip shared with the phone.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cartucho {

bool IsArchive(const std::string& file_name);

struct ArchiveEntry {
    std::string name;   // path inside the archive
    uint64_t size = 0;
};

// The first game in the archive (looks at a few entries), or false.
bool PeekArchive(const std::string& archive, ArchiveEntry* out);

// Extracts `entry` to `target` (written atomically). `progress` gets 0..1.
bool ExtractEntry(const std::string& archive, const std::string& entry, const std::string& target,
                  const std::function<void(float)>& progress, std::string* error);

// Where a game from an archive is extracted to (kept while the archive does
// not change; only the latest extraction is kept).
std::string ExtractedPath(const std::string& archive, uint64_t archive_size, const std::string& entry);

// A whole .zip written from a list of (name in zip, file on disk), plus
// small text entries.
struct ZipItem {
    std::string name;
    std::string file;   // read from disk, or...
    std::string text;   // ...this, when file is empty
};
bool WriteZip(const std::string& path, const std::vector<ZipItem>& items, std::string* error);

// Every entry of a zip, read one at a time; `fn` gets the name and bytes and
// returns false to stop.
bool ReadZip(const std::string& path, const std::function<bool(const std::string&, const std::vector<uint8_t>&)>& fn,
             std::string* error);
bool ZipHasEntry(const std::string& path, const std::string& name);

}  // namespace cartucho
