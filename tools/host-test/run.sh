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

$CXX pacer_test.cpp $SRC/session.cpp $SRC/core_host.cpp $SRC/gl_presenter.cpp -lGLESv2 -ldl -o "$OUT/pacer_test"
$CXX resampler_test.cpp $SRC/resampler.cpp $SRC/core_host.cpp -ldl -o "$OUT/resampler_test"
gcc -shared -fPIC -O1 -I$SRC fake_hw_core.c -o "$OUT/fake_hw_core.so"
$CXX gl_test.cpp $SRC/core_host.cpp $SRC/gl_presenter.cpp -lEGL -lGLESv2 -ldl -o "$OUT/gl_test"
$CXX host_test.cpp $SRC/core_host.cpp -ldl -o "$OUT/host_test"

"$OUT/pacer_test"
"$OUT/resampler_test"
# Under gdb when available, so a crash in the GL driver comes with a stack.
if command -v gdb > /dev/null; then
    EGL_PLATFORM=surfaceless gdb -q -batch -return-child-result -ex run -ex bt \
        --args "$OUT/gl_test" "$OUT/fake_hw_core.so"
else
    EGL_PLATFORM=surfaceless "$OUT/gl_test" "$OUT/fake_hw_core.so"
fi
if [ $# -ge 2 ]; then
    "$OUT/host_test" "$1" "$2" "$OUT/frame.ppm" 400 2>/dev/null
fi
