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
# The "full screen" tip would cover the first game.
adb shell settings put secure immersive_mode_confirmations confirmed
shot() { sleep "${2:-2}"; adb exec-out screencap -p > "shots/$1.png"; }
key() { adb shell input keyevent "$@"; sleep 0.6; }
right() { for _ in $(seq 1 "$1"); do key KEYCODE_DPAD_RIGHT; done; }
# start <style> <landscape> <settings "key=value;...">
start() {
  adb shell am force-stop $PKG
  adb shell am start -W -n $PKG/.LibraryActivity --es cartucho_test_folder /sdcard/Roms --es cartucho_test_style "$1" \
    --ez cartucho_test_landscape "$2" --es cartucho_test_set "'$3'" >> shots/am.txt 2>&1
  sleep 12
  adb shell dumpsys window | grep -E "mCurrentFocus|mFocusedApp" >> shots/am.txt
}
# Leaves a running game through its menu (the last item), back to the list.
quit_game() {
  key KEYCODE_BACK; sleep 1
  adb shell input keyevent KEYCODE_MOVE_END; sleep 0.5
  adb shell input keyevent KEYCODE_ENTER
  sleep 6
}

# Landscape, NES on a tube TV with the curved CRT filter.
start console true "filter=8;bezel=auto"
shot 01-home-landscape 2
right 1
shot 02-home-nes 2
key KEYCODE_DPAD_CENTER
shot 03-list-landscape 3
key KEYCODE_BUTTON_X
shot 04-game-options 2
key KEYCODE_BACK
key KEYCODE_DPAD_CENTER
shot 05-game-nes-crt-tv 15
key KEYCODE_BACK
shot 06-in-game-menu 3
key KEYCODE_BACK
quit_game
shot 07-back-in-list 4
key KEYCODE_BUTTON_START
shot 08-start-menu 2
key KEYCODE_BACK

# Master System: composite TV look, then the handheld and arcade frames.
# Cards now: recent, GB, NES, SNES, SMS, PS1, all.
for look in "filter=9;bezel=tv:09-sms-composite" "filter=7;bezel=handheld:10-sms-handheld" "filter=2;bezel=arcade:11-sms-arcade"; do
  start console true "${look%%:*}"
  right 4
  key KEYCODE_DPAD_CENTER; sleep 2
  key KEYCODE_DPAD_CENTER
  shot "${look##*:}" 15
  quit_game
done

# Portrait.
start console false "filter=2;bezel=auto"
shot 12-home-portrait 2
key KEYCODE_DPAD_CENTER
shot 13-list-portrait 3
right 2
key KEYCODE_DPAD_CENTER
shot 14-game-portrait 15
quit_game

# The grid look still there.
start grid false ""
shot 15-grid-portrait 3
adb logcat -d > shots/logcat.txt || true
exit 0
