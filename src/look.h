#pragma once

// The canvas look (docs/canvas-look.md): every color, size and shadow the canvas, node search
// and the `?` overlay draw with, derived from three inputs (the theme's palette, the corner
// radius setting and the desktop text size), so it holds on every Omarchy theme, light or dark.
//
// The color rules are docs/canvas-look/derive.py's, which is the reference: tests compare the
// two (tests/canvas/test_look_tokens.cpp). Change both together.
//
// QML reads it as `theme.look` (a CanvasLook): for example `theme.look.body`,
// `theme.look.labelSize`, `theme.look.role("blue").tint`. Theme::muted() and the other plain
// Theme colors stay as they were for the rest of the window (owner's decision, 18.3).

#include <QColor>
#include <QFileSystemWatcher>
#include <QStringList>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "look_params.h"

namespace fugue::look {

// --- color math (sRGB, WCAG contrast, OKLab and OKLCH) ---

double luminance(const QColor &c);
double contrast(const QColor &a, const QColor &b);
// The straight sRGB blend Theme uses: `t` of the way from a to b.
QColor mix(const QColor &a, const QColor &b, double t);
struct Oklch {
    double L = 0, C = 0, h = 0;
};
Oklch oklch(const QColor &c);
// The color at L and h, its chroma reduced until it fits sRGB.
QColor fromOklch(double L, double C, double h);
QColor fromOklch(const Oklch &c);
// OKLab distance.
double deltaE(const QColor &a, const QColor &b);
// Moves only OKLCH lightness, away from `against`, until `color` reads at `target`:1.
QColor lift(const QColor &color, const QColor &against, double target = 3.0);

// --- tokens (6.1) ---

struct Tokens {
    QColor canvas, dot, dotMajor, body, header, hairline, hairlineStrong, well, label, popup;
    QColor errorHeader, errorChip, selectedRow, menuRow, dangerFill, dangerHover;
    // A dialog's destructive answer (Discard, Delete, Quit; KitDialogButtons): the theme's red itself as a solid fill, so it looks alike
    // in light and dark themes, with the ink that reads on it best (white, or the theme's dark colour) at 4.5:1; the fill moves only when
    // neither ink does; the hover a step stronger.
    QColor dangerSolid, dangerSolidHover, dangerInk;
    // A bypassed node (9): the body and the header tinted with the palette's magenta, and the
    // dashed edge in it, as ComfyUI draws a bypassed node in magenta. The tint steps down
    // until the text and the labels still read at 4.5:1 on it.
    QColor bypassBody, bypassHeader, bypassEdge;
    QColor bevel; // the header's top highlight (dark themes), with alpha
    // A sheen on a node's fill (#72), as if light fell on the card from above: `sheenTop` laid
    // over the fill at its top, `sheenBottom` at its bottom, both with alpha. The step between
    // them is small and the same to the eye on every theme (OKLab 0.012): the top lighter, or,
    // on a fill too bright for that, the bottom darker. A hovered or selected node is lit more
    // (`litTop`, `litBottom`: twice the step). Labels read at every end.
    QColor sheenTop, sheenBottom, litTop, litBottom;
    QColor scrim; // behind the `?` overlay, with alpha
    // The Drift kit (docs/canvas-look.md, "Drift kit"): grouped rows on a popup-colored card. `kitGroup` is the
    // rounded group's fill, `kitSeparator` the hairline between its rows, `kitTrack` a slider's rail and a switch's
    // off track, `kitKnob` the round thumb, `kitChip` and `kitChipHover` a chip or pill button. The hero header's
    // three sit over a picture, so they are black at fixed alphas on every theme: `kitHeroShade` is the bottom of
    // its darkening gradient, `kitHeroPill` the status pill and close button, `kitHeroPillHover` the hovered one.
    QColor kitGroup, kitSeparator, kitTrack, kitKnob, kitChip, kitChipHover, kitHeroShade, kitHeroPill, kitHeroPillHover, kitFrost, kitFrostHover, kitFrostEdge, kitWell;
    double labelShare = 0; // the share of text in `label` the 4.5:1 rule settled on
};

// `colors` holds at least background, foreground, accent and red (Theme's fallbacks applied).
Tokens tokens(const QHash<QString, QColor> &colors, bool light);

// A layout color (spec 4.5) as a frame fill, its edge and title, and a node's header tint and
// body tint (the whole node takes the color, as in ComfyUI; #72).
struct RoleTokens {
    QColor frame, frameEdge, frameTitle, tint, tintBody;
};
RoleTokens roleTokens(const Tokens &tokens, const QColor &role, const QColor &text, bool light);

// The port type colors (6.2), keyed text, number, boolean, image, video, audio, declared:
// the theme's own, a colliding one moved to a distinct hue, and each lifted to 3:1 against
// the node and the canvas. A monochrome theme keeps its grays (`mono` set).
QHash<QString, QColor> portColors(const QHash<QString, QColor> &colors, bool light, const Tokens &tokens,
                                  bool *mono = nullptr);

// The six workflow-tab colors (12.10): the theme's red, yellow, green, cyan, blue and magenta, each
// lifted to 3:1 against the tab strip and the current tab. A theme without color gets six fixed hues
// at the accent's lightness, so tabs still differ.
QVariantList tabColors(const QHash<QString, QColor> &colors, const Tokens &tokens);

// --- corners (3) ---

// The node corner r and the popup corner R, in px. `slider` is the Look's Corners slider (look_corners, 0 to 1; 8 px for both at
// its default 0.5, the factor 2 x slider of every kit corner: 0 is square). With `followOmarchy` (look_corners_omarchy) the base is
// Hyprland's rounding `omarchy` instead of 8: R is the rounding and r is held to 8, both times the factor (#63, #467).
struct Corners {
    int node = 0, popup = 0;
};
Corners corners(double slider, bool followOmarchy, int omarchy);
// What the kit's corner parameters are multiplied by on top of the slider when the base is Omarchy's rounding (1 at 8 px).
double omarchyCornerScale(bool followOmarchy, int omarchy);

// --- shadows (7) ---

// One elevation level's shadow (1: node, 2: selected node and pills, 3: popups, 4: a node being
// dragged) for a box with this corner, painted once: a 9-slice whose middle is the box. `margin` is how far it reaches
// past the box on every side; the 9-slice borders are margin + corner + 1.
QImage shadowImage(int level, int corner, bool light, int *margin);

} // namespace fugue::look

// `theme.look` in QML. Its properties change together, on `changed`.
class CanvasLook : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // Colors (docs/canvas-look.md 6.1).
    Q_PROPERTY(QColor canvas MEMBER m_canvas NOTIFY changed)
    Q_PROPERTY(QColor dot MEMBER m_dot NOTIFY changed)
    Q_PROPERTY(QColor dotMajor MEMBER m_dotMajor NOTIFY changed)
    Q_PROPERTY(QColor body MEMBER m_body NOTIFY changed)
    Q_PROPERTY(QColor header MEMBER m_header NOTIFY changed)
    Q_PROPERTY(QColor hairline MEMBER m_hairline NOTIFY changed)
    Q_PROPERTY(QColor hairlineStrong MEMBER m_hairlineStrong NOTIFY changed)
    Q_PROPERTY(QColor well MEMBER m_well NOTIFY changed)
    // Six colors for the workflow tabs (spec 12.10): the theme's red, yellow, green, cyan, blue and magenta,
    // each at least 3:1 against the tab strip and the current tab; a theme without color gets six hues.
    Q_PROPERTY(QVariantList tabColors MEMBER m_tabColors NOTIFY changed)
    Q_PROPERTY(QColor label MEMBER m_label NOTIFY changed)
    Q_PROPERTY(QColor popup MEMBER m_popup NOTIFY changed)
    Q_PROPERTY(QColor errorHeader MEMBER m_errorHeader NOTIFY changed)
    Q_PROPERTY(QColor errorChip MEMBER m_errorChip NOTIFY changed)
    Q_PROPERTY(QColor selectedRow MEMBER m_selectedRow NOTIFY changed)
    Q_PROPERTY(QColor menuRow MEMBER m_menuRow NOTIFY changed)
    Q_PROPERTY(QColor dangerFill MEMBER m_dangerFill NOTIFY changed)
    Q_PROPERTY(QColor dangerHover MEMBER m_dangerHover NOTIFY changed)
    Q_PROPERTY(QColor dangerSolid MEMBER m_dangerSolid NOTIFY changed)
    Q_PROPERTY(QColor dangerSolidHover MEMBER m_dangerSolidHover NOTIFY changed)
    Q_PROPERTY(QColor dangerInk MEMBER m_dangerInk NOTIFY changed)
    Q_PROPERTY(QColor bevel MEMBER m_bevel NOTIFY changed)
    Q_PROPERTY(QColor sheenTop MEMBER m_sheenTop NOTIFY changed)
    Q_PROPERTY(QColor sheenBottom MEMBER m_sheenBottom NOTIFY changed)
    Q_PROPERTY(QColor litTop MEMBER m_litTop NOTIFY changed)
    Q_PROPERTY(QColor litBottom MEMBER m_litBottom NOTIFY changed)
    Q_PROPERTY(QColor scrim MEMBER m_scrim NOTIFY changed)
    // The Drift kit's (src/kit/, docs/canvas-look.md "Drift kit").
    Q_PROPERTY(QColor kitGroup MEMBER m_kitGroup NOTIFY changed)
    Q_PROPERTY(QColor kitSeparator MEMBER m_kitSeparator NOTIFY changed)
    Q_PROPERTY(QColor kitTrack MEMBER m_kitTrack NOTIFY changed)
    Q_PROPERTY(QColor kitKnob MEMBER m_kitKnob NOTIFY changed)
    Q_PROPERTY(QColor kitChip MEMBER m_kitChip NOTIFY changed)
    Q_PROPERTY(QColor kitChipHover MEMBER m_kitChipHover NOTIFY changed)
    Q_PROPERTY(QColor kitHeroShade MEMBER m_kitHeroShade NOTIFY changed)
    Q_PROPERTY(QColor kitHeroPill MEMBER m_kitHeroPill NOTIFY changed)
    Q_PROPERTY(QColor kitWell MEMBER m_kitWell NOTIFY changed)
    Q_PROPERTY(QColor kitFrost MEMBER m_kitFrost NOTIFY changed)
    Q_PROPERTY(QColor kitFrostHover MEMBER m_kitFrostHover NOTIFY changed)
    Q_PROPERTY(QColor kitFrostEdge MEMBER m_kitFrostEdge NOTIFY changed)
    Q_PROPERTY(QColor kitHeroPillHover MEMBER m_kitHeroPillHover NOTIFY changed)
    Q_PROPERTY(QColor bypassBody MEMBER m_bypassBody NOTIFY changed)
    Q_PROPERTY(QColor bypassHeader MEMBER m_bypassHeader NOTIFY changed)
    Q_PROPERTY(QColor bypassEdge MEMBER m_bypassEdge NOTIFY changed)
    // The theme's own, here too so a delegate reads one object.
    Q_PROPERTY(QColor text MEMBER m_text NOTIFY changed)
    Q_PROPERTY(QColor accent MEMBER m_accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground MEMBER m_accentForeground NOTIFY changed)
    Q_PROPERTY(QColor error MEMBER m_error NOTIFY changed)
    // How frosted the controls over a picture are, 0 (off: plain opaque chips) to 1; settings.json's `frost` (default 0.6).
    Q_PROPERTY(qreal frost READ frost WRITE setFrost NOTIFY changed)
    // The node card's face (docs/canvas-look.md 20.14, #466): "compact" (the Drift card) or "flat" (today's face); settings.json's node_style.
    Q_PROPERTY(QString nodeStyle READ nodeStyle WRITE setNodeStyle NOTIFY changed)
    // Every number of the Drift kit's look by name (look_params.h, docs/canvas-look.md 20.3): the default or look.json's value, times its
    // slider's factor, clamped. Read it as `look.params.cornerCard`; it changes with the sliders and the file, live.
    Q_PROPERTY(QVariantMap params READ params NOTIFY changed)
    // What look.json said that was wrong (an unknown name, a bad value), one line each, empty when it is fine or absent.
    Q_PROPERTY(QStringList lookProblems READ lookProblems NOTIFY changed)
    Q_PROPERTY(bool light MEMBER m_light NOTIFY changed)
    // A port color key from the canvas model (text, number, ..., declared) to its color.
    Q_PROPERTY(QVariantMap ports MEMBER m_ports NOTIFY changed)

    // Sizes (3): t is the text size, u = t / 3.
    Q_PROPERTY(int textSize MEMBER m_textSize NOTIFY changed)
    Q_PROPERTY(qreal unit MEMBER m_unit NOTIFY changed)
    Q_PROPERTY(int corner MEMBER m_corner NOTIFY changed)           // r: nodes, frames, pills' cap
    Q_PROPERTY(int popupCorner MEMBER m_popupCorner NOTIFY changed) // R: popups, overlay
    Q_PROPERTY(int smallCorner MEMBER m_smallCorner NOTIFY changed) // wells, chips, keycaps
    Q_PROPERTY(int thumbCorner MEMBER m_thumbCorner NOTIFY changed)
    Q_PROPERTY(int tooltipDelay MEMBER m_tooltipDelay NOTIFY changed)   // ms a pointer rests before a tooltip shows
    Q_PROPERTY(int labelSize MEMBER m_labelSize NOTIFY changed)     // port and widget labels, values
    Q_PROPERTY(int captionSize MEMBER m_captionSize NOTIFY changed) // times, status words, chips
    Q_PROPERTY(int frameTitleSize MEMBER m_frameTitleSize NOTIFY changed)
    Q_PROPERTY(int searchFieldSize MEMBER m_searchFieldSize NOTIFY changed)
    Q_PROPERTY(int resultSize MEMBER m_resultSize NOTIFY changed)
    Q_PROPERTY(int overlayTitleSize MEMBER m_overlayTitleSize NOTIFY changed)

    // Shadows (7): one 9-slice texture per level, shared by everything at that level. Levels 1,
    // 2 and 4 have the node corner, level 3 the popup corner. shadowMargins[level - 1] is how
    // far each reaches past its box; its 9-slice border is that plus the corner plus 1.
    Q_PROPERTY(QString shadow1 MEMBER m_shadow1 NOTIFY changed)
    Q_PROPERTY(QString shadow2 MEMBER m_shadow2 NOTIFY changed)
    Q_PROPERTY(QString shadow3 MEMBER m_shadow3 NOTIFY changed)
    Q_PROPERTY(QString shadow4 MEMBER m_shadow4 NOTIFY changed)
    Q_PROPERTY(QVariantList shadowMargins MEMBER m_shadowMargins NOTIFY changed)

    // Glyphs (16): Lucide icons as a font, drawn with Text { font.family: theme.look.iconFont;
    // text: theme.look.glyph("check") }, so they recolor and batch with labels.
    Q_PROPERTY(QString iconFont READ iconFont CONSTANT)
    // The serif face of the Drift kit's titles: Fraunces (SIL OFL, share/fonts/LICENSE.fraunces), bundled; empty without a GUI.
    Q_PROPERTY(QString serifFont READ serifFont CONSTANT)

public:
    explicit CanvasLook(QObject *parent = nullptr);

    // Recomputes everything; `colors` has Theme's fallbacks applied.
    void update(const QHash<QString, QColor> &colors, bool light, fugue::look::Corners corners, int textSize);

    // A layout color (spec 4.5: a theme role, a palette name such as "blue" or a semantic one
    // such as "accent" or "muted", or a hex value "#rrggbb", #52) as {frame, frameEdge, frameTitle, tint,
    // tintBody}; an unknown role is muted.
    Q_INVOKABLE QVariantMap role(const QString &name) const;
    // The color a port color key stands for: the type colors, "any" (the label color) and
    // "unknown" (a placeholder's port: the label color too).
    Q_INVOKABLE QColor port(const QString &key) const;
    // The character of a Lucide glyph by its name ("check", "triangle-alert", ...), or empty.
    qreal frost() const { return m_frost; }
    void setFrost(qreal frost) {
        frost = qBound(0.0, frost, 1.0);
        if (qFuzzyCompare(1 + frost, 1 + m_frost))
            return;
        m_frost = frost;
        emit changed();
    }
    QString nodeStyle() const { return m_nodeStyle; }
    void setNodeStyle(const QString &style) {
        const QString use = style == QLatin1String("flat") ? style : QStringLiteral("compact");
        if (use == m_nodeStyle)
            return;
        m_nodeStyle = use;
        emit changed();
    }
    const QVariantMap &params() const { return m_params; }
    QStringList lookProblems() const { return m_lookProblems; }
    // The five sliders' positions (settings.json look_colour, look_glow, look_corners, look_spacing, look_depth, each 0 to 1).
    void setSliders(const fugue::look::Sliders &sliders);
    // The file of the power user's overrides, read now and again whenever it changes; "" stops watching.
    void setLookFile(const QString &path);
    const QString &lookFile() const { return m_lookFile; }
    // The look preset's parameters, the layer between the defaults and look.json.
    void setPreset(const QVariantMap &params);
    // A header tone's colours (docs/canvas-look.md 20.5, 20.8): {base, blobs: [3 colors], glass}, `glass` being the colour of a toggle,
    // a chooser's thumb and a slider's fill in a window of that tone. An unknown name is dusk.
    Q_INVOKABLE QVariantMap tone(const QString &name) const {
        struct Tone { const char *name, *base, *blob0, *blob1, *blob2, *glass; };
        static const Tone tones[] = {
            {"dusk", "#2a2533", "#b98bd6", "#e3828f", "#6f86d6", "#968ceb"},
            {"sea", "#1d2a31", "#6fc3c9", "#6f9fd6", "#8fd6c0", "#6ec8cd"},
            {"ember", "#32251f", "#e8a87c", "#e3828f", "#d6b46f", "#eba578"},
            {"moss", "#1f2e28", "#8fcf9a", "#6fc3a8", "#b7d67a", "#78cd8c"},
            {"slate", "#242933", "#8fa6c9", "#a9b4c9", "#6f86d6", "#82a0e1"},
        };
        const Tone *found = &tones[0];
        for (const Tone &t : tones)
            if (name == QLatin1String(t.name))
                found = &t;
        return {{"base", QColor(QLatin1String(found->base))},
                {"blobs", QVariantList{QColor(QLatin1String(found->blob0)), QColor(QLatin1String(found->blob1)), QColor(QLatin1String(found->blob2))}},
                {"glass", QColor(QLatin1String(found->glass))}};
    }
    // The colour of a node's category dot (docs/canvas-look.md 20.14, #476): the top level of the manifest's category ("Image/Adjust" is
    // Image) picks a header tone, and the colour is moved only in lightness until it reads at 3:1 on the card, so it follows a light
    // or dark theme. Transparent for a category with none (a pack's own): the caller draws its kind mark.
    Q_INVOKABLE QColor categoryColor(const QString &category) const;
    Q_INVOKABLE QString glyph(const QString &name) const;
    QString iconFont() const;
    QString serifFont() const;

    const fugue::look::Tokens &tokens() const { return m_tokens; }
    QColor portColor(const QString &key) const { return m_portColors.value(key, m_portColors.value("declared")); }

signals:
    void changed();

private:
    fugue::look::Tokens m_tokens;
    QHash<QString, QColor> m_colors;
    QHash<QString, QColor> m_portColors;
    mutable QHash<QString, QVariantMap> m_roles;

    QColor m_canvas, m_dot, m_dotMajor, m_body, m_header, m_hairline, m_hairlineStrong, m_well, m_label, m_popup;
    QColor m_dangerSolid, m_dangerSolidHover, m_dangerInk;
    QColor m_errorHeader, m_errorChip, m_selectedRow, m_menuRow, m_dangerFill, m_dangerHover, m_bevel, m_scrim, m_bypassBody, m_bypassHeader,
        m_bypassEdge;
    QColor m_sheenTop, m_sheenBottom, m_litTop, m_litBottom;
    QColor m_kitGroup, m_kitSeparator, m_kitTrack, m_kitKnob, m_kitChip, m_kitChipHover, m_kitHeroShade, m_kitHeroPill,
        m_kitHeroPillHover, m_kitFrost, m_kitFrostHover, m_kitFrostEdge, m_kitWell;
    QColor m_text, m_accent, m_accentForeground, m_error;
    bool m_light = false;
    qreal m_frost = 0.6;
    QString m_nodeStyle = QStringLiteral("compact");
    fugue::look::Sliders m_sliders;
    QVariantMap m_overrides, m_preset, m_params;
    QStringList m_lookProblems;
    QString m_lookFile;
    QFileSystemWatcher *m_watcher = nullptr;
    void resolve();
    void readLookFile();
    QVariantMap m_ports;
    QVariantList m_tabColors;
    int m_textSize = 12;
    qreal m_unit = 4;
    int m_tooltipDelay = 700;
    int m_corner = 0, m_popupCorner = 0, m_smallCorner = 0, m_thumbCorner = 0;
    int m_labelSize = 11, m_captionSize = 10, m_frameTitleSize = 16, m_searchFieldSize = 15, m_resultSize = 13,
        m_overlayTitleSize = 18;
    QString m_shadow1, m_shadow2, m_shadow3, m_shadow4;
    QVariantList m_shadowMargins;
};
