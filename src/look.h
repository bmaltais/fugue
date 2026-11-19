#pragma once

// The canvas look (docs/canvas-look.md): every color, size and shadow the canvas, node search
// and the `?` overlay draw with, derived from three theme inputs (the palette, the corner
// radius and the text size), so it holds on every Omarchy theme, light or dark.
//
// The color rules are docs/canvas-look/derive.py's, which is the reference: tests compare the
// two (tests/canvas/test_look_tokens.cpp). Change both together.
//
// QML reads it as `theme.look` (a CanvasLook): for example `theme.look.body`,
// `theme.look.labelSize`, `theme.look.role("blue").tint`. Theme::muted() and the other plain
// Theme colors stay as they were for the rest of the window (owner's decision, 18.3).

#include <QColor>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

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
    QColor errorHeader, errorChip, selectedRow;
    QColor bevel; // the header's top highlight (dark themes), with alpha
    QColor scrim; // behind the `?` overlay, with alpha
    double labelShare = 0; // the share of text in `label` the 4.5:1 rule settled on
};

// `colors` holds at least background, foreground, accent and red (Theme's fallbacks applied).
Tokens tokens(const QHash<QString, QColor> &colors, bool light);

// A layout color (spec 4.5) as a frame fill, its edge and title, and a node header tint.
struct RoleTokens {
    QColor frame, frameEdge, frameTitle, tint;
};
RoleTokens roleTokens(const Tokens &tokens, const QColor &role, const QColor &text, bool light);

// The port type colors (6.2), keyed text, number, boolean, image, video, audio, declared:
// the theme's own, a colliding one moved to a distinct hue, and each lifted to 3:1 against
// the node and the canvas. A monochrome theme keeps its grays (`mono` set).
QHash<QString, QColor> portColors(const QHash<QString, QColor> &colors, bool light, const Tokens &tokens,
                                  bool *mono = nullptr);

// --- shadows (7) ---

// One elevation level's shadow (1: node, 2: selected node and pills, 3: popups) for a box with
// this corner, painted once: a 9-slice whose middle is the box. `margin` is how far it reaches
// past the box on every side; the 9-slice borders are margin + corner + 1.
QImage shadowImage(int level, int corner, bool light, int *margin);

} // namespace fugue::look

// `theme.look` in QML. Its properties change together, on `changed`.
class CanvasLook : public QObject {
    Q_OBJECT
    // Colors (docs/canvas-look.md 6.1).
    Q_PROPERTY(QColor canvas MEMBER m_canvas NOTIFY changed)
    Q_PROPERTY(QColor dot MEMBER m_dot NOTIFY changed)
    Q_PROPERTY(QColor dotMajor MEMBER m_dotMajor NOTIFY changed)
    Q_PROPERTY(QColor body MEMBER m_body NOTIFY changed)
    Q_PROPERTY(QColor header MEMBER m_header NOTIFY changed)
    Q_PROPERTY(QColor hairline MEMBER m_hairline NOTIFY changed)
    Q_PROPERTY(QColor hairlineStrong MEMBER m_hairlineStrong NOTIFY changed)
    Q_PROPERTY(QColor well MEMBER m_well NOTIFY changed)
    Q_PROPERTY(QColor label MEMBER m_label NOTIFY changed)
    Q_PROPERTY(QColor popup MEMBER m_popup NOTIFY changed)
    Q_PROPERTY(QColor errorHeader MEMBER m_errorHeader NOTIFY changed)
    Q_PROPERTY(QColor errorChip MEMBER m_errorChip NOTIFY changed)
    Q_PROPERTY(QColor selectedRow MEMBER m_selectedRow NOTIFY changed)
    Q_PROPERTY(QColor bevel MEMBER m_bevel NOTIFY changed)
    Q_PROPERTY(QColor scrim MEMBER m_scrim NOTIFY changed)
    // The theme's own, here too so a delegate reads one object.
    Q_PROPERTY(QColor text MEMBER m_text NOTIFY changed)
    Q_PROPERTY(QColor accent MEMBER m_accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground MEMBER m_accentForeground NOTIFY changed)
    Q_PROPERTY(QColor error MEMBER m_error NOTIFY changed)
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
    Q_PROPERTY(int labelSize MEMBER m_labelSize NOTIFY changed)     // port and widget labels, values
    Q_PROPERTY(int captionSize MEMBER m_captionSize NOTIFY changed) // times, status words, chips
    Q_PROPERTY(int frameTitleSize MEMBER m_frameTitleSize NOTIFY changed)
    Q_PROPERTY(int searchFieldSize MEMBER m_searchFieldSize NOTIFY changed)
    Q_PROPERTY(int resultSize MEMBER m_resultSize NOTIFY changed)
    Q_PROPERTY(int overlayTitleSize MEMBER m_overlayTitleSize NOTIFY changed)

    // Shadows (7): one 9-slice texture per level, shared by everything at that level. Levels 1
    // and 2 have the node corner, level 3 the popup corner. shadowMargins[level - 1] is how far
    // each reaches past its box; its 9-slice border is that plus the corner plus 1.
    Q_PROPERTY(QString shadow1 MEMBER m_shadow1 NOTIFY changed)
    Q_PROPERTY(QString shadow2 MEMBER m_shadow2 NOTIFY changed)
    Q_PROPERTY(QString shadow3 MEMBER m_shadow3 NOTIFY changed)
    Q_PROPERTY(QVariantList shadowMargins MEMBER m_shadowMargins NOTIFY changed)

    // Glyphs (16): Lucide icons as a font, drawn with Text { font.family: theme.look.iconFont;
    // text: theme.look.glyph("check") }, so they recolor and batch with labels.
    Q_PROPERTY(QString iconFont READ iconFont CONSTANT)

public:
    explicit CanvasLook(QObject *parent = nullptr);

    // Recomputes everything; `colors` has Theme's fallbacks applied.
    void update(const QHash<QString, QColor> &colors, bool light, int radius, int textSize);

    // A layout color (spec 4.5: a theme role, a palette name such as "blue" or a semantic one
    // such as "accent" or "muted") as {frame, frameEdge, frameTitle, tint}; an unknown role is
    // muted.
    Q_INVOKABLE QVariantMap role(const QString &name) const;
    // The color a port color key stands for: the type colors, "any" (the label color) and
    // "unknown" (a placeholder's port: the label color too).
    Q_INVOKABLE QColor port(const QString &key) const;
    // The character of a Lucide glyph by its name ("check", "triangle-alert", ...), or empty.
    Q_INVOKABLE QString glyph(const QString &name) const;
    QString iconFont() const;

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
    QColor m_errorHeader, m_errorChip, m_selectedRow, m_bevel, m_scrim;
    QColor m_text, m_accent, m_accentForeground, m_error;
    bool m_light = false;
    QVariantMap m_ports;
    int m_textSize = 12;
    qreal m_unit = 4;
    int m_corner = 0, m_popupCorner = 0, m_smallCorner = 0, m_thumbCorner = 0;
    int m_labelSize = 11, m_captionSize = 10, m_frameTitleSize = 16, m_searchFieldSize = 15, m_resultSize = 13,
        m_overlayTitleSize = 18;
    QString m_shadow1, m_shadow2, m_shadow3;
    QVariantList m_shadowMargins;
};
