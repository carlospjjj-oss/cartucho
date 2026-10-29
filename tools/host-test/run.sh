#!/bin/bash
# Builds and runs Cartucho's native tests on Linux: the frame pacer, the audio
# resampler, and (with Mesa installed) the GLES presenter with a fake
# hardware-rendered core. Pass a libretro GBA core and ROM to also run the
# full host test:  run.sh vbam_libretro.so suite.gba
set -euo pipefail
cd "$(dirname "$0")"
SRC=../../app/src/main/cpp
OUT=${OUT:-build}
mkdir -p "$OUT"
export OUT
CXX="${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -I$SRC"

# RetroAchievements client library, as the app builds it (CMakeLists.txt).
RA=$SRC/third_party/rcheevos
RAFLAGS="-DRC_CLIENT_SUPPORTS_HASH -DRC_HASH_NO_ENCRYPTED -I$RA/include -I$RA/src/libretro"
mkdir -p "$OUT/rcheevos"
for c in $RA/src/{api,client,hash,libretro,runtime,util}/*.c; do
    o="$OUT/rcheevos/$(basename "$c" .c).o"
    [ "$o" -nt "$c" ] || gcc -c -O1 -w $RAFLAGS -I$SRC "$c" -o "$o"
done
ar rcs "$OUT/librcheevos.a" "$OUT"/rcheevos/*.o
# libchdr (CHD disc images), with its own CMake build.
cmake -S $SRC/third_party/libchdr -B "$OUT/libchdr" -DBUILD_SHARED_LIBS=OFF -DCHDR_WANT_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$OUT/libchdr" --target chdr-static -j "$(nproc)" > /dev/null
L="$OUT/libchdr"
CHDR="-I$SRC/third_party/libchdr/include $L/libchdr-static.a $L/deps/lzma-26.02/libchdr-lzma.a $L/deps/zstd-1.5.7/libzstd.a $L/deps/miniz-3.1.2/libminiz.a"
RCLIB="$SRC/chd_reader.cpp $RAFLAGS $OUT/librcheevos.a $CHDR"
SESSION="$SRC/session.cpp $SRC/achievements.cpp $SRC/recorder.cpp $RCLIB"

$CXX pacer_test.cpp $SESSION $SRC/rewind.cpp $SRC/runahead.cpp $SRC/core_host.cpp $SRC/gl_presenter.cpp -lGLESv2 -lz -ldl -lpthread -o "$OUT/pacer_test"
$CXX resampler_test.cpp $SRC/resampler.cpp $SRC/core_host.cpp -ldl -o "$OUT/resampler_test"
gcc -shared -fPIC -O1 -I$SRC fake_hw_core.c -o "$OUT/fake_hw_core.so"
$CXX gl_test.cpp $SRC/core_host.cpp $SRC/gl_presenter.cpp -lEGL -lGLESv2 -ldl -o "$OUT/gl_test"
$CXX tv_test.cpp $SRC/core_host.cpp $SRC/gl_presenter.cpp -lEGL -lGLESv2 -ldl -o "$OUT/tv_test"
$CXX host_test.cpp $SRC/core_host.cpp -ldl -o "$OUT/host_test"
$CXX rewind_test.cpp $SRC/rewind.cpp $SRC/core_host.cpp -lz -ldl -lpthread -o "$OUT/rewind_test"
$CXX devices_test.cpp $SRC/core_host.cpp -ldl -o "$OUT/devices_test"
$CXX runahead_test.cpp $SRC/runahead.cpp $SRC/core_host.cpp -ldl -o "$OUT/runahead_test"
$CXX link_test.cpp $SRC/core_host.cpp -ldl -lpthread -o "$OUT/link_test"
$CXX recorder_test.cpp $SRC/recorder.cpp -lpthread -o "$OUT/recorder_test"
$CXX achievements_test.cpp $SRC/achievements.cpp $SRC/core_host.cpp $RCLIB -ldl -lpthread -o "$OUT/achievements_test"
$CXX chd_test.cpp $RCLIB -lpthread -o "$OUT/chd_test"

"$OUT/pacer_test"
"$OUT/resampler_test"
"$OUT/recorder_test"
# A PlayStation-like disc as .cue/.bin and as .chd must hash the same.
"$OUT/chd_test" roms/disc.cue roms/disc.chd
# If the GLES test crashes, run it again under gdb for a stack trace.
if ! EGL_PLATFORM=surfaceless "$OUT/gl_test" "$OUT/fake_hw_core.so"; then
    if command -v gdb > /dev/null; then
        EGL_PLATFORM=surfaceless gdb -q -batch -ex run -ex bt \
            --args "$OUT/gl_test" "$OUT/fake_hw_core.so" || true
    fi
    exit 1
fi
EGL_PLATFORM=surfaceless "$OUT/tv_test" "$OUT/fake_hw_core.so"
if [ $# -ge 2 ]; then
    "$OUT/host_test" "$1" "$2" "$OUT/frame.ppm" 400 2>/dev/null
    CARTUCHO_TEST_FD=1 "$OUT/host_test" "$1" "$2" "$OUT/frame-fd.ppm" 200 2>/dev/null | grep -E "load game|FAIL|passed"
    "$OUT/rewind_test" "$1" "$2" 2>/dev/null
    case "$2" in *.gb|*.gbc) "$OUT/achievements_test" "$1" "$2" 2>/dev/null ;; esac
    # Run-ahead: off for the first two seconds, then (after a passing
    # self-check) exactly the normal run's picture N frames later. Checked on
    # the given ROM and on roms/vblank.gba (interrupt-driven like a game).
    for rom in "$2" roms/vblank.gba; do
        for n in 0 1 2; do "$OUT/runahead_test" "$1" "$rom" $n 2>/dev/null > "$OUT/ra$n.txt"; done
        python3 - "$OUT" "$rom" <<'PY'
import sys
out, rom = sys.argv[1], sys.argv[2]
def load(p): return [l.split()[1] for l in open(p)]
normal = load(f"{out}/ra0.txt")
fail = 0
for n in (1, 2):
    ra = load(f"{out}/ra{n}.txt")
    start = all(ra[k] == normal[k] for k in range(118))
    same = [ra[k] == normal[k + n] for k in range(125, len(ra) - n)
            if all((k + i) // 40 == k // 40 for i in range(1, n + 1))]
    ok = start and all(same) and len(same) > 200
    fail += not ok
    print(("ok    " if ok else "FAIL  ") + f"run-ahead {n} on {rom.split('/')[-1]}: {sum(same)}/{len(same)} frames match the future")
sys.exit(fail)
PY
    done
    # Network link between two processes, when the core has the extension.
    if nm -D "$1" 2>/dev/null | grep -c cartucho_link_start > /dev/null; then
        "$OUT/link_test" "$1" "$2" server 5791 2>/dev/null &
        server=$!
        sleep 1
        "$OUT/link_test" "$1" "$2" client 5791 2>/dev/null
        wait $server
    fi
fi
