// The look parameters of the Drift kit (docs/canvas-look.md 20.3, the parameter table): every number the kit and the App view
// draw with, named, with its default (the approved value), its range and the slider that scales it. The kit reads only these
// (`CanvasLook::params`); nothing in src/kit or src/apps keeps a look number of its own.
//
// Layers, from the bottom: the default; `look.json`'s override of it (a power user's file in the config folder, by name); then the
// five macro sliders (Colour, Glow, Corners, Spacing, Depth; Frost is its own setting and scales the glass in KitFrost), each a
// factor that multiplies a documented group of parameters around the value below it, and the result clamped to the range. The
// default slider position gives a factor of 1, so the default look is the approved one exactly.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace fugue::look {

struct ParamDef {
    QString name;
    double def = 0;
    double min = 0;
    double max = 1;
    QString slider; // "colour", "glow", "corners", "spacing", "depth", or empty: no slider moves it
    bool reach = false; // a glow parameter that grows more slowly than the strength (half as fast): the halo's size
    QString doc;
    QStringList choices; // not empty: an enumerated parameter (a structural choice), its value one of these; the first is its default
};

// The positions of the five sliders, 0 to 1. The defaults are the approved look.
struct Sliders {
    static constexpr double defaultColour = 0.4, defaultGlow = 0.5, defaultCorners = 0.5, defaultSpacing = 0.5, defaultDepth = 0.5;
    double colour = defaultColour, glow = defaultGlow, corners = defaultCorners, spacing = defaultSpacing, depth = defaultDepth;
};

const QList<ParamDef> &paramDefs();
const ParamDef *paramDef(const QString &name);
// The factor a slider's position makes: 1 at its default, 0 (or near) at 0, 2 (or so) at 1.
double sliderFactor(const QString &slider, const Sliders &sliders);
// Every parameter's value now, by name: default or override, times its slider's factor, clamped to its range.
QVariantMap resolveParams(const Sliders &sliders, const QVariantMap &overrides);
// A look preset (share/looks/<name>.json, compiled in under :/fugue/looks): a name to show, parameters over the defaults and the
// positions of the six sliders (the five here and Frost; a slider it does not name stays at its default). A look is pure data: no
// code compares a preset's name.
struct Preset {
    QString id;
    QString title;
    QVariantMap params;
    QVariantMap sliders; // colour, glow, frost, depth, corners, spacing, each a number
};
QStringList presetIds(); // the shipped presets, in the order they are listed
Preset loadPreset(const QString &id, QStringList *problems = nullptr); // an unknown id is Calm
Preset parsePreset(const QString &id, const QByteArray &json, QStringList *problems);
// look.json's text to the overrides it names. A name that is unknown, a value that is not a number and a number out of range are
// each one line in `problems` (the out-of-range one is clamped, the other two dropped); a file that is not a JSON object is one.
QVariantMap parseLookFile(const QByteArray &json, QStringList *problems);
// A look.json with every parameter listed at its default, one per line with its range and what moves it.
QByteArray defaultLookFile();

} // namespace fugue::look
