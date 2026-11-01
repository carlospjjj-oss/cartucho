// Cartucho PC - RetroAchievements: the account (as RetroAchievements.kt on
// the phone; the password is used once for a token, never stored) and the
// HTTP side of the engine's rcheevos client.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "systems.h"

namespace cartucho {

class Achievements;

namespace RA {

std::string User();
bool SignedIn();
bool Enabled();            // signed in and switched on
int Score();
// Blocking. "" on success, else the message to show.
std::string SignIn(const std::string& user, const std::string& password);
void SignOut();
// RC_CONSOLE_* id, 0 when the console has no achievements here.
int ConsoleId(Sys sys, const std::string& file_name);

// Carries the client's requests to the server until destroyed.
class Pump {
public:
    explicit Pump(std::shared_ptr<Achievements> ra);
    ~Pump();

private:
    void Run();
    std::shared_ptr<Achievements> ra_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

}  // namespace RA

}  // namespace cartucho
