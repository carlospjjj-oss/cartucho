// Cartucho PC - updates from the "pc" release on GitHub (the zip CI
// publishes): checks for a newer build, downloads it, and hands over to a
// small script that waits for the program to close, copies the new files
// over and starts it again.
#pragma once

#include <string>

namespace cartucho {

namespace Updater {

struct Info {
    bool ok = false;          // the check worked
    bool newer = false;
    int build = 0;            // build number of the release
    int current = 0;          // this program's
    std::string url, message;
};

int CurrentBuild();
Info Check();                 // blocking
// Blocking: downloads and prepares; true when the program should now quit
// (the helper takes over).
bool Install(const Info& info, std::string* message);

}  // namespace Updater

}  // namespace cartucho
