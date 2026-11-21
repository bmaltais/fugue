#pragma once

// A layout color that is not a theme role (#52, spec 4.5): the color of a node or a Frame may be a
// theme role name ("blue", "accent") or a hex value "#rrggbb". A hex value is a hue, not a final
// color: the look derives the node's tint and the Frame's fill from it the way it does from a
// role, mixing it into the theme's own surfaces and stepping the share down until the text reads,
// so it follows light and dark themes (docs/canvas-look.md 6.1). The file stores the one string.
//
// Also the color math of the wheel (hue, saturation, value) and the list of recently used colors.

#include <QString>
#include <QStringList>

namespace fugue {

// Hue in degrees [0, 360), saturation and value in [0, 1].
struct Hsv {
    double h = 0;
    double s = 0;
    double v = 0;
};

// "#rrggbb" in lower case from "#rrggbb", "rrggbb", "#rgb" or "rgb" (any case, spaces around), or
// an empty string when the text is not one of those. An alpha channel is refused.
QString normalizeHex(const QString &text);

// True for a whole "#rrggbb" (any case): the stored form of a hex layout color.
bool isHexColor(const QString &text);

// The hue, saturation and value of a hex color; black when the text is not one.
Hsv hsvFromHex(const QString &hex);

// "#rrggbb" for an Hsv; the hue wraps around, saturation and value stop at 0 and 1.
QString hexFromHsv(const Hsv &hsv);

// The three channels of a color, 0 to 255.
struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

// What a picker's number fields show for an Hsv: hue in whole degrees [0, 359] (359.6 rounds to 0,
// not 360), saturation and brightness in whole percent [0, 100].
struct Hsb {
    int h = 0;
    int s = 0;
    int b = 0;
};

// The channels of a hex color; black when the text is not one.
Rgb rgbFromHex(const QString &hex);

// "#rrggbb" for channels; each stops at 0 and 255.
QString hexFromRgb(int r, int g, int b);

// The whole numbers a picker shows for an Hsv (see Hsb).
Hsb hsbRounded(const Hsv &hsv);

// The colors last applied, newest first, with no repeats, kept in a small JSON file (a list of
// "#rrggbb") so they outlast the session. A file that cannot be read starts the list empty.
class RecentColors {
public:
    explicit RecentColors(QString path, int limit = 12);

    QStringList list() const { return m_colors; }
    // Moves `color` (any form normalizeHex takes) to the front and saves; text that is not a hex
    // color changes nothing.
    void use(const QString &color);

private:
    QString m_path;
    int m_limit;
    QStringList m_colors;
};

} // namespace fugue
