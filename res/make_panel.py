#!/usr/bin/env python3
# Draws res/DungRevisions.svg. VCV Rack can't draw SVG <text>, so every label
# is turned into outline paths with fontTools. Positions are in mm and must
# match src/DungRevisions.cpp.
#   pip install fonttools
#   python3 res/make_panel.py
import math
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")

W, H = 142.24, 128.5  # 28 HP

SOIL = "#2F2720"
SOIL_DARK = "#251F19"
INK = "#EFE6D2"
SOFT = "#B3A68F"
GATE = "#6CCB8A"
CV = "#E9A84A"
BUS2 = "#E07AB8"
WINDOW = "#100E0C"

fonts = {}


def text(s, x, y, size, font=FONT_MED, fill=INK, anchor="middle", spacing=0.04):
    """Return an SVG path for string s; (x, y) is the baseline point in mm."""
    if font not in fonts:
        f = TTFont(font)
        fonts[font] = (f, f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    f, gs, cmap, upm = fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    if anchor == "middle":
        x0 = x - width * scale / 2
    elif anchor == "end":
        x0 = x - width * scale
    else:
        x0 = x
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        tp = TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y))
        gs[n].draw(tp)
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


parts = []
add = parts.append


def ring(x, y, colour, r=4.7):
    add(f'<circle cx="{x}" cy="{y}" r="{r}" fill="none" stroke="{colour}" stroke-width="1.1"/>')


def label(s, x, y, size=2.2, fill=INK, font=FONT_BOLD):
    add(text(s, x, y, size, font, fill, spacing=0.05))


add(f'<rect width="{W}" height="{H}" fill="{SOIL}"/>')
# faint horizontal strata
for i, y in enumerate(range(14, 128, 6)):
    add(f'<line x1="0" y1="{y}" x2="{W}" y2="{y + (1.5 if i % 2 else -1.0)}" stroke="{SOIL_DARK}" stroke-width="0.9" opacity="0.55"/>')

# title
add(text("DUNG REVISIONS", 4.0, 10.6, 4.1, FONT_BOLD, INK, anchor="start", spacing=0.08))
add(text("divide  /  bus  /  rung", W - 4.0, 10.3, 2.6, FONT_MED, SOFT, anchor="end"))
add(text("Haer", W / 2 - 34.0, 126.0, 2.0, FONT_BOLD, SOFT, spacing=0.12))

# display window
add(f'<rect x="73.4" y="12.4" width="65.2" height="105.2" rx="1.6" fill="{WINDOW}"/>')

# divisions: /8 at the top down to /2
SW_X, DIV_X, Y0, DY = 6.5, 16.5, 20.0, 12.0
for r in range(7):
    y = Y0 + r * DY
    ring(DIV_X, y, GATE)
    add(text(f"/{8 - r}", DIV_X + 5.3, y + 1.0, 2.9, FONT_BOLD, INK, anchor="start"))
add(text("up: bus 1", 11.0, 101.0, 2.4, FONT_MED, GATE))
add(text("mid: off", 11.0, 103.9, 2.4, FONT_MED, SOFT))
add(text("down: bus 2", 11.0, 106.8, 2.4, FONT_MED, BUS2))

# outputs, top right
for x, s, c in [(33.0, "Bus 1", GATE), (45.0, "Bus 2", GATE), (57.0, "Noise", CV)]:
    ring(x, 20.0, c)
    label(s, x, 28.4, 2.9)
for x, s, c in [(33.0, "1-Bit", GATE), (45.0, "3-Bit", CV), (57.0, "8-Bit", CV)]:
    ring(x, 34.0, c)
    label(s, x, 42.4, 2.9)

# length knob ring labels
KX, LY = 54.0, 58.0
for i, s in enumerate(["2", "3", "4", "5", "6", "7", "8", "16"]):
    a = math.radians(-150 + 300 * i / 7)
    rl = 12.6
    add(text(s, KX + rl * math.sin(a), LY - rl * math.cos(a) + 0.9, 2.6, FONT_BOLD, INK))

# direction / length jacks
label("Direction", 35.0, 80.9, 3.2)
label("Length", KX, 80.9, 3.2)
add(text("fwd / rev", 35.0, 64.6, 2.4, FONT_MED, SOFT))

# write switch and chance knob
label("High", 35.0, 86.4, 2.9)
label("Low", 35.0, 100.8, 2.9)
CY = 92.0
for v, s in [(0.0, "new"), (1.0, "loop")]:
    a = math.radians(-150 + 300 * v)
    r0, r1 = 7.0, 8.0
    add(f'<line x1="{KX + r0 * math.sin(a):.3f}" y1="{CY - r0 * math.cos(a):.3f}" x2="{KX + r1 * math.sin(a):.3f}" '
        f'y2="{CY - r1 * math.cos(a):.3f}" stroke="{SOFT}" stroke-width="0.35"/>')
    add(text(s, KX + 9.6 * math.sin(a), CY - 9.6 * math.cos(a) + 1.4, 2.4, FONT_MED, SOFT))

# bottom row inputs
for x, s in [(10.0, "Clock"), (22.0, "Reset"), (38.0, "Data"), (52.0, "Chance")]:
    label(s, x, 119.6, 2.9)

svg = (
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" '
    f'viewBox="0 0 {W} {H}">\n' + "\n".join(parts) + "\n</svg>\n"
)
with open(os.path.join(HERE, "DungRevisions.svg"), "w") as fh:
    fh.write(svg)
print("wrote DungRevisions.svg")
