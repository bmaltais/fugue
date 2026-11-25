#!/usr/bin/env python3
"""Builds the Fugue icon and wordmark SVGs (share/brand, share/icons/hicolor/scalable) from Fraunces outlines.

usage: python -I scripts/make-app-icon.py FRAUNCES-VF.ttf [OUT-ROOT]
Needs fontTools. The font is SIL OFL 1.1 and is not stored in the repo; only the outlines are.
"""
import sys
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.boundsPen import BoundsPen

CREDIT = "Letterforms outlined from Fraunces (SIL Open Font License 1.1, https://github.com/undercasetype/Fraunces)."


def instance(path, wght):
    return instantiateVariableFont(TTFont(path), {"opsz": 144, "wght": wght, "SOFT": 0, "WONK": 0})


def outline(font, text, size, x=0.0, y=0.0):
    """Path data for text, baseline at y, left edge at x, em = size px (kerning ignored for one glyph)."""
    gs, cmap, hmtx = font.getGlyphSet(), font.getBestCmap(), font["hmtx"]
    s = size / font["head"].unitsPerEm
    pen = SVGPathPen(gs, ntos=lambda v: ("%.2f" % v).rstrip("0").rstrip("."))
    adv = 0.0
    for ch in text:
        n = cmap[ord(ch)]
        gs[n].draw(TransformPen(pen, (s, 0, 0, -s, x + adv, y)))
        adv += hmtx[n][0] * s
    return pen.getCommands(), adv


def glyph_bounds(font, ch, s):
    gs = font.getGlyphSet()
    bp = BoundsPen(gs)
    gs[font.getBestCmap()[ord(ch)]].draw(bp)
    x0, y0, x1, y1 = bp.bounds
    return x0 * s, y0 * s, x1 * s, y1 * s


def icon(font, cap_frac=0.575):
    upm = font["head"].unitsPerEm
    cap = font["OS/2"].sCapHeight
    size = 512 * cap_frac / (cap / upm)
    s = size / upm
    x0, y0, x1, y1 = glyph_bounds(font, "F", s)
    x = 256 - (x0 + x1) / 2
    y = 256 + (y0 + y1) / 2  # baseline so that the F's box is centred
    d, _ = outline(font, "F", size, x, y)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">
  <title>Fugue</title>
  <desc>{CREDIT}</desc>
  <defs>
    <radialGradient id="lilac" gradientUnits="userSpaceOnUse" cx="128" cy="154" r="358">
      <stop offset="0" stop-color="#b98bd6"/><stop offset="1" stop-color="#b98bd6" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="rose" gradientUnits="userSpaceOnUse" cx="399" cy="143" r="307">
      <stop offset="0" stop-color="#e3828f"/><stop offset="1" stop-color="#e3828f" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="blue" gradientUnits="userSpaceOnUse" cx="358" cy="486" r="410">
      <stop offset="0" stop-color="#6f86d6"/><stop offset="1" stop-color="#6f86d6" stop-opacity="0"/>
    </radialGradient>
    <clipPath id="tile"><rect width="512" height="512" rx="118" ry="118"/></clipPath>
  </defs>
  <g clip-path="url(#tile)">
    <rect width="512" height="512" fill="#2a2533"/>
    <rect width="512" height="512" fill="url(#lilac)"/>
    <rect width="512" height="512" fill="url(#rose)"/>
    <rect width="512" height="512" fill="url(#blue)"/>
  </g>
  <path fill="#ffffff" d="{d}"/>
</svg>
"""


def wordmark(font, color):
    size = 160
    d, adv = outline(font, "Fugue", size, 0, 0)
    gs = font.getGlyphSet()
    w, h = int(adv) + 24, 230
    base = 150
    d, adv = outline(font, "Fugue", size, 12, base)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">
  <title>Fugue</title>
  <desc>{CREDIT}</desc>
  <path fill="{color}" d="{d}"/>
</svg>
"""


if __name__ == "__main__":
    root = Path(sys.argv[2] if len(sys.argv) > 2 else ".")
    f = instance(sys.argv[1], 450)
    (root / "share/icons/hicolor/scalable/apps").mkdir(parents=True, exist_ok=True)
    (root / "share/brand").mkdir(parents=True, exist_ok=True)
    (root / "share/icons/hicolor/scalable/apps/fugue.svg").write_text(icon(f))
    # Smaller sizes: a heavier cut keeps the F's stems two or more pixels wide.
    (root / "share/brand/fugue-icon-small.svg").write_text(icon(instance(sys.argv[1], 600), 0.6))
    wm = instance(sys.argv[1], 400)
    (root / "share/brand/fugue-wordmark-white.svg").write_text(wordmark(wm, "#ffffff"))
    (root / "share/brand/fugue-wordmark-dark.svg").write_text(wordmark(wm, "#2a2533"))
