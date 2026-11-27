#include "look.h"

#include "engine/layout_color.h"

#include <QBuffer>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fugue::look {

namespace {

// The sheen's step from the top of a node's fill to its bottom, in OKLab (#72).
constexpr double kSheenStep = 0.012;

// A color with alpha laid over an opaque fill.
QColor over(const QColor &fill, const QColor &layer) { return mix(fill, QColor(layer.rgb()), layer.alphaF()); }

// Whether `fg` (at 4.5:1) and `label` (at `labelNeed`) read on `fill` at every end of the sheen.
bool readsUnderSheen(const Tokens &t, const QColor &fill, const QColor &fg, double labelNeed) {
    for (const QColor &layer : {QColor(0, 0, 0, 0), t.sheenTop, t.sheenBottom, t.litTop, t.litBottom})
        if (contrast(fg, over(fill, layer)) < 4.5 || contrast(t.label, over(fill, layer)) < labelNeed)
            return false;
    return true;
}


double linear(double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); }
double gamma(double v) { return v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055; }
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
constexpr double pi = 3.14159265358979323846;

struct Lab {
    double L, a, b;
};

Lab oklab(const QColor &c) {
    const double r = linear(c.redF()), g = linear(c.greenF()), b = linear(c.blueF());
    const double l = std::cbrt(.4122214708 * r + .5363325363 * g + .0514459929 * b);
    const double m = std::cbrt(.2119034982 * r + .6806995451 * g + .1073969566 * b);
    const double s = std::cbrt(.0883024619 * r + .2817188376 * g + .6299787005 * b);
    return {.2104542553 * l + .7936177850 * m - .0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + .4505937099 * s,
            .0259040371 * l + .7827717662 * m - .8086757660 * s};
}

// Linear sRGB, possibly out of gamut.
std::array<double, 3> fromOklabRaw(double L, double a, double b) {
    const double l = std::pow(L + .3963377774 * a + .2158037573 * b, 3);
    const double m = std::pow(L - .1055613458 * a - .0638541728 * b, 3);
    const double s = std::pow(L - .0894841775 * a - 1.2914855480 * b, 3);
    return {4.0767416621 * l - 3.3077115913 * m + .2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - .3413193965 * s,
            -.0041960863 * l - .7034186147 * m + 1.7076147010 * s};
}

QColor fromLinear(const std::array<double, 3> &rgb) {
    return QColor::fromRgbF(float(gamma(clamp01(rgb[0]))), float(gamma(clamp01(rgb[1]))), float(gamma(clamp01(rgb[2]))));
}

// Theme roles a port type takes its color from (Theme::portColor, spec 12.6), the order in
// which colliding types keep theirs (the most used first), and the hue each falls back to.
const QList<QPair<QString, QString>> &portRoles() {
    static const QList<QPair<QString, QString>> roles{{"text", "green"},    {"number", "blue"},  {"boolean", "orange"},
                                                      {"image", "magenta"}, {"video", "cyan"},   {"audio", "yellow"}};
    return roles;
}
const QStringList keepOrder{"image", "text", "number", "video", "audio", "boolean", "declared"};
const QHash<QString, double> fallbackHue{{"text", 145},  {"number", 255}, {"boolean", 55}, {"image", 330},
                                         {"video", 205}, {"audio", 90},   {"declared", 290}};
constexpr double minDeltaE = .08;
constexpr double minChroma = .04;

} // namespace

double luminance(const QColor &c) {
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}

double contrast(const QColor &a, const QColor &b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + .05) / (std::min(la, lb) + .05);
}

QColor mix(const QColor &a, const QColor &b, double t) {
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

Oklch oklch(const QColor &c) {
    const Lab lab = oklab(c);
    double h = std::atan2(lab.b, lab.a) * 180 / pi;
    if (h < 0)
        h += 360;
    return {lab.L, std::hypot(lab.a, lab.b), h};
}

QColor fromOklch(double L, double C, double h) {
    const double rad = h * pi / 180;
    while (C > 0) {
        const auto rgb = fromOklabRaw(L, C * std::cos(rad), C * std::sin(rad));
        if (std::all_of(rgb.begin(), rgb.end(), [](double v) { return v >= -1e-4 && v <= 1 + 1e-4; }))
            return fromLinear(rgb);
        C -= .005;
    }
    return fromLinear(fromOklabRaw(L, 0, 0));
}

QColor fromOklch(const Oklch &c) { return fromOklch(c.L, c.C, c.h); }

double deltaE(const QColor &a, const QColor &b) {
    const Lab x = oklab(a), y = oklab(b);
    return std::sqrt((x.L - y.L) * (x.L - y.L) + (x.a - y.a) * (x.a - y.a) + (x.b - y.b) * (x.b - y.b));
}

QColor lift(const QColor &color, const QColor &against, double target) {
    if (contrast(color, against) >= target)
        return color;
    const Oklch c = oklch(color);
    const double step = luminance(against) < .18 ? .005 : -.005;
    for (int i = 1; i < 200; ++i) {
        const QColor moved = fromOklch(c.L + step * i, c.C, c.h);
        if (contrast(moved, against) >= target)
            return moved;
    }
    return color;
}

// The tab colors: the theme's own hues when it has any, else six fixed hues at the accent's lightness.
QVariantList tabColors(const QHash<QString, QColor> &colors, const Tokens &t) {
    const QStringList roles{"red", "yellow", "green", "cyan", "blue", "magenta"};
    const double hues[] = {25, 95, 145, 205, 255, 330};
    bool chromatic = false;
    for (const QString &role : roles)
        chromatic = chromatic || oklch(colors.value(role)).C >= minChroma;
    const Oklch accent = oklch(colors.value("accent"));
    QVariantList out;
    for (int i = 0; i < roles.size(); ++i) {
        QColor color = colors.value(roles[i]);
        if (!chromatic || oklch(color).C < minChroma)
            color = fromOklch(std::clamp(accent.L, .45, .8), .13, hues[i]);
        out.append(lift(lift(color, t.well, 3.0), t.body, 3.0));
    }
    return out;
}

Tokens tokens(const QHash<QString, QColor> &colors, bool light) {
    const QColor bg = colors.value("background"), fg = colors.value("foreground"), red = colors.value("red");
    const QColor accent = colors.value("accent");
    Tokens t;
    if (light) {
        // The canvas a shade under the window, so white cards lift off it (owner's decision 18.5).
        t.canvas = mix(bg, fg, .045);
        t.body = mix(bg, Qt::white, .6);
        t.header = mix(t.body, fg, .05);
        t.hairline = mix(t.body, fg, .13);
        t.hairlineStrong = mix(t.body, fg, .26);
        t.well = mix(t.body, fg, .05);
        t.dot = mix(t.canvas, fg, .15);
        t.dotMajor = mix(t.canvas, fg, .24);
        t.bevel = QColor(255, 255, 255, 0);
        t.scrim = QColor::fromRgbF(0, 0, 0, .22f);
        t.errorHeader = mix(t.header, red, .14);
        t.errorChip = mix(t.body, red, .10);
        t.selectedRow = mix(t.body, accent, .12);
        t.popup = t.body;
    } else {
        t.canvas = bg;
        t.body = mix(bg, fg, .075);
        t.header = mix(bg, fg, .13);
        t.hairline = mix(bg, fg, .17);
        t.hairlineStrong = mix(bg, fg, .30);
        t.well = mix(bg, fg, .035); // darker than the body: recessed
        t.dot = mix(t.canvas, fg, .13);
        t.dotMajor = mix(t.canvas, fg, .22);
        t.bevel = fg;
        t.bevel.setAlphaF(.10f);
        t.scrim = QColor::fromRgbF(0, 0, 0, .45f);
        t.errorHeader = mix(t.header, red, .20);
        t.errorChip = mix(t.body, red, .14);
        t.selectedRow = mix(t.body, accent, .16);
        t.popup = mix(bg, fg, .09);
    }
    // The current row of a menu (hovered or by key) on the popup surface: a clear accent fill,
    // lessened until the text reads on it at 4.5:1.
    for (const double k : {.32, .28, .24, .20, .16, .12, .08}) {
        t.menuRow = mix(t.popup, accent, k);
        if (contrast(fg, t.menuRow) >= 4.5)
            break;
    }
    // A button that cancels or deletes (#120): the well tinted red on the popup surface, the text
    // reading on it at 4.5:1; the hover a step redder, lessened by the same rule.
    for (const double k : {.30, .26, .22, .18, .14, .10}) {
        t.dangerFill = mix(t.popup, red, k);
        if (contrast(fg, t.dangerFill) >= 4.5)
            break;
    }
    for (const double k : {.46, .40, .34, .28, .22, .16}) {
        t.dangerHover = mix(t.popup, red, k);
        if (contrast(fg, t.dangerHover) >= 4.5)
            break;
    }
    // A dialog's destructive answer: the theme's red as a solid fill, so it looks alike in light and dark themes, with the ink
    // that reads on it best: white, or the theme's dark colour (the background of a dark theme, the text of a light one), at 4.5:1. Only
    // when neither reaches it is the fill darkened (white ink) or lightened (dark ink) in 5% steps, whichever needs fewer. The hover
    // is a step stronger: 12% away from the ink.
    {
        // The red itself, when it reads as red and apart from the accent: some chroma (tabColors' floor), an OKLCH hue within 0-50 or
        // 330-360 degrees and an OKLab distance of .1 from the accent; else the theme's bright red by the same test; else a fixed red
        // at the theme red's lightness held to .50-.65 (hackerman's green red, vantablack's and solitude's grey ones, white's black).
        const auto readsRed = [&](const QColor &c) {
            if (!c.isValid())
                return false;
            const Oklch o = oklch(c);
            return o.C >= minChroma && (o.h <= 50 || o.h >= 330) && deltaE(c, accent) >= .1;
        };
        // The fill and its ink for a red: the red itself with the ink that reads better (white, or the theme's dark colour) at 4.5:1;
        // only when neither does, the red darkened (white ink) or lightened (dark ink) in 5% steps, whichever needs fewer.
        const QColor white(Qt::white);
        const QColor dark = light ? fg : bg;
        const auto filled = [&](const QColor &base) {
            const bool whiteInk = contrast(white, base) >= contrast(dark, base);
            QColor ink = whiteInk ? white : dark, fill = base;
            if (contrast(ink, base) < 4.5) {
                QColor darker = base, lighter = base;
                int down = 0, up = 0;
                while (down < 20 && contrast(white, darker) < 4.5)
                    darker = mix(base, QColor(Qt::black), ++down * .05);
                while (up < 20 && contrast(dark, lighter) < 4.5)
                    lighter = mix(base, QColor(Qt::white), ++up * .05);
                const bool goDark = contrast(white, darker) >= 4.5 && (down <= up || contrast(dark, lighter) < 4.5);
                ink = goDark ? white : dark;
                fill = goDark ? darker : lighter;
            }
            return std::pair{fill, ink};
        };
        // The theme's red, else its bright red, else a fixed red (hue 25, C .17, at the theme red's lightness held to .50-.65, then the
        // nearby lightnesses and red hues): the first whose fill, once its ink is chosen, still reads as red apart from the accent.
        QList<QColor> bases{colors.value("red"), colors.value("bright_red")};
        const double L0 = std::clamp(oklch(colors.value("red")).L, .50, .65);
        for (const double L : {L0, .50, .55, .60, .65})
            for (const double h : {25., 15., 35., 5., 45., 355.})
                bases << fromOklch(L, .17, h);
        std::pair<QColor, QColor> chosen = filled(bases.at(2));
        double farthest = -1;
        for (const QColor &base : bases) {
            if (!base.isValid())
                continue;
            const auto made = filled(base);
            if (readsRed(made.first)) {
                chosen = made;
                break;
            }
            if (const double apart = deltaE(made.first, accent); oklch(made.first).C >= minChroma && apart > farthest) {
                farthest = apart;
                chosen = made;
            }
        }
        t.dangerSolid = chosen.first;
        t.dangerInk = chosen.second;
        t.dangerSolidHover = mix(t.dangerSolid, t.dangerInk == white ? QColor(Qt::black) : QColor(Qt::white), .12);
    }
    // The sheen: the least alpha of light (the text on dark themes, white on light ones) that
    // makes the step; on a fill with no room to lighten, of black at the bottom instead.
    const auto sheen = [&](const QColor &over, double step) {
        QColor layer = over;
        for (int i = 1; i <= 120; ++i) // alpha .005 to .6, in steps of .005
            if (const double a = i * .005; deltaE(mix(t.body, over, a), t.body) >= step) {
                layer.setAlphaF(float(a));
                return layer;
            }
        layer.setAlphaF(0);
        return layer;
    };
    const auto ends = [&](double step, QColor &top, QColor &bottom) {
        top = sheen(light ? QColor(Qt::white) : fg, step);
        bottom = QColor(0, 0, 0, 0);
        if (top.alpha() == 0)
            bottom = sheen(QColor(Qt::black), step);
    };
    ends(kSheenStep, t.sheenTop, t.sheenBottom);
    ends(2 * kSheenStep, t.litTop, t.litBottom);
    // Labels sit on all of these: the share of text grows until the label reads at 4.5:1 on each.
    const QList<QColor> surfaces{t.body,      t.header,  t.well,   t.popup, t.selectedRow,
                                 t.menuRow,   t.canvas,  over(t.body, t.sheenTop), over(t.body, t.sheenBottom),
                                 over(t.body, t.litTop), over(t.body, t.litBottom)};
    for (int share = light ? 87 : 80; share <= 100; share += 2) {
        t.labelShare = share / 100.0;
        t.label = mix(t.body, fg, t.labelShare);
        if (std::all_of(surfaces.begin(), surfaces.end(), [&](const QColor &s) { return contrast(t.label, s) >= 4.5; }))
            break;
    }
    // A bypassed node: ComfyUI's magenta, from the palette so it follows the theme. Each tint
    // steps down until the text (and the label, where the plain body allows it) reads on it.
    const QColor magenta = colors.value("magenta", accent);
    const double labelNeed = std::min(4.5, contrast(t.label, t.body));
    for (const double k : {.26, .22, .18, .14, .10, .06}) {
        t.bypassBody = mix(t.body, magenta, k);
        if (readsUnderSheen(t, t.bypassBody, fg, labelNeed)) // each end of the sheen over it too (#72)
            break;
    }
    for (const double k : {.40, .34, .28, .22, .16, .10}) {
        t.bypassHeader = mix(t.header, magenta, k);
        if (contrast(fg, t.bypassHeader) >= 4.5)
            break;
    }
    t.bypassEdge = lift(magenta, t.canvas, 3.0);
    // The Drift kit: a group a step off the popup it sits on, its separator, rail, knob and chips from the group.
    t.kitGroup = mix(t.popup, fg, light ? .045 : .07);
    t.kitSeparator = mix(t.kitGroup, fg, .10);
    t.kitTrack = mix(t.kitGroup, fg, light ? .30 : .28);
    t.kitKnob = QColor(Qt::white); // Drift: a white knob and thumb on every theme, on and off
    t.kitChip = mix(t.kitGroup, fg, .10);
    t.kitChipHover = mix(t.kitGroup, fg, .17);
    t.kitHeroShade = QColor(0, 0, 0, 140);
    t.kitHeroPill = QColor(0, 0, 0, 112);
    t.kitHeroPillHover = QColor(0, 0, 0, 153);
    t.kitWell = mix(t.kitGroup, QColor(Qt::black), light ? .03 : .12); // a field set in a group: a shade darker, not a step
    t.kitFrost = QColor(255, 255, 255, 51);      // frosted glass over a picture: white tint at 20%, hover 30%, edge 12%
    t.kitFrostHover = QColor(255, 255, 255, 77);
    t.kitFrostEdge = QColor(255, 255, 255, 31); // 12%: at 28% the edge glowed like the accent surrounds the owner asked away (2026-11-25)
    return t;
}

RoleTokens roleTokens(const Tokens &t, const QColor &role, const QColor &text, bool light) {
    RoleTokens r;
    r.frame = mix(t.canvas, role, light ? .12 : .10);
    r.frameEdge = mix(t.canvas, role, .35);
    for (const double k : {.5, .6, .7, .8, .9, 1.0}) {
        r.frameTitle = mix(role, text, k);
        if (contrast(r.frameTitle, r.frame) >= 4.5)
            break;
    }
    // A tint steps down until the title on it still reads at 4.5:1.
    const std::vector<double> tints = light ? std::vector<double>{.14, .10, .06} : std::vector<double>{.18, .14, .10, .06};
    for (const double k : tints) {
        r.tint = mix(t.header, role, k);
        if (contrast(text, r.tint) >= 4.5)
            break;
    }
    // The body takes the color too (#72), as a bypassed node takes magenta: each tint steps down
    // until the text, and the label as well as it reads on the plain body, read on it.
    const double labelNeed = std::min(4.5, contrast(t.label, t.body));
    const std::vector<double> bodies =
        light ? std::vector<double>{.18, .14, .10, .06} : std::vector<double>{.22, .18, .14, .10, .06};
    for (const double k : bodies) {
        r.tintBody = mix(t.body, role, k);
        if (readsUnderSheen(t, r.tintBody, text, labelNeed)) // at every end of the sheen over it
            break;
    }
    return r;
}

QHash<QString, QColor> portColors(const QHash<QString, QColor> &colors, bool light, const Tokens &t, bool *mono) {
    Q_UNUSED(light);
    const QColor fg = colors.value("foreground");
    QHash<QString, QColor> theirs;
    for (const auto &[type, role] : portRoles())
        theirs.insert(type, colors.value(role, fg));
    theirs.insert("declared", colors.value("accent"));

    std::vector<double> lightness, chroma;
    for (const QColor &c : std::as_const(theirs)) {
        const Oklch o = oklch(c);
        if (o.C >= minChroma) {
            lightness.push_back(o.L);
            chroma.push_back(o.C);
        }
    }
    const bool monochrome = lightness.empty();
    if (mono)
        *mono = monochrome;

    QHash<QString, QColor> out;
    if (monochrome) {
        out = theirs;
    } else {
        std::sort(lightness.begin(), lightness.end());
        std::sort(chroma.begin(), chroma.end());
        const double medianL = lightness[lightness.size() / 2];
        const double medianC = std::max(.12, chroma[chroma.size() / 2]);
        auto nearest = [&](const QColor &c) {
            double best = 1.0;
            for (const QColor &kept : std::as_const(out))
                best = std::min(best, deltaE(c, kept));
            return best;
        };
        for (const QString &type : keepOrder) {
            QColor color = theirs.value(type);
            if (oklch(color).C < minChroma || nearest(color) < minDeltaE) {
                // The type's own hue first, then its neighbours 30 degrees at a time; failing all,
                // the most distinct of them, or for a declared type the label color (its diamond
                // already sets it apart).
                QColor best;
                double bestDistance = -1;
                for (const int d : {0, 30, -30, 60, -60, 90, -90, 120, -120, 150, -150, 180}) {
                    const double hue = std::fmod(fallbackHue.value(type) + d + 360, 360);
                    const QColor candidate = fromOklch(medianL, medianC, hue);
                    const double distance = nearest(candidate);
                    if (distance > bestDistance) {
                        best = candidate;
                        bestDistance = distance;
                    }
                    if (distance >= minDeltaE)
                        break;
                }
                color = bestDistance >= minDeltaE || type != "declared" ? best : t.label;
            }
            out.insert(type, color);
        }
    }
    for (auto it = out.begin(); it != out.end(); ++it)
        *it = lift(lift(*it, t.body), t.canvas);
    return out;
}

Corners corners(double slider, bool followOmarchy, int omarchy) {
    const double factor = 2.0 * std::clamp(slider, 0.0, 1.0); // 0 is square (an old corner_radius 0 stays square)
    const int node = followOmarchy ? std::min(omarchy, 8) : 8;
    const int popup = followOmarchy ? omarchy : 8;
    return {static_cast<int>(std::lround(node * factor)), static_cast<int>(std::lround(popup * factor))};
}

double omarchyCornerScale(bool followOmarchy, int omarchy) { return followOmarchy ? omarchy / 8.0 : 1.0; }

namespace {

struct ShadowLayer {
    double y, blur, alpha;
};

std::vector<ShadowLayer> shadowLayers(int level, bool light) {
    // A tight contact shadow under the edge, then a large soft ambient one, both cast down.
    // Dark themes need far more: black over a near-black canvas only shows when it is deep.
    using Layers = std::vector<ShadowLayer>;
    switch (level) {
    case 1: return light ? Layers{{1, 3, .12}, {6, 18, .12}} : Layers{{2, 4, .75}, {8, 24, .65}};
    case 2: return light ? Layers{{2, 5, .14}, {12, 32, .18}} : Layers{{3, 8, .80}, {16, 40, .75}};
    case 4: return light ? Layers{{3, 8, .14}, {20, 48, .24}} : Layers{{4, 10, .80}, {24, 60, .80}};
    case 3:
    default: return light ? Layers{{4, 10, .08}, {18, 48, .20}} : Layers{{6, 16, .50}, {20, 56, .55}};
    }
}

// One box blur pass of `radius` along rows (`step` 1) or columns (`step` width), with a
// running sum, so its cost does not grow with the radius; outside the buffer is clear.
void boxPass(const std::vector<float> &from, std::vector<float> &to, int lines, int length, int lineStep, int step,
             int radius) {
    const float scale = 1.0f / float(2 * radius + 1);
    for (int line = 0; line < lines; ++line) {
        const size_t base = size_t(line) * size_t(lineStep);
        const auto at = [&](int i) { return i >= 0 && i < length ? from[base + size_t(i) * size_t(step)] : 0.0f; };
        float sum = 0;
        for (int i = -radius; i <= radius; ++i)
            sum += at(i);
        for (int i = 0; i < length; ++i) {
            to[base + size_t(i) * size_t(step)] = sum * scale;
            sum += at(i + radius + 1) - at(i - radius);
        }
    }
}

// A Gaussian blur of a coverage buffer, as three box blurs whose widths add up to the same
// spread (Kovesi's approximation); CSS's blur radius B is sigma B / 2.
void blur(std::vector<float> &buffer, int width, int height, double sigma) {
    if (sigma < .3)
        return;
    constexpr int passes = 3;
    int lower = int(std::floor(std::sqrt(12 * sigma * sigma / passes + 1)));
    if (lower % 2 == 0)
        --lower;
    const int wider = int(std::round((12 * sigma * sigma - passes * lower * lower - 4.0 * passes * lower - 3 * passes)
                                     / (-4.0 * lower - 4)));
    std::vector<float> pass(buffer.size());
    for (int i = 0; i < passes; ++i) {
        const int radius = ((i < wider ? lower : lower + 2) - 1) / 2;
        boxPass(buffer, pass, height, width, width, 1, radius);
        boxPass(pass, buffer, width, height, 1, width, radius);
    }
}

} // namespace

QImage shadowImage(int level, int corner, bool light, int *margin) {
    const std::vector<ShadowLayer> layers = shadowLayers(level, light);
    double reach = 0;
    for (const ShadowLayer &layer : layers)
        reach = std::max(reach, std::abs(layer.y) + layer.blur * 1.5);
    const int e = int(std::ceil(reach));
    const int border = e + corner + 1; // the 9-slice border: the margin, the corner and 1 px
    const int out = 2 * border + 1;    // and a 1 px middle that stretches
    if (margin)
        *margin = e;
    // Painted around a box big enough that its middle is as dark as a real node's (a blur wider
    // than the box would leave it pale), then cut down to the borders and one middle line.
    const int box = 2 * corner + 3 + 2 * e;
    const int size = 2 * e + box;

    // Total opacity: each layer is black laid over the others.
    std::vector<float> clear(size_t(size * size), 1.0f);
    for (const ShadowLayer &layer : layers) {
        QImage shape(size, size, QImage::Format_Alpha8);
        shape.fill(0);
        {
            QPainter painter(&shape);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(e, e + layer.y, box, box), corner, corner);
            painter.fillPath(path, Qt::black);
        }
        std::vector<float> coverage(size_t(size * size));
        for (int y = 0; y < size; ++y) {
            const uchar *line = shape.constScanLine(y);
            for (int x = 0; x < size; ++x)
                coverage[size_t(y * size + x)] = line[x] / 255.0f;
        }
        blur(coverage, size, size, layer.blur / 2);
        for (size_t i = 0; i < clear.size(); ++i)
            clear[i] *= 1.0f - float(layer.alpha) * std::clamp(coverage[i], 0.0f, 1.0f);
    }
    const auto from = [&](int i) { return i < border ? i : i == border ? size / 2 : size - out + i; };
    QImage image(out, out, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < out; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < out; ++x)
            line[x] = qRgba(0, 0, 0, qRound((1.0f - clear[size_t(from(y) * size + from(x))]) * 255));
    }
    return image;
}

} // namespace fugue::look

namespace {

QString dataUrl(const QImage &image) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

QColor blackOrWhiteOn(const QColor &c) { return fugue::look::luminance(c) > .179 ? QColor(Qt::black) : QColor(Qt::white); }

} // namespace

namespace {

// The icon font, loaded once per process; its glyph names from the map beside it.
struct Icons {
    QString family;
    QString serif; // Fraunces, the Drift kit's title face
    QHash<QString, QString> glyphs;
};

const Icons &icons() {
    static const Icons loaded = [] {
        Icons icons;
        if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
            const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/lucide.ttf"));
            if (id >= 0)
                icons.family = QFontDatabase::applicationFontFamilies(id).value(0);
        }
        if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
            const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/fraunces.ttf"));
            if (id >= 0)
                icons.serif = QFontDatabase::applicationFontFamilies(id).value(0);
        }
        QFile map(QStringLiteral(":/fonts/lucide.txt"));
        if (map.open(QIODevice::ReadOnly)) {
            for (const QByteArray &line : map.readAll().split('\n')) {
                const QList<QByteArray> parts = line.trimmed().split(' ');
                if (parts.size() == 2 && !line.startsWith('#'))
                    icons.glyphs.insert(QString::fromLatin1(parts[0]), QChar(char16_t(parts[1].toUInt(nullptr, 16))));
            }
        }
        return icons;
    }();
    return loaded;
}

} // namespace

CanvasLook::CanvasLook(QObject *parent) : QObject(parent) {
    icons();
    m_params = fugue::look::resolveParams(m_sliders, m_overrides);
}

void CanvasLook::setPreset(const QVariantMap &params) {
    if (params == m_preset)
        return;
    m_preset = params;
    resolve();
}

void CanvasLook::resolve() {
    QVariantMap layered = m_preset; // the preset, then look.json over it
    for (auto it = m_overrides.cbegin(); it != m_overrides.cend(); ++it)
        layered.insert(it.key(), it.value());
    m_params = fugue::look::resolveParams(m_sliders, layered);
    emit changed();
}

void CanvasLook::setSliders(const fugue::look::Sliders &sliders) {
    fugue::look::Sliders clamped;
    clamped.colour = qBound(0.0, sliders.colour, 1.0);
    clamped.glow = qBound(0.0, sliders.glow, 1.0);
    clamped.corners = qBound(0.0, sliders.corners, 1.0);
    clamped.spacing = qBound(0.0, sliders.spacing, 1.0);
    clamped.depth = qBound(0.0, sliders.depth, 1.0);
    clamped.cornerScale = qBound(0.0, sliders.cornerScale, 4.0);
    if (clamped.cornerScale == m_sliders.cornerScale && clamped.colour == m_sliders.colour && clamped.glow == m_sliders.glow && clamped.corners == m_sliders.corners
        && clamped.spacing == m_sliders.spacing && clamped.depth == m_sliders.depth)
        return;
    m_sliders = clamped;
    resolve();
}

void CanvasLook::setLookFile(const QString &path) {
    if (path == m_lookFile)
        return;
    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        // An editor that saves by replacing the file drops the watch on it: the folder's change brings it back.
        connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { readLookFile(); });
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { readLookFile(); });
    }
    if (!m_watcher->files().isEmpty())
        m_watcher->removePaths(m_watcher->files());
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
    m_lookFile = path;
    readLookFile();
}

void CanvasLook::readLookFile() {
    QVariantMap overrides;
    QStringList problems;
    if (!m_lookFile.isEmpty()) {
        const QFileInfo info(m_lookFile);
        if (!m_watcher->directories().contains(info.absolutePath()) && info.dir().exists())
            m_watcher->addPath(info.absolutePath());
        QFile file(m_lookFile);
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            if (!m_watcher->files().contains(m_lookFile))
                m_watcher->addPath(m_lookFile);
            overrides = fugue::look::parseLookFile(file.readAll(), &problems);
        }
    }
    // A problem is said once: a reload with the same text says nothing new.
    for (const QString &problem : std::as_const(problems))
        if (!m_lookProblems.contains(problem))
            qWarning().noquote() << problem;
    if (overrides == m_overrides && problems == m_lookProblems)
        return;
    m_overrides = overrides;
    m_lookProblems = problems;
    resolve();
}

QString CanvasLook::iconFont() const { return icons().family; }
QString CanvasLook::serifFont() const { return icons().serif; }

QString CanvasLook::glyph(const QString &name) const { return icons().glyphs.value(name); }

void CanvasLook::update(const QHash<QString, QColor> &colors, bool light, fugue::look::Corners corners, int textSize) {
    using namespace fugue::look;
    m_colors = colors;
    m_roles.clear();
    m_tokens = fugue::look::tokens(colors, light);
    const Tokens &t = m_tokens;
    m_canvas = t.canvas;
    m_dot = t.dot;
    m_dotMajor = t.dotMajor;
    m_body = t.body;
    m_header = t.header;
    m_hairline = t.hairline;
    m_hairlineStrong = t.hairlineStrong;
    m_well = t.well;
    m_label = t.label;
    m_popup = t.popup;
    m_errorHeader = t.errorHeader;
    m_errorChip = t.errorChip;
    m_selectedRow = t.selectedRow;
    m_menuRow = t.menuRow;
    m_dangerFill = t.dangerFill;
    m_dangerHover = t.dangerHover;
    m_dangerSolid = t.dangerSolid;
    m_dangerSolidHover = t.dangerSolidHover;
    m_dangerInk = t.dangerInk;
    m_bevel = t.bevel;
    m_sheenTop = t.sheenTop;
    m_sheenBottom = t.sheenBottom;
    m_litTop = t.litTop;
    m_litBottom = t.litBottom;
    m_scrim = t.scrim;
    m_kitGroup = t.kitGroup;
    m_kitSeparator = t.kitSeparator;
    m_kitTrack = t.kitTrack;
    m_kitKnob = t.kitKnob;
    m_kitChip = t.kitChip;
    m_kitChipHover = t.kitChipHover;
    m_kitHeroShade = t.kitHeroShade;
    m_kitHeroPill = t.kitHeroPill;
    m_kitHeroPillHover = t.kitHeroPillHover;
    m_kitFrost = t.kitFrost;
    m_kitWell = t.kitWell;
    m_kitFrostHover = t.kitFrostHover;
    m_kitFrostEdge = t.kitFrostEdge;
    m_bypassBody = t.bypassBody;
    m_bypassHeader = t.bypassHeader;
    m_bypassEdge = t.bypassEdge;
    m_text = colors.value("foreground");
    m_accent = colors.value("accent");
    m_accentForeground = blackOrWhiteOn(m_accent);
    m_error = colors.value("red");
    m_light = light;

    m_portColors = portColors(colors, light, t);
    m_tabColors = tabColors(colors, t);
    m_ports.clear();
    for (auto it = m_portColors.cbegin(); it != m_portColors.cend(); ++it)
        m_ports.insert(it.key(), it.value());
    m_ports.insert("any", t.label);
    m_ports.insert("unknown", t.label);

    m_textSize = textSize;
    m_unit = textSize / 3.0;
    m_corner = corners.node;
    m_popupCorner = corners.popup;
    m_smallCorner = std::min(m_corner, 4);
    m_thumbCorner = std::max(0, m_corner - 6);
    m_labelSize = qRound(textSize * 11 / 12.0);
    m_captionSize = std::max(9, qRound(textSize * 10 / 12.0));
    m_frameTitleSize = qRound(textSize * 4 / 3.0);
    m_searchFieldSize = qRound(textSize * 5 / 4.0);
    m_resultSize = qRound(textSize * 13 / 12.0);
    m_overlayTitleSize = qRound(textSize * 3 / 2.0);

    int margins[4] = {};
    m_shadow1 = dataUrl(shadowImage(1, m_corner, light, &margins[0]));
    m_shadow2 = dataUrl(shadowImage(2, m_corner, light, &margins[1]));
    m_shadow3 = dataUrl(shadowImage(3, m_popupCorner, light, &margins[2]));
    m_shadow4 = dataUrl(shadowImage(4, m_corner, light, &margins[3]));
    m_shadowMargins = {margins[0], margins[1], margins[2], margins[3]};
    emit changed();
}

QVariantMap CanvasLook::role(const QString &name) const {
    if (const auto it = m_roles.constFind(name); it != m_roles.constEnd())
        return *it;
    QColor color;
    // A hex layout color (#52): a hue, derived below as any role is, so it follows the theme.
    if (const QString hex = fugue::normalizeHex(name); name.startsWith(QLatin1Char('#')) && !hex.isEmpty())
        color = QColor(hex);
    else if (name == QLatin1String("accent"))
        color = m_accent;
    else if (name == QLatin1String("text"))
        color = m_text;
    else if (name == QLatin1String("error"))
        color = m_error;
    else if (name != QLatin1String("muted") && m_colors.contains(name))
        color = m_colors.value(name);
    else
        color = m_tokens.label; // muted, and anything unknown
    const fugue::look::RoleTokens r = fugue::look::roleTokens(m_tokens, color, m_text, m_light);
    const QVariantMap map{{"frame", r.frame},
                          {"frameEdge", r.frameEdge},
                          {"frameTitle", r.frameTitle},
                          {"tint", r.tint},
                          {"tintBody", r.tintBody}};
    m_roles.insert(name, map);
    return map;
}

QColor CanvasLook::port(const QString &key) const {
    if (key == QLatin1String("any") || key == QLatin1String("unknown"))
        return m_tokens.label;
    return portColor(key);
}
