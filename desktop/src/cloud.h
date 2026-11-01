// Cartucho PC - the saves backup (one .zip, the same format as the phone's
// Backup.kt) and "Saves na nuvem": a backup file the player keeps in a
// synced folder (Google Drive, OneDrive, Dropbox...), rewritten after
// playing. The phone picks the same file in its own settings, so both share
// saves, states and cheats.
#pragma once

#include <cstdint>
#include <string>

namespace cartucho {

namespace Backup {

struct Result {
    bool ok = false;
    int files = 0;
    uint64_t bytes = 0;
    std::string message;
};

// light: the cloud copy (no 3DS states, only small BIOS files).
Result Export(const std::string& zip, bool light);
Result Restore(const std::string& zip);
int64_t LastChange();   // newest change to anything a backup holds (ms)
std::string DefaultName();

}  // namespace Backup

namespace Cloud {

std::string File();       // "" when not set
void SetFile(const std::string& path);
bool Auto();
int64_t LastSent();
Backup::Result Send();
// After playing: sends if anything changed since the last copy.
void SendIfChanged();
Backup::Result Fetch();           // restores from the cloud file

}  // namespace Cloud

}  // namespace cartucho
