#!/usr/bin/env python3
"""Canvas look tokens from an Omarchy theme: the one source for docs/canvas-look.md.

The numbers in the spec, the colors in the HTML mockups and the contrast table all come from
here, so they cannot drift apart. Step 2 of #47 reproduces `tokens()` and `port_colors()` in the
C++ Theme (src/theme.cpp); this file is the reference it is checked against.

    python3 docs/canvas-look/derive.py --table        # contrast table for every installed theme
    python3 docs/canvas-look/derive.py --write        # refresh the token block in the mockups
    python3 docs/canvas-look/derive.py --theme NAME   # print one theme's tokens

The table covers Omarchy's shipped themes (/usr/share/omarchy/themes); --theme also finds the
user's own (~/.config/omarchy/themes). Both are parsed as
Theme::readThemeColors parses them (same aliases, same light/dark rule).
"""
import argparse
import glob
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
THEME_DIRS = ["/usr/share/omarchy/themes", os.path.expanduser("~/.config/omarchy/themes")]
# Mockup themes: the owner's (ethereal, dark), a common dark, the weakest light, a strong light,
# and a monochrome one. Each gets a [data-theme] block in every mockup.
MOCKUP_THEMES = ["ethereal", "tokyo-night", "rose-pine", "flexoki-light", "white"]

ALIASES = [("background", "bg", "color0"), ("foreground", "fg", "color7"), ("red", "color1"),
           ("green", "color2"), ("yellow", "color3"), ("blue", "color4"),
           ("magenta", "color5", "purple"), ("cyan", "color6"), ("muted", "color8", "dark_foreground"),
           ("selection", "selection_background", "color8", "background"), ("orange", "yellow")]
FALLBACK = {"accent": "#FFD60A", "background": "#0e0e10", "foreground": "#e6e6e9", "red": "#f7768e",
            "green": "#9ece6a", "yellow": "#e0af68", "orange": "#ff9e64", "blue": "#7aa2f7",
            "magenta": "#bb9af7", "cyan": "#7dcfff"}
# Theme::portColor's mapping (spec 12.6), and the order in which a port type keeps its theme
# color when two collide: the most used types first.
PORT_ROLE = {"text": "green", "number": "blue", "boolean": "orange", "image": "magenta",
             "video": "cyan", "audio": "yellow"}
KEEP_ORDER = ["image", "text", "number", "video", "audio", "boolean", "declared"]
# Every surface `label` text is drawn on.
LABEL_SURFACES = ["body", "header", "well", "popup", "selected-row", "menu-row", "canvas"]
# Two port colors closer than this OKLab distance count as the same color.
MIN_DE = 0.08
# OKLCH hue a port type falls back to when its theme color collides with one kept before it.
FALLBACK_HUE = {"text": 145, "number": 255, "boolean": 55, "image": 330, "video": 205, "audio": 90,
                "declared": 290}


# ---- color math (sRGB, WCAG contrast, OKLab/OKLCH) ----
def hexc(s):
    s = s.lstrip("#")
    return tuple(int(s[i:i + 2], 16) / 255 for i in (0, 2, 4))


def tohex(c):
    return "#" + "".join("%02x" % round(max(0.0, min(1.0, v)) * 255) for v in c)


def _lin(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def _unlin(v):
    return 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055


def luminance(c):
    r, g, b = map(_lin, c)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def mix(a, b, t):
    """Theme.cpp's mix(): a straight sRGB blend, t of the way from a to b."""
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def oklab(c):
    r, g, b = map(_lin, c)
    l = (0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b) ** (1 / 3)
    m = (0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b) ** (1 / 3)
    s = (0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b) ** (1 / 3)
    return (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)


def _from_oklab_raw(L, a, b):
    l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
    m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
    s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
    return (4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)


def oklch(c):
    L, a, b = oklab(c)
    return L, math.hypot(a, b), math.degrees(math.atan2(b, a)) % 360


def from_oklch(L, C, h):
    """The color at L and h, with chroma reduced until it fits sRGB."""
    while C > 0:
        rgb = _from_oklab_raw(L, C * math.cos(math.radians(h)), C * math.sin(math.radians(h)))
        if all(-1e-4 <= v <= 1 + 1e-4 for v in rgb):
            return tuple(_unlin(max(0.0, min(1.0, v))) for v in rgb)
        C -= 0.005
    return tuple(_unlin(max(0.0, min(1.0, v))) for v in _from_oklab_raw(L, 0, 0))


def delta_e(a, b):
    return math.dist(oklab(a), oklab(b))


def lift(col, against, target=3.0):
    """Move only OKLCH lightness (away from `against`) until `col` reaches `target`:1."""
    if contrast(col, against) >= target:
        return col
    L, C, h = oklch(col)
    step = 0.005 if luminance(against) < 0.18 else -0.005
    for i in range(1, 200):
        c2 = from_oklch(L + step * i, C, h)
        if contrast(c2, against) >= target:
            return c2
    return col


# ---- reading a theme like Theme::readThemeColors ----
ENTRY = re.compile(r"""^\s*([a-z0-9_]+)\s*=\s*(["']?)(#[0-9a-fA-F]{6}|light|dark)\2\s*(#.*)?$""")


def read_theme(path):
    colors, modes = {}, {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = ENTRY.match(line.strip())
            if not m:
                continue
            key, value = m.group(1), m.group(3)
            if value.startswith("#"):
                colors[key] = hexc(value)
            else:
                modes[key] = value
    for names in ALIASES:
        for name in names[1:]:
            if names[0] not in colors and name in colors:
                colors[names[0]] = colors[name]
    mode = modes.get("mode", modes.get("theme_type"))
    if not mode and os.path.exists(os.path.join(os.path.dirname(path), "light.mode")):
        mode = "light"
    if not mode and "background" in colors:
        mode = "light" if sum(round(v * 255) for v in colors["background"]) > 382 else "dark"
    for k, v in FALLBACK.items():
        colors.setdefault(k, hexc(v))
    return colors, mode == "light"


def all_themes(user=False):
    """Omarchy's shipped themes; with `user`, the user's own themes too."""
    found = {}
    for d in THEME_DIRS if user else THEME_DIRS[:1]:
        for p in sorted(glob.glob(os.path.join(d, "*", "colors.toml"))):
            found.setdefault(p.split("/")[-2], p)
    return found


# ---- the canvas tokens (docs/canvas-look.md, "Color") ----
WHITE = (1.0, 1.0, 1.0)
BLACK = (0.0, 0.0, 0.0)


def tokens(colors, light):
    bg, fg, red = colors["background"], colors["foreground"], colors["red"]
    t = {}
    if light:
        t["canvas"] = mix(bg, fg, 0.045)          # a shade under the window, so white cards lift
        t["body"] = mix(bg, WHITE, 0.6)           # nodes are lighter than the canvas
        t["header"] = mix(t["body"], fg, 0.05)
        t["hairline"] = mix(t["body"], fg, 0.13)
        t["hairline-strong"] = mix(t["body"], fg, 0.26)
        t["well"] = mix(t["body"], fg, 0.05)      # widget fields sit a shade into the body
        t["dot"] = mix(t["canvas"], fg, 0.15)
        t["dot-major"] = mix(t["canvas"], fg, 0.24)
        t["bevel"] = (1.0, 1.0, 1.0, 0.0)         # no top highlight on light themes
        t["scrim"] = (0.0, 0.0, 0.0, 0.22)
    else:
        t["canvas"] = bg
        t["body"] = mix(bg, fg, 0.075)
        t["header"] = mix(bg, fg, 0.13)
        t["hairline"] = mix(bg, fg, 0.17)
        t["hairline-strong"] = mix(bg, fg, 0.30)
        t["well"] = mix(bg, fg, 0.035)           # darker than the body: recessed
        t["dot"] = mix(t["canvas"], fg, 0.13)
        t["dot-major"] = mix(t["canvas"], fg, 0.22)
        t["bevel"] = fg + (0.10,)                 # 1 px highlight along the card's top edge (#63)
        t["scrim"] = (0.0, 0.0, 0.0, 0.45)
    t["text"] = fg
    t["accent"] = colors["accent"]
    t["accent-fg"] = BLACK if luminance(colors["accent"]) > 0.179 else WHITE
    t["error"] = red
    t["error-header"] = mix(t["header"], red, 0.20 if not light else 0.14)
    t["error-chip"] = mix(t["body"], red, 0.14 if not light else 0.10)
    t["selected-row"] = mix(t["body"], colors["accent"], 0.16 if not light else 0.12)
    t["popup"] = t["body"] if light else mix(bg, fg, 0.09)
    # The current row of a menu on the popup surface: a clear accent fill, lessened until the text
    # reads on it at 4.5:1 (as look.cpp).
    for k in (0.32, 0.28, 0.24, 0.20, 0.16, 0.12, 0.08):
        t["menu-row"] = mix(t["popup"], colors["accent"], k)
        if contrast(fg, t["menu-row"]) >= 4.5:
            break
    # The sheen (#72): the least alpha of light (the text on dark themes, white on light ones)
    # that makes a step of OKLab 0.012 from the top of a node's fill to its bottom; on a fill
    # with no room to lighten, of black at the bottom instead. Lit (hovered, selected): twice.
    def sheen(over, step):
        for i in range(1, 121):  # alpha .005 to .6, in steps of .005
            a = i * 0.005
            if delta_e(mix(t["body"], over, a), t["body"]) >= step:
                return over + (round(a, 3),)
        return over + (0.0,)
    for name, step in (("sheen", 0.012), ("lit", 0.024)):
        top = sheen(WHITE if light else fg, step)
        bottom = BLACK + (0.0,)
        if top[3] == 0:
            bottom = sheen(BLACK, step)
        t[f"{name}-top"], t[f"{name}-bottom"] = top, bottom
        t[f"_{name}-top-over"] = mix(t["body"], top[:3], top[3])
        t[f"_{name}-bottom-over"] = mix(t["body"], bottom[:3], bottom[3])
    sheened = ["_sheen-top-over", "_sheen-bottom-over", "_lit-top-over", "_lit-bottom-over"]
    # Labels and captions sit on all of these; the share of text grows from its base until the
    # label reads at 4.5:1 on every one of them.
    for k in [b / 100 for b in range(87 if light else 80, 101, 2)]:
        t["label"] = mix(t["body"], fg, k)
        t["_label-share"] = k
        if min(contrast(t["label"], t[s]) for s in LABEL_SURFACES + sheened) >= 4.5:
            break
    # The Drift kit (docs/canvas-look.md "Drift kit"), as look.cpp.
    t["kit-group"] = mix(t["popup"], fg, 0.045 if light else 0.07)
    t["kit-separator"] = mix(t["kit-group"], fg, 0.10)
    t["kit-track"] = mix(t["kit-group"], fg, 0.30 if light else 0.28)
    t["kit-knob"] = WHITE if light else fg
    t["kit-chip"] = mix(t["kit-group"], fg, 0.10)
    t["kit-chip-hover"] = mix(t["kit-group"], fg, 0.17)
    t["kit-hero-shade"], t["kit-hero-pill"], t["kit-hero-pill-hover"] = (0, 0, 0, 0.55), (0, 0, 0, 0.44), (0, 0, 0, 0.60)
    # Frame fills and node-color header tints per theme role (layout colors are roles, spec 4.5).
    # A tint steps down until the title on it still reads at 4.5:1.
    for role in ("muted", "blue", "green", "magenta", "orange"):
        rc = t["label"] if role == "muted" else colors[role]
        t[f"frame-{role}"] = mix(t["canvas"], rc, 0.12 if light else 0.10)
        t[f"frame-{role}-edge"] = mix(t["canvas"], rc, 0.35)
        for k in (0.5, 0.6, 0.7, 0.8, 0.9, 1.0):
            t[f"frame-{role}-title"] = mix(rc, fg, k)
            if contrast(t[f"frame-{role}-title"], t[f"frame-{role}"]) >= 4.5:
                break
        for k in ((0.14, 0.10, 0.06) if light else (0.18, 0.14, 0.10, 0.06)):
            t[f"tint-{role}"] = mix(t["header"], rc, k)
            if contrast(fg, t[f"tint-{role}"]) >= 4.5:
                break
        # The body takes the color too (#72): each tint steps down until the text, and the
        # label as well as it reads on the plain body, read on it.
        # Text and label read at every end of the sheen over it too.
        label_need = min(4.5, contrast(t["label"], t["body"]))
        layers = [(0, 0, 0, 0.0)] + [t[k] for k in ("sheen-top", "sheen-bottom", "lit-top", "lit-bottom")]
        for k in ((0.18, 0.14, 0.10, 0.06) if light else (0.22, 0.18, 0.14, 0.10, 0.06)):
            fill = mix(t["body"], rc, k)
            t[f"tint-body-{role}"] = fill
            ends = [mix(fill, layer[:3], layer[3]) for layer in layers]
            if all(contrast(fg, e) >= 4.5 and contrast(t["label"], e) >= label_need for e in ends):
                break
    return t


def port_colors(colors, light, t):
    """Theme port colors made distinct and readable (the proposed Theme::portColor).

    1. A monochrome theme (no port color has OKLCH chroma 0.04 or more) keeps its grays: shape
       and label carry the type there.
    2. Otherwise, in KEEP_ORDER, a type keeps its theme color unless it is gray or within OKLab
       distance MIN_DE of one already kept; then it takes FALLBACK_HUE (or the nearest hue, in
       30 degree steps, that is distinct) at the theme's median port lightness and chroma.
    3. Each color's lightness alone moves until it reaches 3:1 against the node body and the
       canvas (non-text contrast), so marks and wires read on light themes too.
    """
    pal = {k: colors.get(v, colors["foreground"]) for k, v in PORT_ROLE.items()}
    pal["declared"] = colors["accent"]
    chromatic = [oklch(v) for v in pal.values() if oklch(v)[1] >= 0.04]
    mono = not chromatic
    out = {}
    if mono:
        out = dict(pal)
    else:
        Lm = sorted(x[0] for x in chromatic)[len(chromatic) // 2]
        Cm = max(0.12, sorted(x[1] for x in chromatic)[len(chromatic) // 2])

        def distinct(a, b):
            return delta_e(a, b) >= MIN_DE

        for k in KEEP_ORDER:
            col = pal[k]
            if oklch(col)[1] < 0.04 or any(not distinct(col, o) for o in out.values()):
                # The type's own hue first, then its neighbours, 30 degrees at a time; failing
                # all, the most distinct of them, or for declared types the neutral label color
                # (their diamond already sets them apart).
                best, best_d = None, -1.0
                for d in (0, 30, -30, 60, -60, 90, -90, 120, -120, 150, -150, 180):
                    cand = from_oklch(Lm, Cm, (FALLBACK_HUE[k] + d) % 360)
                    near = min((delta_e(cand, o) for o in out.values()), default=1.0)
                    if near > best_d:
                        best, best_d = cand, near
                    if near >= MIN_DE:
                        break
                col = best if best_d >= MIN_DE or k != "declared" else t["label"]
            out[k] = col
    # Against whichever of body and canvas is nearer the color's own luminance.
    return {k: lift(lift(v, t["body"]), t["canvas"]) for k, v in out.items()}, mono


def css_block(name, colors, light):
    t = tokens(colors, light)
    ports, _ = port_colors(colors, light, t)
    lines = [f'[data-theme="{name}"] {{', f"  color-scheme: {'light' if light else 'dark'};"]
    for k, v in t.items():
        if k.startswith("_"):
            continue
        if len(v) == 4:
            lines.append(f"  --{k}: rgba({round(v[0]*255)}, {round(v[1]*255)}, {round(v[2]*255)}, {v[3]});")
        else:
            lines.append(f"  --{k}: {tohex(v)};")
    for k, v in ports.items():
        lines.append(f"  --port-{k}: {tohex(v)};")
    # Raw palette roles a mockup may name (frame and node colors are theme roles, spec 4.5).
    for role in ("red", "green", "yellow", "orange", "blue", "magenta", "cyan"):
        lines.append(f"  --role-{role}: {tohex(colors[role])};")
    # Shadows (docs/canvas-look.md, "Elevation"): the same values the 9-slice images bake.
    if light:
        lines += ["  --shadow-1: 0 1px 3px rgba(0,0,0,.12), 0 6px 18px rgba(0,0,0,.12);",
                  "  --shadow-2: 0 2px 5px rgba(0,0,0,.14), 0 12px 32px rgba(0,0,0,.18);",
                  "  --shadow-3: 0 4px 10px rgba(0,0,0,.08), 0 18px 48px rgba(0,0,0,.20);",
                  "  --shadow-4: 0 3px 8px rgba(0,0,0,.14), 0 20px 48px rgba(0,0,0,.24);"]
    else:
        lines += ["  --shadow-1: 0 2px 4px rgba(0,0,0,.75), 0 8px 24px rgba(0,0,0,.65);",
                  "  --shadow-2: 0 3px 8px rgba(0,0,0,.80), 0 16px 40px rgba(0,0,0,.75);",
                  "  --shadow-3: 0 6px 16px rgba(0,0,0,.50), 0 20px 56px rgba(0,0,0,.55);",
                  "  --shadow-4: 0 4px 10px rgba(0,0,0,.80), 0 24px 60px rgba(0,0,0,.80);"]
    lines.append("}")
    return "\n".join(lines)


def write_mockups():
    themes = all_themes()
    blocks = []
    for name in MOCKUP_THEMES:
        colors, light = read_theme(themes[name])
        blocks.append(css_block(name, colors, light))
    block = "/* tokens:begin (generated by derive.py --write; do not edit) */\n" + "\n".join(blocks) \
            + "\n/* tokens:end */"
    with open(os.path.join(HERE, "src", "look.css"), encoding="utf-8") as f:
        css = "/* look-css:begin (src/look.css, inlined by derive.py --write) */\n" + f.read() + "/* look-css:end */"
    with open(os.path.join(HERE, "src", "look.js"), encoding="utf-8") as f:
        js = "// look-js:begin (src/look.js, inlined by derive.py --write)\n" + f.read() + "// look-js:end"
    regions = [(re.compile(r"/\* tokens:begin.*?/\* tokens:end \*/", re.S), block),
               (re.compile(r"/\* look-css:begin.*?/\* look-css:end \*/", re.S), css),
               (re.compile(r"// look-js:begin.*?// look-js:end", re.S), js)]
    for path in sorted(glob.glob(os.path.join(HERE, "*.html"))):
        with open(path, encoding="utf-8") as f:
            html = f.read()
        for pat, text in regions:
            html = pat.sub(lambda _, text=text: text, html)
        with open(path, "w", encoding="utf-8") as f:
            f.write(html)
        print("wrote", os.path.relpath(path))


def table():
    rows = []
    for name, path in all_themes().items():
        colors, light = read_theme(path)
        t = tokens(colors, light)
        ports, mono = port_colors(colors, light, t)
        raw = {k: colors.get(v, colors["foreground"]) for k, v in PORT_ROLE.items()}
        raw["declared"] = colors["accent"]
        keys = list(raw)
        raw_de = min(delta_e(raw[a], raw[b]) for i, a in enumerate(keys) for b in keys[i + 1:])
        new_de = min(delta_e(ports[a], ports[b]) for i, a in enumerate(keys) for b in keys[i + 1:])
        worst = min(LABEL_SURFACES, key=lambda s: contrast(t["label"], t[s]))
        roles = ("blue", "green", "magenta", "orange")
        rows.append((name, "light" if light else "dark",
                     contrast(t["text"], t["header"]), "%.2f" % t["_label-share"],
                     "%.2f (%s)" % (contrast(t["label"], t[worst]), worst),
                     contrast(t["accent"], t["canvas"]),
                     min(contrast(raw[k], t["body"]) for k in PORT_ROLE),
                     min(min(contrast(v, t["body"]), contrast(v, t["canvas"])) for v in ports.values()),
                     raw_de, "mono" if mono else "%.2f" % new_de,
                     contrast(t["error"], t["canvas"]), contrast(t["text"], t["error-chip"]),
                     min(contrast(t["text"], t[f"tint-{r}"]) for r in roles),
                     min(contrast(t[f"frame-{r}-title"], t[f"frame-{r}"]) for r in roles + ("muted",))))
    print("| Theme | Mode | Title/header | Label share | Label, worst surface | Accent/canvas "
          "| Port/body (theme) | Port (lifted) | Port ΔE (theme) | Port ΔE (resolved) | Error/canvas "
          "| Text/error chip | Title/tinted header | Frame title/fill |")
    print("|" + " --- |" * 14)
    for r in rows:
        cells = [r[0], r[1]] + ["%.2f" % v if isinstance(v, float) else v for v in r[2:]]
        print("| " + " | ".join(cells) + " |")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--table", action="store_true")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--theme")
    a = ap.parse_args()
    if a.write:
        write_mockups()
    if a.table:
        table()
    if a.theme:
        colors, light = read_theme(all_themes(user=True)[a.theme])
        print(css_block(a.theme, colors, light))
    if not (a.write or a.table or a.theme):
        ap.print_help()


if __name__ == "__main__":
    sys.exit(main())
