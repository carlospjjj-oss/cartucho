#!/usr/bin/env python3
"""Fetches real photos of the consoles from Wikimedia Commons (public domain
or free licences only), cuts away the plain background, trims and shrinks
them, and writes app/src/main/res/drawable-nodpi/photo_<name>.png plus
tools/art/PHOTO_CREDITS.md. Run by .github/workflows/art.yml.

For each console a few known file names are tried in order; if none exists
the Commons search is used. The log says which file each picture came from.
"""
import io
import json
import os
import sys
import time
import urllib.parse
import urllib.request

from PIL import Image, ImageFilter

API = "https://commons.wikimedia.org/w/api.php"
UA = {"User-Agent": "CartuchoArtFetcher/1.0 (https://github.com/carlospjjj-oss/cartucho)"}
FREE = ("public domain", "pd", "cc0", "cc by", "cc-by", "cc by-sa", "cc-by-sa")

CANDIDATES = {
    "gb": ["Game-Boy-FL.jpg", "Game-Boy-FL.png", "Nintendo-Game-Boy-FL.jpg"],
    "gba": ["Nintendo-Game-Boy-Advance-Purple-FL.jpg", "Game-Boy-Advance-Purple-FL.jpg",
            "Nintendo-Game-Boy-Advance-Milky-Pink-FL.jpg"],
    "nds": ["Nintendo-DS-Fat-Blue.jpg", "Nintendo-DS-Fat-Blue.png", "Nintendo-DS-Lite-Black-Open.jpg",
            "Nintendo-DS-Lite-w-stylus.jpg"],
    "n3ds": ["Nintendo-3DS-AquaOpen.jpg", "Nintendo-3DS-AquaOpen.png", "Nintendo-3DS-AquaOpen-Pushed.jpg"],
    "nes": ["NES-Console-Set.png", "NES-Console-Set.jpg"],
    "snes": ["SNES-Mod1-Console-Set.png", "SNES-Mod1-Console-Set.jpg", "Nintendo-Super-Famicom-Set-FL.jpg"],
    "md": ["Sega-Mega-Drive-JP-Mk1-Console-Set.png", "Sega-Mega-Drive-JP-Mk1-Console-Set.jpg",
           "Sega-Mega-Drive-EU-Mk1-wController-FL.png", "Sega-Genesis-Mod1-Set.jpg"],
    "sms": ["Sega-Master-System-Set.png", "Sega-Master-System-Set.jpg", "Master-System-Set.jpg",
            "Sega-Master-System-Mk1-Set.jpg"],
    "gg": ["Game-Gear-Handheld.png", "Game-Gear-Handheld.jpg", "Sega-Game-Gear-WB.png", "Sega-Game-Gear-WB.jpg"],
    "ps1": ["PSX-Console-wController.png", "PSX-Console-wController.jpg",
            "PlayStation-SCPH-1000-with-Controller.png"],
}
SEARCH = {
    "gb": "Game Boy DMG Evan-Amos", "gba": "Game Boy Advance Evan-Amos", "nds": "Nintendo DS Evan-Amos",
    "n3ds": "Nintendo 3DS Evan-Amos", "nes": "NES console set Evan-Amos", "snes": "SNES console set Evan-Amos",
    "md": "Mega Drive console set Evan-Amos", "sms": "Master System console set Evan-Amos",
    "gg": "Game Gear Evan-Amos", "ps1": "PlayStation console controller Evan-Amos",
}


def api(**params):
    params.update(format="json", formatversion="2")
    url = API + "?" + urllib.parse.urlencode(params)
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
                return json.load(r)
        except Exception as e:
            print("  retry:", e)
            time.sleep(3 * (attempt + 1))
    raise RuntimeError(url)


def info(titles):
    """Title -> (thumb url, licence, artist) for the titles that exist."""
    out = {}
    data = api(action="query", prop="imageinfo", iiprop="url|extmetadata", iiurlwidth="900",
               iiextmetadatafilter="LicenseShortName|Artist", titles="|".join("File:" + t for t in titles))
    for p in data["query"]["pages"]:
        if p.get("missing") or "imageinfo" not in p:
            continue
        ii = p["imageinfo"][0]
        meta = ii.get("extmetadata", {})
        lic = meta.get("LicenseShortName", {}).get("value", "")
        artist = meta.get("Artist", {}).get("value", "")
        out[p["title"][5:]] = (ii.get("thumburl") or ii["url"], lic, artist, ii["descriptionurl"])
    return out


def free(lic):
    return any(lic.lower().startswith(f) for f in FREE)


def fetch(url):
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
                return r.read()
        except Exception as e:
            print("  retry:", e)
            time.sleep(5 * (attempt + 1))
    raise RuntimeError(url)


def cut_background(im):
    """Plain light background -> transparent (flood fill from the borders)."""
    im = im.convert("RGBA")
    w, h = im.size
    if im.getextrema()[3][0] < 250:
        return im  # already has transparency
    px = im.load()
    seen = bytearray(w * h)
    stack = [(x, 0) for x in range(w)] + [(x, h - 1) for x in range(w)] + \
            [(0, y) for y in range(h)] + [(w - 1, y) for y in range(h)]

    def bg(c):
        r, g, b, _ = c
        return min(r, g, b) >= 232 and max(r, g, b) - min(r, g, b) <= 18

    mask = Image.new("L", (w, h), 255)
    m = mask.load()
    while stack:
        x, y = stack.pop()
        i = y * w + x
        if seen[i]:
            continue
        seen[i] = 1
        if not bg(px[x, y]):
            continue
        m[x, y] = 0
        if x > 0: stack.append((x - 1, y))
        if x < w - 1: stack.append((x + 1, y))
        if y > 0: stack.append((x, y - 1))
        if y < h - 1: stack.append((x, y + 1))
    # Soften the cut edge a little.
    mask = mask.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.8))
    im.putalpha(mask)
    return im


def process(data, size=560):
    im = Image.open(io.BytesIO(data))
    im = cut_background(im)
    box = im.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox()
    if box:
        im = im.crop(box)
    im.thumbnail((size, size), Image.LANCZOS)
    return im


def main(res_dir, credits_path):
    os.makedirs(res_dir, exist_ok=True)
    credits = ["# Console photos", "",
               "Fetched from Wikimedia Commons by tools/art/photos.py. Background removed and resized.", ""]
    failed = []
    for name, cands in CANDIDATES.items():
        print(f"== {name}")
        found = info(cands)
        pick = next(((t, found[t]) for t in cands if t in found and free(found[t][1])), None)
        if pick is None:
            res = api(action="query", list="search", srsearch=SEARCH[name], srnamespace="6", srlimit="10")
            titles = [r["title"][5:] for r in res["query"]["search"]
                      if r["title"].lower().endswith((".jpg", ".jpeg", ".png"))]
            print("  search:", titles)
            found = info(titles) if titles else {}
            pick = next(((t, found[t]) for t in titles if t in found and free(found[t][1])), None)
        if pick is None:
            print("  NOTHING FOUND")
            failed.append(name)
            continue
        title, (url, lic, artist, page) = pick
        print(f"  {title} [{lic}] {url}")
        im = process(fetch(url))
        im.save(os.path.join(res_dir, f"photo_{name}.png"), optimize=True)
        print("  ->", im.size)
        import re
        artist = re.sub("<[^>]+>", "", artist).strip()
        credits.append(f"- photo_{name}.png: [{title}]({page}), {artist or 'unknown author'}, {lic}")
    with open(credits_path, "w") as f:
        f.write("\n".join(credits) + "\n")
    if failed:
        print("missing:", failed)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
