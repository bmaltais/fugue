#include "look_params.h"

#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QtMath>
#include <algorithm>

namespace fugue::look {

const QList<ParamDef> &paramDefs() {
    static const QList<ParamDef> defs{
        {"panelTint", 0.124, 0.0, 0.5, "colour", false, "alpha of the strongest blob of a panel's tint (the board: 0.06 + 0.16 x 0.4)", {}},
        {"tintBlob1", 0.8, 0.0, 1.0, "", false, "the second blob's alpha as a fraction of the first", {}},
        {"tintBlob2", 0.9, 0.0, 1.0, "", false, "the third blob's alpha as a fraction of the first", {}},
        {"tintCalm", 0.55, 0.0, 1.0, "", false, "the stage and the rail's tint as a fraction of the panel's", {}},
        {"groupFillAlpha", 0.035, 0.0, 0.3, "", false, "a group's white fill over a tinted panel, dark theme", {}},
        {"groupFillAlphaLight", 0.04, 0.0, 0.3, "", false, "a group's black fill over a tinted panel, light theme", {}},
        {"toggleOnTop", 0.52, 0.0, 1.0, "colour", false, "an on toggle's tone alpha at the top of the track", {}},
        {"toggleOnBottom", 0.34, 0.0, 1.0, "colour", false, "the same at the bottom", {}},
        {"segThumbTop", 0.55, 0.0, 1.0, "colour", false, "a chooser thumb's tone alpha at the top", {}},
        {"segThumbBottom", 0.34, 0.0, 1.0, "colour", false, "the same at the bottom", {}},
        {"sliderFill", 0.85, 0.0, 1.0, "colour", false, "a slider fill's tone alpha", {}},
        {"haloStop", 0.22, 0.0, 1.0, "glow", false, "the halo's alpha at its centre before its opacity", {}},
        {"haloOpacity", 0.18, 0.0, 1.0, "glow", false, "the halo's opacity when on (centre about 4%)", {}},
        {"haloWidth", 65.0, 10.0, 160.0, "glow", true, "the halo's width in u", {}},
        {"haloHeight", 30.0, 6.0, 80.0, "glow", true, "the halo's height in u", {}},
        {"knobShine", 0.75, 0.0, 1.0, "glow", false, "the toggle knob's light edge along the top", {}},
        {"thumbShine", 0.35, 0.0, 1.0, "glow", false, "the chooser thumb's light edge along the top", {}},
        {"wellDarken", 0.12, 0.02, 0.3, "depth", false, "how much darker a field is than its group, dark theme (black)", {}},
        {"wellDarkenLight", 0.03, 0.01, 0.2, "depth", false, "the same on a light theme", {}},
        {"wellShadowAlpha", 0.22, 0.0, 0.4, "depth", false, "the inner shadow along a field's top edge, dark theme", {}},
        {"wellShadowAlphaLight", 0.08, 0.0, 0.4, "depth", false, "the same on a light theme", {}},
        {"wellShadowHeight", 1.0, 0.0, 3.0, "depth", false, "the inner shadow's height in px", {}},
        {"wellBottomLight", 0.045, 0.0, 0.5, "depth", false, "the light line along a field's bottom edge, dark theme", {}},
        {"wellBottomLightLight", 0.6, 0.0, 1.0, "depth", false, "the same on a light theme", {}},
        {"raiseEdge", 0.07, 0.0, 0.5, "depth", false, "the light top edge of a raised control, dark theme", {}},
        {"raiseEdgeLight", 0.6, 0.0, 1.0, "depth", false, "the same on a light theme", {}},
        {"raiseShadowAlpha", 0.28, 0.0, 0.5, "depth", false, "the drop shadow under a raised control, dark theme", {}},
        {"raiseShadowAlphaLight", 0.1, 0.0, 0.5, "depth", false, "the same on a light theme", {}},
        {"raiseShadowOffset", 1.0, 0.0, 3.0, "depth", false, "the drop shadow's offset in px", {}},
        {"glassTrackAlpha", 0.3, 0.0, 0.6, "depth", false, "the recessed glass of a toggle or a chooser off, dark theme (black)", {}},
        {"glassTrackAlphaLight", 0.12, 0.0, 0.5, "depth", false, "the same on a light theme", {}},
        {"glassTrackShadow", 0.4, 0.0, 0.6, "depth", false, "the inner shadow of that track, dark theme", {}},
        {"cornerCard", 4.5, 0.4, 9.0, "corners", false, "a card: the panel, the stage, the rail, in u", {}},
        {"cornerGroup", 3.0, 0.4, 7.0, "corners", false, "a group of rows, in u", {}},
        {"cornerWell", 2.0, 0.4, 5.0, "corners", false, "a field, in u", {}},
        {"cornerChip", 2.5, 0.4, 6.0, "corners", false, "a chip and a rail item, in u", {}},
        {"cornerSegment", 3.5, 0.4, 7.0, "corners", false, "a chooser's track and thumb, in u (a pill at its default)", {}},
        {"cornerHero", 4.5, 0.4, 9.0, "corners", false, "the header's top corners, in u", {}},
        {"areaGap", 2.0, 0.0, 6.0, "spacing", false, "between the rail, the stage and the panel, in u", {}},
        {"panelPad", 3.0, 0.0, 8.0, "spacing", false, "the panel's padding round its form and Run, in u", {}},
        {"rowPadLeft", 3.5, 0.0, 8.0, "spacing", false, "a row's padding at its start, in u", {}},
        {"rowPadRight", 3.0, 0.0, 8.0, "spacing", false, "a row's padding at its end, in u", {}},
        {"rowPadY", 3.0, 0.0, 6.0, "spacing", false, "a row's padding above and below, in u (the row keeps its minimum height)", {}},
        {"rowGap", 2.0, 0.0, 6.0, "spacing", false, "between a row's controls, in u", {}},
        {"rowMinHeight", 10.0, 7.0, 16.0, "", false, "a row's least height in u (a press target: 7u is the floor)", {}},
        {"heroFadeStart", 0.58, 0.2, 0.95, "", false, "where the header's art starts to fade, as a fraction of its height", {}},
        {"heroOverlap", 5.0, 0.0, 10.0, "", false, "how far the header's art reaches below its content, in u", {}},
        {"heroShadeTop", 0.05, 0.0, 0.6, "", false, "the title shade's alpha at the top of the header", {}},
        {"heroShadeMid", 0.3, 0.0, 0.8, "", false, "its alpha at heroShadeAt", {}},
        {"heroShadeMidLight", 0.42, 0.0, 0.8, "", false, "the same on a light theme, where the header's blend is paler (white ink keeps 4.5:1)", {}},
        {"heroShadeAt", 0.6, 0.1, 0.95, "", false, "where the shade is strongest, as a fraction of the art's height", {}},
        {"knobTop", 0.62, 0.0, 1.0, "", false, "the toggle knob's white alpha at the top", {}},
        {"knobBottom", 0.34, 0.0, 1.0, "", false, "the same at the bottom", {}},
        {"knobEdge", 0.55, 0.0, 1.0, "", false, "the knob's white edge", {}},
        {"trackEdge", 0.25, 0.0, 1.0, "", false, "the glass track's dark edge", {}},
        {"durColor", 100.0, 0.0, 1000.0, "", false, "a colour change in ms", {}},
        {"durToggle", 160.0, 0.0, 1000.0, "", false, "a toggle's knob and track in ms", {}},
        {"durCopied", 1500.0, 0.0, 10000.0, "", false, "how long a copy button shows Copied, in ms", {}},
        {"durThumb", 180.0, 0.0, 1000.0, "", false, "a chooser's thumb in ms", {}},
        {"thumbAlpha", 0.35, 0.0, 1.0, "", false, "a scroll bar's thumb alpha at rest", {}},
        {"thumbAlphaLight", 0.45, 0.0, 1.0, "", false, "the same on a light theme", {}},
        {"thumbHover", 0.7, 0.0, 1.0, "", false, "the thumb's alpha while hovered", {}},
        {"thumbPress", 0.9, 0.0, 1.0, "", false, "the thumb's alpha while pressed", {}},
        {"titleFont", 0, 0, 0, "", false, "the header's title: the serif (Fraunces) or the interface font", {"serif","ui"}},
        {"heroStyle", 0, 0, 0, "", false, "a header: its picture (else the tone), the tone's blend always, or a plain fill", {"picture","tone","plain"}},
        {"chipShape", 0, 0, 0, "", false, "a chip: fully round or rounded like a field", {"pill","rounded"}},
        {"borders", 0, 0, 0, "", false, "the hairline round cards and groups", {"hairline","none"}},
        {"rowSeparators", 0, 0, 0, "", false, "the hairline between a group's rows", {"hairline","none"}},
        {"areaLayout", 0, 0, 0, "", false, "rounded cards with gaps between them, or panes edge to edge with 1px dividers", {"cards","panes"}},
        {"controlColour", 0, 0, 0, "", false, "a switch, a chooser and a slider in the window's tone or in the accent", {"tone","accent"}},
        {"motion", 0, 0, 0, "", false, "animations at their durations, or none", {"normal","reduced"}},
        {"durHalo", 260.0, 0.0, 1000.0, "", false, "the halo's fade in ms", {}},
    };
    return defs;
}

const ParamDef *paramDef(const QString &name) {
    for (const ParamDef &def : paramDefs())
        if (def.name == name)
            return &def;
    return nullptr;
}

double sliderFactor(const QString &slider, const Sliders &s) {
    if (slider == QLatin1String("colour"))
        return s.colour / Sliders::defaultColour;
    if (slider == QLatin1String("glow"))
        return s.glow / Sliders::defaultGlow;
    if (slider == QLatin1String("depth"))
        return s.depth / Sliders::defaultDepth;
    if (slider == QLatin1String("corners")) // 0.1 to 1.9, 1 at the default
        return s.cornerScale * 2.0 * s.corners; // 0 square, 1 at the default 0.5, 2 at 1
    if (slider == QLatin1String("spacing")) // 0.25 to 1.75
        return 0.25 + 1.5 * s.spacing;
    return 1;
}

QVariantMap resolveParams(const Sliders &sliders, const QVariantMap &overrides) {
    QVariantMap out;
    for (const ParamDef &def : paramDefs()) {
        if (!def.choices.isEmpty()) {
            const QString given = overrides.value(def.name).toString();
            out.insert(def.name, def.choices.contains(given) ? given : def.choices.first());
            continue;
        }
        double value = overrides.contains(def.name) ? overrides.value(def.name).toDouble() : def.def;
        if (!def.slider.isEmpty()) {
            const double factor = sliderFactor(def.slider, sliders);
            value *= def.reach ? 0.5 + 0.5 * factor : factor;
        }
        out.insert(def.name, std::clamp(value, def.min, def.max));
    }
    if (out.value(QStringLiteral("motion")).toString() == QLatin1String("reduced")) // no animation at all
        for (auto it = out.begin(); it != out.end(); ++it)
            if (it.key().startsWith(QLatin1String("dur")))
                it.value() = 0.0;
    return out;
}

QVariantMap parseLookFile(const QByteArray &json, QStringList *problems) {
    QVariantMap out;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (!document.isObject()) {
        if (problems)
            *problems << QStringLiteral("look.json is not a JSON object%1.").arg(error.error != QJsonParseError::NoError ? QStringLiteral(" (%1)").arg(error.errorString()) : QString());
        return out;
    }
    const QJsonObject object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const ParamDef *def = paramDef(it.key());
        if (!def) {
            if (problems)
                *problems << QStringLiteral("look.json: %1 is not a look parameter.").arg(it.key());
            continue;
        }
        if (!def->choices.isEmpty()) {
            if (!it.value().isString() || !def->choices.contains(it.value().toString())) {
                if (problems)
                    *problems << QStringLiteral("look.json: %1 must be one of %2.").arg(it.key(), def->choices.join(QStringLiteral(", ")));
                continue;
            }
            out.insert(it.key(), it.value().toString());
            continue;
        }
        if (!it.value().isDouble()) {
            if (problems)
                *problems << QStringLiteral("look.json: %1 must be a number.").arg(it.key());
            continue;
        }
        const double value = it.value().toDouble();
        if (value < def->min || value > def->max) {
            if (problems)
                *problems << QStringLiteral("look.json: %1 is %2, outside %3 to %4; clamped.").arg(it.key()).arg(value).arg(def->min).arg(def->max);
        }
        out.insert(it.key(), std::clamp(value, def->min, def->max));
    }
    return out;
}

QByteArray defaultLookFile() {
    // JSON has no comments: the file lists every parameter at its default (delete the lines you do not change); the range and the
    // slider are in docs/canvas-look.md 20.3.
    QByteArray text = "{\n";
    const QList<ParamDef> &defs = paramDefs();
    for (int i = 0; i < defs.size(); ++i)
        text += QByteArray("  \"") + defs[i].name.toUtf8() + "\": "
                + (defs[i].choices.isEmpty() ? QByteArray::number(defs[i].def, 'g', 6) : "\"" + defs[i].choices.first().toUtf8() + "\"")
                + (i + 1 < defs.size() ? ",\n" : "\n");
    return text + "}\n";
}

QStringList presetIds() { return {QStringLiteral("calm"), QStringLiteral("flat")}; }

Preset parsePreset(const QString &id, const QByteArray &json, QStringList *problems) {
    Preset preset;
    preset.id = id;
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    preset.title = object.value(QLatin1String("title")).toString(id);
    preset.params = parseLookFile(QJsonDocument(object.value(QLatin1String("params")).toObject()).toJson(), problems);
    const QJsonObject sliders = object.value(QLatin1String("sliders")).toObject();
    static const QStringList names{"colour", "glow", "frost", "depth", "corners", "spacing"};
    for (auto it = sliders.begin(); it != sliders.end(); ++it) {
        if (!names.contains(it.key()) || !it.value().isDouble() || it.value().toDouble() < 0 || it.value().toDouble() > 1) {
            if (problems)
                *problems << QStringLiteral("look preset %1: slider %2 must be a number from 0 to 1 (colour, glow, frost, depth, corners, spacing).").arg(id, it.key());
            continue;
        }
        preset.sliders.insert(it.key(), it.value().toDouble());
    }
    // Settings the preset also sets (settings.json keys, a string each); checked where they are applied.
    const QJsonObject settings = object.value(QLatin1String("settings")).toObject();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (it.key() != QLatin1String("node_style") || !it.value().isString()) {
            if (problems)
                *problems << QStringLiteral("look preset %1: setting %2 is not one a preset sets (node_style).").arg(id, it.key());
            continue;
        }
        preset.settings.insert(it.key(), it.value().toString());
    }
    return preset;
}

Preset loadPreset(const QString &id, QStringList *problems) {
    const QString use = presetIds().contains(id) ? id : presetIds().first();
    QFile file(QStringLiteral(":/fugue/looks/%1.json").arg(use));
    if (!file.open(QIODevice::ReadOnly)) {
        if (problems)
            *problems << QStringLiteral("The look preset %1 is missing.").arg(use);
        return {use, use, {}, {}, {}};
    }
    return parsePreset(use, file.readAll(), problems);
}

} // namespace fugue::look
