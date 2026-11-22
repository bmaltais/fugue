#pragma once

// What a press, a drag and a release on the canvas mean (spec 12.3, 12.4), split out of
// FugueCanvas.qml like GroupEdit (canvas/group_edit.h) and exposed to QML as `model.gesture`.
// The window feeds it pointer events and it makes the model edits itself, so the priority of a
// press, the travel threshold, the release rules and the double-click targets are tested with
// a CanvasModel and no window (tests/canvas/test_canvas_gesture.cpp).
//
// Events in: admit() and press(), move(), release(), cancel() and doubleClick(). Each carries the pointer
// twice, in canvas (screen) coordinates and in world coordinates, because only the window knows
// the magnifier (#74), through which a screen point maps to another world. A press and a double
// click also carry `portTolerance`, how far from a port still hits it, in world units at the
// zoom the point is seen at (the magnifier's inside it); zero or less means ports cannot be hit
// there (the overview, where a press is on the node), and `onCutHandle`, whether the point is on
// the hovered wire's cut handle, which the window draws.
//
// What the view tells it, as properties the window binds: `navigation`, `spaceHeld`, `tier`,
// `zoom`, `panX`/`panY` (the marquee's corners map through them) and `frameBand` (the height of
// a Frame's title band in the world, which depends on the look).
//
// Out, what only the window can do, as signals: `panStarted` (it keeps the pan's origin and
// moves the world on every move while `current` is "pan"), `panFinished`, `cutHandlePressed`,
// `pressStarted` (the window forgets what it hovered), `finished`, `dropRequested` (a link
// let go on a node: the window's chooser), `linkReleased`, `searchRequested`,
// `contextMenuRequested` and `viewerRequested`. Everything else is a call on the model.
//
// The state is read-only: `current` is "" with no gesture, else "pan", "move", "link",
// "marquee", "resize", or "alt" (a press with Alt on a node, not yet a drag).

#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

class CanvasModel;

class CanvasGesture : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through the canvas model")
    // What the view tells the module.
    Q_PROPERTY(QString navigation READ navigation WRITE setNavigation NOTIFY viewChanged)
    Q_PROPERTY(bool spaceHeld READ spaceHeld WRITE setSpaceHeld NOTIFY viewChanged)
    Q_PROPERTY(int tier READ tier WRITE setTier NOTIFY viewChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY viewChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY viewChanged)
    Q_PROPERTY(qreal frameBand READ frameBand WRITE setFrameBand NOTIFY viewChanged)
    // The state, while a button is down.
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)
    Q_PROPERTY(bool travelled READ travelled NOTIFY travelledChanged)
    Q_PROPERTY(QString pressedNode READ pressedNode NOTIFY pressedNodeChanged)
    Q_PROPERTY(QString resizeCorner READ resizeCorner NOTIFY resizeCornerChanged)
    Q_PROPERTY(QPointF pressAt READ pressAt NOTIFY pressAtChanged)
    Q_PROPERTY(QPointF pointer READ pointer NOTIFY pointerChanged)

public:
    explicit CanvasGesture(CanvasModel *canvas, QObject *parent = nullptr);

    QString navigation() const { return m_navigation; }
    bool spaceHeld() const { return m_spaceHeld; }
    int tier() const { return m_tier; }
    qreal zoom() const { return m_zoom; }
    qreal panX() const { return m_panX; }
    qreal panY() const { return m_panY; }
    qreal frameBand() const { return m_frameBand; }
    void setNavigation(const QString &navigation);
    void setSpaceHeld(bool held);
    void setTier(int tier);
    void setZoom(qreal zoom);
    void setPanX(qreal x);
    void setPanY(qreal y);
    void setFrameBand(qreal band);

    QString current() const { return m_current; }
    bool travelled() const { return m_travelled; }
    QString pressedNode() const { return m_pressedNode; }
    QString resizeCorner() const { return m_resizeCorner; }
    QPointF pressAt() const { return m_pressAt; }
    QPointF pointer() const { return m_pointer; }

    // A button went down: first admit() it, then, once the window has done what a press does
    // whatever is under it (the focus, the magnifier), press() it with the points worked out after
    // that. admit() is false for a second button while one holds a gesture: it is left out of it and
    // its release does nothing (no context menu mid-drag). `buttons` is every button down now. A
    // gesture whose release never came is finished first.
    Q_INVOKABLE bool admit(int button, int buttons);
    Q_INVOKABLE void press(const QPointF &screen, const QPointF &world, int button, int modifiers,
                           qreal portTolerance, bool onCutHandle);
    // The pointer moved (with a button down or not).
    Q_INVOKABLE void move(const QPointF &screen, const QPointF &world, qreal portTolerance);
    Q_INVOKABLE void release(const QPointF &screen, const QPointF &world, int button, int modifiers,
                             qreal portTolerance);
    // The window lost the pointer: the gesture ends where it was.
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void doubleClick(const QPointF &screen, const QPointF &world, int button, qreal portTolerance);

signals:
    void viewChanged();
    void currentChanged();
    void travelledChanged();
    void pressedNodeChanged();
    void resizeCornerChanged();
    void pressAtChanged();
    void pointerChanged();
    // The pan began (the window keeps the world's position then) and ended (it remembers the view).
    void panStarted();
    void panFinished();
    // A press began a gesture on the canvas: the window forgets what it hovered.
    void pressStarted();
    // A press on the hovered wire's cut handle: the window cuts the link it is showing.
    void cutHandlePressed();
    // A gesture ended (`kind` is what it was, "" when none was held): the window clears what it
    // showed for it.
    void finished(const QString &kind);
    // A link let go on a node (not on a port): the window picks the port. `at` is the screen point.
    void dropRequested(const QString &node, const QPointF &at);
    // A link let go on empty canvas: `from` is {node, port, output}, the point in world units.
    void linkReleased(const QVariantMap &from, const QPointF &world);
    // A double-click on empty canvas, and a right-click (both in the unit of the event's point).
    void searchRequested(const QPointF &world);
    void contextMenuRequested(const QPointF &screen);
    // A double-click on a node's picture. `kind` is "compare", "crop", "mask", "paint" or "image". The window
    // opens it when the node has a picture to show, and ends the move the click began first.
    void viewerRequested(const QString &kind, const QString &node);

private:
    void startPan();
    void finish(const QPointF &screen, const QPointF &world, int modifiers, qreal portTolerance);
    void begin(const QString &gesture);
    QVariantMap portAt(const QPointF &world, qreal tolerance) const;
    QVariantMap cornerAt(const QPointF &world) const;
    QPointF toWorld(const QPointF &screen) const;
    void setCurrent(const QString &current);
    void setTravelled(bool travelled);
    void setPointer(const QPointF &pointer);

    CanvasModel *m_canvas;
    QString m_navigation = QStringLiteral("legacy");
    bool m_spaceHeld = false;
    int m_tier = 0;
    qreal m_zoom = 1;
    qreal m_panX = 0, m_panY = 0;
    qreal m_frameBand = 0;

    QString m_current;
    int m_button = 0;            // the button that holds the gesture
    int m_ignored = 0;           // pressed while another held a gesture: their release does nothing
    QPointF m_pressAt;           // where the gesture began, in canvas coordinates
    QPointF m_rightPressAt;      // where the right button went down
    QPointF m_pressWorld;        // the press in world coordinates
    QPointF m_pointer;           // the pointer, canvas coordinates
    QPointF m_pointerWorld;      // and in world coordinates, as of the last event
    qreal m_tolerance = 0;       // the port tolerance of the last event
    bool m_travelled = false;
    QString m_pressedNode;
    bool m_pressedWasSelected = false;
    int m_pressModifiers = 0;
    QString m_resizeCorner;
};
