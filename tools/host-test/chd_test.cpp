// Disc games: rcheevos must give a .chd the same hash as the same disc as
// .cue/.bin (its own reader), through Cartucho's CHD reader.
//   chd_test game.cue game.chd
#include <cstdio>
#include <cstring>

#include "chd_reader.h"
extern "C" {
#include "rc_consoles.h"
#include "rc_hash.h"
}

static int Hash(const char* path, bool ours, char out[33]) {
    rc_hash_iterator_t it;
    rc_hash_initialize_iterator(&it, path, nullptr, 0);
    if (ours)
        cartucho::GetDiscReader(&it.callbacks.cdreader);
    const int ok = rc_hash_generate(out, RC_CONSOLE_PLAYSTATION, &it);
    rc_hash_destroy_iterator(&it);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    char a[33] = {}, b[33] = {}, c[33] = {};
    const int ok_a = Hash(argv[1], false, a);
    const int ok_b = Hash(argv[2], true, b);
    const int ok_c = Hash(argv[1], true, c);
    int failures = 0;
    std::printf("%s  .cue hashed by rcheevos: %s\n", ok_a ? "ok  " : "FAIL", a);
    failures += !ok_a;
    const bool same = ok_b && std::strcmp(a, b) == 0;
    std::printf("%s  .chd hashed through the CHD reader: %s\n", same ? "ok  " : "FAIL", b);
    failures += !same;
    const bool passthrough = ok_c && std::strcmp(a, c) == 0;
    std::printf("%s  .cue still handled with the CHD reader installed\n", passthrough ? "ok  " : "FAIL");
    failures += !passthrough;
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
