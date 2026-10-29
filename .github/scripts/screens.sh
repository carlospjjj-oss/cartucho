#!/bin/bash
# Screenshots on the CI emulator (see .github/workflows/screens.yml).
set -x
mkdir -p shots
PKG=com.cartucho.emu
adb wait-for-device
adb shell 'while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 2; done'
sleep 20
adb install -r -g app.apk 2>&1 | tee shots/install.txt
adb shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow
adb shell mkdir -p /sdcard/Roms
adb push roms/. /sdcard/Roms/
# No sounds or music on the emulator log; keep everything else default.
shot() { sleep "${2:-2}"; adb exec-out screencap -p > "shots/$1.png"; }
key() { adb shell input keyevent "$@"; sleep 0.6; }
start() {
  adb shell am force-stop $PKG
  adb shell am start -W -n $PKG/.LibraryActivity --es cartucho_test_folder /sdcard/Roms --es cartucho_test_style "$1" \
    >> shots/am.txt 2>&1
  sleep 12
  adb shell dumpsys window | grep -E "mCurrentFocus|mFocusedApp" >> shots/am.txt
}

# Landscape (rotation off, user rotation 90°).
adb shell settings put system accelerometer_rotation 0
adb shell settings put system user_rotation 1
adb shell wm user-rotation lock 1 || adb shell cmd window user-rotation lock 1
start console
shot 01-home-landscape 2
key KEYCODE_DPAD_RIGHT; key KEYCODE_DPAD_RIGHT
shot 02-home-moved 2
key KEYCODE_DPAD_CENTER
shot 03-list 3
key KEYCODE_DPAD_DOWN
shot 04-list-down 2
key KEYCODE_BUTTON_X
shot 05-game-menu 2
key KEYCODE_BACK
# Play a game: it runs in its own process.
key KEYCODE_DPAD_CENTER
shot 06-game 15
key KEYCODE_BACK
shot 07-in-game-menu 3
# Leave the game (the menu's last item) to make the moving preview.
adb shell input keyevent KEYCODE_MOVE_END; sleep 0.5
key KEYCODE_DPAD_UP; key KEYCODE_DPAD_DOWN
adb shell input keyevent KEYCODE_ENTER
sleep 6
shot 08-back-in-list 4
key KEYCODE_BUTTON_START
shot 09-start-menu 2
key KEYCODE_BACK

# Portrait.
adb shell settings put system user_rotation 0
adb shell wm user-rotation lock 0 || adb shell cmd window user-rotation lock 0
start console
shot 10-home-portrait 2
key KEYCODE_DPAD_CENTER
shot 11-list-portrait 3

# The grid look still there.
start grid
shot 12-grid-portrait 3
adb logcat -d > shots/logcat.txt || true
exit 0
