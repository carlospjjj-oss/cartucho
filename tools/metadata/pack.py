#!/usr/bin/env python3
"""Packs game facts from the libretro database (metadat/) into small TSV files
the app ships: name, year, genre, developer, publisher, players.

  pack.py <libretro-database checkout> <output dir>
"""
import os
import re
import sys

SYSTEMS = [
    "Nintendo - Game Boy", "Nintendo - Game Boy Color", "Nintendo - Game Boy Advance",
    "Nintendo - Nintendo DS", "Nintendo - Nintendo 3DS",
    "Nintendo - Nintendo Entertainment System", "Nintendo - Super Nintendo Entertainment System",
    "Sega - Mega Drive - Genesis", "Sega - Master System - Mark III", "Sega - Game Gear",
    "Sony - PlayStation",
]
FIELDS = ["releaseyear", "genre", "developer", "publisher", "maxusers"]
GAME = re.compile(r'game\s*\((.*?)\n\)', re.S)
KV = re.compile(r'^\s*(\w+)\s+"((?:[^"\\]|\\.)*)"', re.M)


def read(path):
    out = {}
    if not os.path.exists(path):
        return out
    text = open(path, encoding="utf-8", errors="replace").read()
    for block in GAME.findall(text):
        kv = dict(KV.findall(block))
        name = kv.get("comment") or kv.get("name")
        if name:
            out[name] = kv
    return out


def main():
    db, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    for system in SYSTEMS:
        games = {}
        for field in FIELDS:
            for name, kv in read(os.path.join(db, "metadat", field, system + ".dat")).items():
                value = kv.get(field, "").replace("\t", " ").replace("\n", " ").strip()
                if value:
                    games.setdefault(name, {})[field] = value
        if not games:
            continue
        path = os.path.join(out_dir, system + ".tsv")
        with open(path, "w", encoding="utf-8") as f:
            for name in sorted(games):
                g = games[name]
                f.write("\t".join([name.replace("\t", " ")] + [g.get(k, "") for k in FIELDS]) + "\n")
        print(f"{system}: {len(games)} games, {os.path.getsize(path) // 1024} KB")


if __name__ == "__main__":
    main()
