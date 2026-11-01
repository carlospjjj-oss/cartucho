// Cartucho PC - operating system bits: starting the game process, picking
// a folder or file, opening a web page or folder.
#pragma once

#include <string>
#include <vector>

namespace cartucho {

struct Process;
// Starts this program again with these arguments; null on failure.
Process* StartSelf(const std::vector<std::string>& args);
bool ProcessRunning(Process* p);      // false once it ended
int ProcessExitCode(Process* p);
void ProcessFree(Process* p);

// Native folder picker (Windows); "" when cancelled or not available.
std::string PickFolder(const std::string& title);
std::string PickFile(const std::string& title, const std::string& filter_desc, const std::string& patterns /* "*.bin;*.rom" */);
// Where to save a new file (Windows dialog); "" when cancelled.
std::string PickSaveFile(const std::string& title, const std::string& default_name, const std::string& filter_desc,
                         const std::string& patterns);
void OpenUrl(const std::string& url);   // browser or file manager
// The user's Videos folder (Windows), else data/videos.
std::string VideosFolder();
// Runs a command without waiting (the updater's helper script).
bool StartDetached(const std::string& program, const std::vector<std::string>& args);

}  // namespace cartucho
