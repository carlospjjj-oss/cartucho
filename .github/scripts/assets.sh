#!/bin/bash
# Lists and data the apps ship (Android APK assets, PC assets/): the names
# of the libretro cover collection, of the ready-made cheat files, and the
# game facts. Only file lists and small text are fetched, no images.
#   assets.sh <output folder>
set -euo pipefail
OUT=$1
WORK=$(mktemp -d)
mkdir -p "$OUT/covers" "$OUT/cheats" "$OUT/meta"
for repo in Nintendo_-_Game_Boy Nintendo_-_Game_Boy_Color Nintendo_-_Game_Boy_Advance \
            Nintendo_-_Nintendo_DS Nintendo_-_Nintendo_3DS \
            Nintendo_-_Nintendo_Entertainment_System Nintendo_-_Super_Nintendo_Entertainment_System \
            Sega_-_Mega_Drive_-_Genesis Sega_-_Master_System_-_Mark_III Sega_-_Game_Gear \
            Sony_-_PlayStation; do
  git clone -q --depth 1 --filter=blob:none --no-checkout \
    "https://github.com/libretro-thumbnails/$repo.git" "$WORK/thumbs-$repo"
  # -z: names exactly as stored (no quoting of accents or quotes).
  git -C "$WORK/thumbs-$repo" ls-tree -z --name-only HEAD Named_Boxarts/ | tr '\0' '\n' \
    | sed -n 's|^Named_Boxarts/\(.*\)\.png$|\1|p' > "$OUT/covers/$repo.txt"
  echo "$repo: $(wc -l < "$OUT/covers/$repo.txt") names"
done
git clone -q --depth 1 --filter=blob:none --no-checkout \
  https://github.com/libretro/libretro-database.git "$WORK/lrdb"
for dir in "Nintendo - Game Boy" "Nintendo - Game Boy Color" "Nintendo - Game Boy Advance" \
           "Nintendo - Nintendo DS" "Nintendo - Nintendo Entertainment System" \
           "Nintendo - Super Nintendo Entertainment System" "Sega - Mega Drive - Genesis" \
           "Sega - Master System - Mark III" "Sega - Game Gear" "Sony - PlayStation"; do
  git -C "$WORK/lrdb" ls-tree -z --name-only HEAD "cht/$dir/" | tr '\0' '\n' \
    | sed -n 's|^cht/[^/]*/\(.*\)\.cht$|\1|p' > "$OUT/cheats/$dir.txt"
  echo "$dir: $(wc -l < "$OUT/cheats/$dir.txt") cheat files"
done
for f in releaseyear genre developer publisher maxusers; do
  git -C "$WORK/lrdb" checkout -q HEAD -- "metadat/$f" || true
done
python3 "$(dirname "$0")/../../tools/metadata/pack.py" "$WORK/lrdb" "$OUT/meta"
rm -rf "$WORK"
