#pragma once

// Where the canvas task bar (#231, CanvasTaskBar.qml) sits when the person has moved it (#381; Photoshop's Contextual Task Bar
// has a grip, Reset position and Pin position). Until it is moved the bar floats under the selection, so `hasPlace` is false
// and the view uses its own default. Once moved it is kept as fractions (0 to 1) of the room the bar has in the picture's area
// (src/engine/float_place.h: the same as the run toolbar's), so a resized window keeps it in the same relative place and it
// can never lie outside the area: `positionIn` holds it inside, whatever the size. `pinned` locks it where it is (the grip
// no longer moves it) until it is unpinned or reset; Reset position forgets the place and the pin, and the bar follows the
// selection again.
//
// Kept in one small file of the state folder: {"place": {"x", "y"}, "pinned": true}. No file, or a bad one: the default.

#include <QObject>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class TaskBarPlace : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App.canvasActions")
    Q_PROPERTY(bool hasPlace READ hasPlace NOTIFY changed)
    Q_PROPERTY(QPointF place READ place WRITE setPlace NOTIFY changed) // fractions of the room, held to 0..1
    Q_PROPERTY(bool pinned READ pinned WRITE setPinned NOTIFY changed)

public:
    // `path` is the file it is kept in; empty keeps nothing.
    explicit TaskBarPlace(QString path, QObject *parent = nullptr);

    bool hasPlace() const { return m_has; }
    QPointF place() const { return m_place; }
    bool pinned() const { return m_pinned; }
    // A place is held to 0..1 on each axis (a bad number reads as the middle). Moving a pinned bar is refused.
    void setPlace(const QPointF &place);
    void setPinned(bool pinned);
    // Back to the default: no place, not pinned.
    Q_INVOKABLE void reset();
    // Where the bar's top left lies in an area, `margin` from its edges: its place held inside, whatever the sizes.
    Q_INVOKABLE QPointF positionIn(const QSizeF &area, const QSizeF &box, double margin) const;

signals:
    void changed();

private:
    void save() const;

    QString m_path;
    bool m_has = false;
    QPointF m_place{0.5, 1};
    bool m_pinned = false;
};

} // namespace fugue
