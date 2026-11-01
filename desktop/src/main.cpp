// Cartucho PC - entry point. Without arguments: the library (console-style
// interface). With --play: one game, in this process (the library starts
// each game as a new process).
#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "cloud.h"
#include "game.h"
#include "library_app.h"
#include "paths.h"

using namespace cartucho;

int main(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cartucho", SDL_GetError(), nullptr);
        return 1;
    }
    Paths::Init(argv[0]);
    // Extra controller layouts next to the program (gamecontrollerdb.txt).
    SDL_GameControllerAddMappingsFromFile(JoinPath(Paths::Get().assets, "gamecontrollerdb.txt").c_str());

    // Backups from the command line: --export-backup <zip>, --import-backup <zip>.
    if (argc == 3 && (std::strcmp(argv[1], "--export-backup") == 0 || std::strcmp(argv[1], "--import-backup") == 0)) {
        const bool exp = std::strcmp(argv[1], "--export-backup") == 0;
        const Backup::Result r = exp ? Backup::Export(argv[2], false) : Backup::Restore(argv[2]);
        std::printf("%s\n", r.message.c_str());
        SDL_Quit();
        return r.ok ? 0 : 1;
    }

    GameArgs game;
    bool play = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--play") { play = true; game.rom = next(); }
        else if (a == "--system") game.system = next();
        else if (a == "--save-name") game.save_name = next();
        else if (a == "--title") game.title = next();
        else if (a == "--no-resume") game.resume = false;
        else if (a == "--entry") game.entry = next();
    }
    int rc;
    if (play) {
        if (game.title.empty()) game.title = FileName(game.rom);
        if (game.save_name.empty()) {
            game.save_name = FileName(game.rom);
            const size_t dot = game.save_name.find_last_of('.');
            if (dot != std::string::npos) game.save_name.resize(dot);
        }
        rc = RunGame(game);
    } else {
        rc = RunLibrary();
    }
    SDL_Quit();
    return rc;
}
