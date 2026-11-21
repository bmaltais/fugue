#pragma once

// A hex layout color on the canvas (#52, spec 4.5): what the color picker's canvas caller needs.
// The picker itself knows none of this; node and Frame coloring is its first caller
// (src/canvas/LayoutColorPicker.qml).

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

class CanvasModel;

namespace fugue {

class LayoutColors : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")

public:
    explicit LayoutColors(CanvasModel &canvas, QObject *parent = nullptr) : QObject(parent), m_canvas(canvas) {}

    // Gives the Frame `frame` when set, else the selected nodes, the color `hex` (any form
    // normalizeHex takes) as one undo step. False, with nothing changed, when it is not one.
    Q_INVOKABLE bool apply(const QString &hex, const QString &frame = {});
    // The hex color the Frame, or else the first selected node, has now; empty when it has a theme
    // role or none, so the picker knows where to start.
    Q_INVOKABLE QString current(const QString &frame = {}) const;

private:
    CanvasModel &m_canvas;
};

} // namespace fugue
