#!/usr/bin/env python3
"""Original, colourful console illustrations for the carousel cards.

Each drawing is a list of simple shapes, written out both as an Android
VectorDrawable (app/src/main/res/drawable/art_<name>.xml) and as SVG for
previewing. Generic hardware shapes only: no logos, no brand wordmarks.

  consoles.py <res/drawable dir> [<svg preview dir>]
"""
import os
import sys

V = 200  # viewport


def rr(x, y, w, h, r, fill, stroke=None, sw=0):
    return ("rr", x, y, w, h, r, fill, stroke, sw)


def circ(cx, cy, r, fill, stroke=None, sw=0):
    return ("c", cx, cy, r, fill, stroke, sw)


def path(d, fill, stroke=None, sw=0):
    return ("p", d, fill, stroke, sw)


def lg(c1, c2, x1=0, y1=0, x2=0, y2=V):
    return ("lg", c1, c2, x1, y1, x2, y2)


def shadow(x, y, w, h, r):
    return rr(x + 4, y + 7, w, h, r, "#33000000")


def dpad(cx, cy, s, color="#2B2B33"):
    a = s * 0.34
    return [rr(cx - a, cy - s, 2 * a, 2 * s, a * 0.4, color), rr(cx - s, cy - a, 2 * s, 2 * a, a * 0.4, color)]


def rr_path(x, y, w, h, r):
    r = min(r, w / 2, h / 2)
    return (f"M{x+r},{y} H{x+w-r} A{r},{r} 0 0 1 {x+w},{y+r} V{y+h-r} A{r},{r} 0 0 1 {x+w-r},{y+h} "
            f"H{x+r} A{r},{r} 0 0 1 {x},{y+h-r} V{y+r} A{r},{r} 0 0 1 {x+r},{y} Z")


def circ_path(cx, cy, r):
    return f"M{cx-r},{cy} A{r},{r} 0 1 0 {cx+r},{cy} A{r},{r} 0 1 0 {cx-r},{cy} Z"


def screen(x, y, w, h, glow1, glow2):
    """A lit screen with a soft reflection."""
    return [
        rr(x, y, w, h, 4, lg(glow1, glow2, x, y, x + w, y + h)),
        path(f"M{x+4},{y+4} L{x+w*0.55},{y+4} L{x+4},{y+h*0.6} Z", "#30FFFFFF"),
    ]


ART = {}

# Game Boy: upright handheld, grey, green screen, magenta buttons.
ART["gb"] = [shadow(52, 18, 96, 164, 12),
    rr(52, 18, 96, 164, 12, lg("#E9E6EF", "#BDB8C9")),
    rr(64, 30, 72, 62, 6, "#5E5A72"),
    *screen(76, 38, 48, 44, "#B6E37A", "#5FA33A"),
    *dpad(78, 120, 13),
    circ(118, 124, 8, "#D6336C"), circ(134, 113, 8, "#D6336C"),
    rr(84, 150, 14, 5, 2.5, "#8E8A9C"), rr(104, 150, 14, 5, 2.5, "#8E8A9C"),
    path("M118,160 L142,166 M120,166 L142,172 M122,172 L142,178", None, "#9A96A8", 2)]

# Game Boy Advance: wide purple handheld.
ART["gba"] = [shadow(14, 58, 172, 90, 40),
    rr(14, 58, 172, 90, 40, lg("#9B7BFF", "#5A3BD6", 14, 58, 186, 148)),
    rr(56, 66, 88, 70, 8, "#2A2440"),
    *screen(64, 72, 72, 56, "#7FD4FF", "#4A6CF0"),
    *dpad(36, 104, 12, "#2A2440"),
    circ(162, 96, 8, "#2A2440"), circ(176, 110, 8, "#2A2440"),
    circ(160, 96, 5, "#FFB547"), circ(174, 110, 5, "#FF6B9A"),
    rr(84, 140, 10, 4, 2, "#2A2440"), rr(104, 140, 10, 4, 2, "#2A2440")]

# Nintendo DS: clamshell, two screens.
ART["nds"] = [shadow(40, 20, 120, 160, 12),
    rr(40, 20, 120, 76, 12, lg("#5DA9FF", "#2E6FD8")),
    *screen(56, 30, 88, 58, "#FFE08A", "#FF8A5B"),
    rr(40, 100, 120, 80, 12, lg("#4A95F0", "#2459B8")),
    *screen(68, 110, 64, 50, "#9BF0C8", "#3CC6A0"),
    *dpad(54, 132, 9, "#1D3C78"),
    circ(146, 126, 5, "#1D3C78"), circ(154, 134, 5, "#1D3C78"), circ(138, 134, 5, "#1D3C78"), circ(146, 142, 5, "#1D3C78"),
    rr(44, 93, 112, 9, 4, "#1D3C78")]

# Nintendo 3DS: clamshell with a wide top screen and a circle pad.
ART["n3ds"] = [shadow(36, 20, 128, 162, 14),
    rr(36, 20, 128, 78, 14, lg("#FF6B6B", "#C9302C")),
    *screen(48, 30, 104, 60, "#B4F0FF", "#5A9BFF"),
    rr(36, 102, 128, 80, 14, lg("#F25555", "#B02424")),
    *screen(70, 112, 60, 46, "#FFF1A8", "#FFB547"),
    circ(54, 124, 10, "#5A1414"), circ(54, 124, 7, "#2B0A0A"),
    *dpad(54, 156, 8, "#5A1414"),
    circ(146, 130, 4.5, "#FFD24D"), circ(154, 138, 4.5, "#4DD2FF"), circ(138, 138, 4.5, "#5AE07A"), circ(146, 146, 4.5, "#FF5A5A"),
    rr(40, 95, 120, 10, 4, "#5A1414")]

# NES: a grey box with a dark front strip, and its flat controller.
ART["nes"] = [shadow(30, 44, 140, 76, 8),
    rr(30, 44, 140, 76, 8, lg("#ECE9F2", "#C9C4D4")),
    rr(30, 92, 140, 28, 6, "#3B3946"),
    rr(44, 100, 26, 6, 3, "#E03C3C"), rr(44, 110, 26, 4, 2, "#8E8A9C"),
    rr(118, 100, 40, 12, 3, "#5A5866"),
    shadow(48, 130, 104, 44, 6),
    rr(48, 130, 104, 44, 6, lg("#F0EDF5", "#CFCADB")),
    rr(56, 137, 88, 30, 4, "#2B2933"),
    *dpad(72, 152, 9, "#E6E3EC"),
    circ(122, 155, 6, "#E03C3C"), circ(136, 155, 6, "#E03C3C"),
    rr(94, 152, 9, 4, 2, "#8E8A9C")]

# Super Nintendo: rounded grey console with a colourful controller.
ART["snes"] = [shadow(34, 36, 132, 70, 16),
    rr(34, 36, 132, 70, 16, lg("#EFEDF4", "#C7C2D3")),
    rr(56, 46, 88, 30, 10, "#9B95AE"), rr(64, 50, 72, 6, 3, "#7E7894"),
    circ(50, 92, 5, "#8F67D9"), circ(150, 92, 5, "#8F67D9"),
    shadow(36, 118, 128, 56, 28),
    path("M64,118 H136 A28,28 0 0 1 136,174 H64 A28,28 0 0 1 64,118 Z", lg("#E8E5EF", "#C2BDCF")),
    *dpad(64, 146, 11, "#3A3846"),
    circ(136, 134, 6, "#4D8BFF"), circ(148, 146, 6, "#FF4D4D"), circ(124, 146, 6, "#3DD66B"), circ(136, 158, 6, "#FFC93D"),
    rr(90, 148, 8, 4, 2, "#8E8A9C"), rr(102, 148, 8, 4, 2, "#8E8A9C")]

# Mega Drive: black console with a round top, three-button pad.
ART["md"] = [shadow(26, 40, 148, 74, 18),
    rr(26, 40, 148, 74, 18, lg("#3A3A44", "#141418")),
    circ(100, 70, 26, "#26262E"), circ(100, 70, 18, "#0D0D10"),
    path("M40,100 H160", None, "#FF7A2E", 3),
    rr(132, 52, 26, 8, 4, "#FF7A2E"),
    shadow(42, 124, 116, 52, 24),
    path("M66,124 H134 A24,26 0 0 1 134,176 H66 A24,26 0 0 1 66,124 Z", lg("#33333C", "#141418")),
    *dpad(68, 150, 10, "#55555F"),
    circ(116, 156, 6, "#FF7A2E"), circ(130, 150, 6, "#FF7A2E"), circ(144, 144, 6, "#FF7A2E")]

# Master System: angular black console with a red stripe.
ART["sms"] = [shadow(28, 56, 144, 70, 6),
    path("M28,70 L44,56 H172 V112 L156,126 H28 Z", lg("#33333C", "#111115")),
    path("M28,98 H172 V106 H28 Z", "#E8393C"),
    rr(44, 66, 52, 22, 3, "#1B1B21"), rr(112, 70, 44, 10, 3, "#E8393C"),
    shadow(54, 136, 92, 40, 4),
    rr(54, 136, 92, 40, 4, lg("#2D2D35", "#121216")),
    *dpad(78, 156, 10, "#4A4A55"),
    rr(110, 146, 12, 18, 3, "#E8393C"), rr(128, 146, 12, 18, 3, "#E8393C")]

# Game Gear: wide black handheld, colour screen.
ART["gg"] = [shadow(14, 56, 172, 92, 30),
    rr(14, 56, 172, 92, 30, lg("#3A3A44", "#131317", 14, 56, 186, 148)),
    rr(56, 64, 88, 74, 8, "#0B0B0E"),
    *screen(64, 72, 72, 58, "#8CF59A", "#2FA8E0"),
    *dpad(36, 104, 12, "#55555F"),
    circ(160, 110, 9, "#2F7DE0"), circ(176, 98, 9, "#2F7DE0"),
    circ(160, 74, 4, "#E8393C")]

# PlayStation: grey console with a round lid, and a pad with handles.
ART["ps1"] = [shadow(28, 30, 144, 80, 10),
    rr(28, 30, 144, 80, 10, lg("#E4E3EA", "#B9B7C4")),
    circ(100, 66, 30, "#CFCDD8"), circ(100, 66, 24, "#D9D7E1"), circ(100, 66, 6, "#A9A6B6"),
    circ(46, 96, 5, "#8F8C9C"), circ(154, 96, 5, "#8F8C9C"), rr(132, 42, 28, 6, 3, "#8F8C9C"),
    shadow(32, 120, 136, 60, 22),
    path("M54,120 H146 C164,120 172,140 168,170 C166,182 150,184 142,172 L132,160 H68 L58,172 C50,184 34,182 32,170 C28,140 36,120 54,120 Z",
         lg("#D8D6E0", "#A9A6B6", 0, 120, 0, 182)),
    *dpad(62, 140, 9, "#6E6B7C"),
    circ(138, 132, 5.5, "#3DD6B5"), circ(150, 142, 5.5, "#FF5A6A"), circ(126, 142, 5.5, "#FF7AD0"), circ(138, 152, 5.5, "#5A8BFF"),
    circ(84, 160, 8, "#3A3846"), circ(116, 160, 8, "#3A3846")]

# Collections.
ART["recent"] = [circ(100, 100, 70, lg("#FFB04D", "#FF6B3D")), circ(100, 100, 56, "#26FFFFFF"),
    path("M84,68 L138,100 L84,132 Z", "#FFFFFF")]
ART["fav"] = [circ(100, 100, 70, lg("#FFD45C", "#FF9F1C")),
    path("M100,46 L114,82 L152,84 L122,108 L132,146 L100,124 L68,146 L78,108 L48,84 L86,82 Z", "#FFFFFF")]
ART["all"] = [rr(30, 30, 64, 64, 12, lg("#9B7BFF", "#5A3BD6")), rr(106, 30, 64, 64, 12, lg("#5DA9FF", "#2E6FD8")),
    rr(30, 106, 64, 64, 12, lg("#3FD69B", "#1E9C6B")), rr(106, 106, 64, 64, 12, lg("#FF8A5B", "#E0483C"))]
ART["search"] = [circ(88, 88, 46, None, "#FFFFFF", 14), path("M120,120 L160,160", None, "#FFFFFF", 16)]


def color_xml(c):
    if c is None:
        return None
    if c.startswith("#") and len(c) == 9:
        return c
    return c


def vd_fill(attr, f):
    if f is None:
        return "", ""
    if isinstance(f, tuple) and f[0] == "lg":
        _, c1, c2, x1, y1, x2, y2 = f
        inner = (f'        <aapt:attr name="android:{attr}">\n'
                 f'            <gradient android:type="linear" android:startX="{x1}" android:startY="{y1}" '
                 f'android:endX="{x2}" android:endY="{y2}" android:startColor="{c1}" android:endColor="{c2}" />\n'
                 f'        </aapt:attr>\n')
        return "", inner
    return f' android:{attr}="{color_xml(f)}"', ""


def shape_path(s):
    k = s[0]
    if k == "rr":
        _, x, y, w, h, r, fill, stroke, sw = s
        return rr_path(x, y, w, h, r), fill, stroke, sw
    if k == "c":
        _, cx, cy, r, fill, stroke, sw = s
        return circ_path(cx, cy, r), fill, stroke, sw
    _, d, fill, stroke, sw = s
    return d, fill, stroke, sw


def to_vd(shapes):
    out = ['<?xml version="1.0" encoding="utf-8"?>',
           '<!-- Generated by tools/art/consoles.py: original illustration, no logos. -->',
           '<vector xmlns:android="http://schemas.android.com/apk/res/android"',
           '    xmlns:aapt="http://schemas.android.com/aapt"',
           f'    android:width="{V}dp" android:height="{V}dp"',
           f'    android:viewportWidth="{V}" android:viewportHeight="{V}">']
    for s in shapes:
        d, fill, stroke, sw = shape_path(s)
        fa, fi = vd_fill("fillColor", fill)
        sa, si = vd_fill("strokeColor", stroke)
        extra = f' android:strokeWidth="{sw}" android:strokeLineCap="round"' if stroke else ""
        if fi or si:
            out.append(f'    <path android:pathData="{d}"{fa}{sa}{extra}>')
            out.append((fi + si).rstrip("\n"))
            out.append('    </path>')
        else:
            out.append(f'    <path android:pathData="{d}"{fa}{sa}{extra} />')
    out.append('</vector>')
    return "\n".join(out) + "\n"


def svg_color(c):
    # #AARRGGBB -> rgba
    if c.startswith("#") and len(c) == 9:
        a = int(c[1:3], 16) / 255
        return f"rgba({int(c[3:5],16)},{int(c[5:7],16)},{int(c[7:9],16)},{a:.3f})"
    return c


def to_svg(shapes, size=200, prefix="a"):
    defs, body = [], []
    for i, s in enumerate(shapes):
        d, fill, stroke, sw = shape_path(s)
        def paint(p, kind):
            if p is None:
                return "none"
            if isinstance(p, tuple):
                gid = f"{prefix}{i}{kind}"
                _, c1, c2, x1, y1, x2, y2 = p
                defs.append(f'<linearGradient id="{gid}" gradientUnits="userSpaceOnUse" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">'
                            f'<stop offset="0" stop-color="{svg_color(c1)}"/><stop offset="1" stop-color="{svg_color(c2)}"/></linearGradient>')
                return f"url(#{gid})"
            return svg_color(p)
        body.append(f'<path d="{d}" fill="{paint(fill,"f")}" stroke="{paint(stroke,"s")}" stroke-width="{sw}" stroke-linecap="round"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 {V} {V}">'
            f'<defs>{"".join(defs)}</defs>{"".join(body)}</svg>')


if __name__ == "__main__":
    res = sys.argv[1]
    os.makedirs(res, exist_ok=True)
    # The consoles themselves use photos now (photos.py); only the collections ship as drawings.
    for name, shapes in ART.items():
        if name not in ("recent", "fav", "all", "search"):
            continue
        with open(os.path.join(res, f"art_{name}.xml"), "w") as f:
            f.write(to_vd(shapes))
    if len(sys.argv) > 2:
        os.makedirs(sys.argv[2], exist_ok=True)
        for name, shapes in ART.items():
            with open(os.path.join(sys.argv[2], f"{name}.svg"), "w") as f:
                f.write(to_svg(shapes, prefix=name))
    print(f"{len(ART)} drawings")
