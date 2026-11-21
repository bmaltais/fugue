#pragma once

// The colors used lately and the wheel's color math, for any QML that picks a color (#52): the
// color picker (src/canvas/ColorPickerPane.qml) reads and fills it, and a node's color widget or
// another surface can do the same. It knows nothing of nodes, Frames or the canvas. The list is
// newest first, kept in a file of the state folder (engine/layout_color.h RecentColors).

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/layout_color.h"

namespace fugue {

class ColorHistory : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(QStringList recent READ recent NOTIFY recentChanged)

public:
    explicit ColorHistory(QString path, QObject *parent = nullptr) : QObject(parent), m_recent(std::move(path)) {}

    QStringList recent() const { return m_recent.list(); }

    // Moves a color (any form `normalize` takes) to the front; false when it is not a hex color.
    Q_INVOKABLE bool use(const QString &color);
    // {h, s, v} (degrees, 0 to 1, 0 to 1) of a hex color, and "#rrggbb" from them.
    Q_INVOKABLE QVariantMap hsv(const QString &hex) const;
    Q_INVOKABLE QString hex(double h, double s, double v) const { return hexFromHsv({h, s, v}); }
    // {r, g, b} (0 to 255) of a hex color, and "#rrggbb" from them (each stops at 0 and 255).
    Q_INVOKABLE QVariantMap rgb(const QString &hex) const;
    Q_INVOKABLE QString hexFromRgb(int r, int g, int b) const { return fugue::hexFromRgb(r, g, b); }
    // {h, s, b} in whole degrees [0, 359] and whole percent, as the picker's number fields show an
    // h, s, v of hsv().
    Q_INVOKABLE QVariantMap hsb(double h, double s, double v) const;
    // "#rrggbb" from what a person typed ("#F80", "ff8800"), or empty.
    Q_INVOKABLE QString normalize(const QString &text) const { return normalizeHex(text); }

signals:
    void recentChanged();

private:
    RecentColors m_recent;
};

} // namespace fugue
