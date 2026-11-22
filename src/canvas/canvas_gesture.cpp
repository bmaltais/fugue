#include "canvas/canvas_gesture.h"

#include "canvas/canvas_model.h"

#include <QGuiApplication>
#include <QStyleHints>
#include <QtMath>

namespace {

bool has(int flags, Qt::KeyboardModifier modifier) { return (flags & modifier) != 0; }

qreal dragDistance() {
    return QGuiApplication::styleHints() ? QGuiApplication::styleHints()->startDragDistance() : 10;
}

qreal distance(const QPointF &a, const QPointF &b) { return qHypot(a.x() - b.x(), a.y() - b.y()); }

} // namespace

CanvasGesture::CanvasGesture(CanvasModel *canvas, QObject *parent) : QObject(parent), m_canvas(canvas) {}

#define FUGUE_VIEW_SETTER(Name, member, Type)                                                                          \
    void CanvasGesture::set##Name(Type value) {                                                                        \
        if (member == value)                                                                                           \
            return;                                                                                                    \
        member = value;                                                                                                \
        emit viewChanged();                                                                                            \
    }
FUGUE_VIEW_SETTER(Navigation, m_navigation, const QString &)
FUGUE_VIEW_SETTER(SpaceHeld, m_spaceHeld, bool)
FUGUE_VIEW_SETTER(Tier, m_tier, int)
FUGUE_VIEW_SETTER(Zoom, m_zoom, qreal)
FUGUE_VIEW_SETTER(PanX, m_panX, qreal)
FUGUE_VIEW_SETTER(PanY, m_panY, qreal)
FUGUE_VIEW_SETTER(FrameBand, m_frameBand, qreal)
#undef FUGUE_VIEW_SETTER

void CanvasGesture::setCurrent(const QString &current) {
    if (m_current == current)
        return;
    m_current = current;
    emit currentChanged();
}

void CanvasGesture::setTravelled(bool travelled) {
    if (m_travelled == travelled)
        return;
    m_travelled = travelled;
    emit travelledChanged();
}

void CanvasGesture::setPointer(const QPointF &pointer) {
    if (m_pointer == pointer)
        return;
    m_pointer = pointer;
    emit pointerChanged();
}

// The plain map of the view, for the marquee: it never starts in the magnifier (a press there closes it).
QPointF CanvasGesture::toWorld(const QPointF &screen) const {
    return {(screen.x() - m_panX) / m_zoom, (screen.y() - m_panY) / m_zoom};
}

QVariantMap CanvasGesture::portAt(const QPointF &world, qreal tolerance) const {
    return tolerance > 0 ? m_canvas->portAt(world.x(), world.y(), tolerance) : QVariantMap();
}

// A node's corner, unless the view is too far out for corners or a port is there too (a port wins).
QVariantMap CanvasGesture::cornerAt(const QPointF &world) const {
    if (m_tier != 0 || m_canvas->portAt(world.x(), world.y(), qMax(8.0, 10 / m_zoom)).contains(QStringLiteral("node")))
        return {};
    return m_canvas->resizeCornerAt(world.x(), world.y(), qMax(4 * m_canvas->unit(), 12 / m_zoom));
}

void CanvasGesture::startPan() {
    setCurrent(QStringLiteral("pan"));
    emit panStarted();
}

bool CanvasGesture::admit(int button, int buttons) {
    // A second button while one holds a gesture is left out of it: the gesture goes on, and that
    // button's release does nothing (no context menu mid-drag).
    if (!m_current.isEmpty()) {
        if (buttons & m_button) {
            m_ignored |= button;
            return false;
        }
        finish(m_pointer, m_pointerWorld, 0, m_tolerance); // its release never came
    }
    return true;
}

void CanvasGesture::press(const QPointF &screen, const QPointF &world, int button, int modifiers, qreal portTolerance,
                          bool onCutHandle) {
    if (button == Qt::RightButton) {
        m_rightPressAt = screen; // a click opens the menu on release
        return;
    }
    m_button = button;
    m_pressAt = screen;
    emit pressAtChanged();
    setPointer(screen);
    m_pressWorld = world;
    m_pointerWorld = world;
    m_tolerance = portTolerance;
    setTravelled(false);
    m_pressModifiers = modifiers;

    if (button == Qt::MiddleButton || m_spaceHeld) {
        startPan();
        return;
    }
    if (onCutHandle) {
        emit cutHandlePressed();
        return;
    }
    // A node's corner resizes it.
    const QVariantMap corner = button == Qt::LeftButton ? cornerAt(world) : QVariantMap();
    emit pressStarted();
    if (corner.contains(QStringLiteral("node"))) {
        const QString node = corner.value(QStringLiteral("node")).toString();
        setCurrent(QStringLiteral("resize"));
        m_pressedNode = node;
        emit pressedNodeChanged();
        m_resizeCorner = corner.value(QStringLiteral("corner")).toString();
        emit resizeCornerChanged();
        m_canvas->beginResize(node, m_resizeCorner);
        return;
    }
    const QVariantMap port = portAt(world, portTolerance);
    if (port.contains(QStringLiteral("node"))) {
        const QString node = port.value(QStringLiteral("node")).toString();
        const QString name = port.value(QStringLiteral("port")).toString();
        const bool output = port.value(QStringLiteral("output")).toBool();
        if (has(modifiers, Qt::ControlModifier) && has(modifiers, Qt::AltModifier)) {
            m_canvas->disconnectPort(node, name, output);
            return;
        }
        setCurrent(QStringLiteral("link"));
        if (output && has(modifiers, Qt::ShiftModifier))
            m_canvas->beginLinksMove(node, name);
        else
            m_canvas->beginLinkDrag(node, name, output);
        return;
    }
    // The Advanced section's header opens and folds it (5.4, 6.1).
    if (button == Qt::LeftButton && m_tier == 0) {
        const QString folded = m_canvas->advancedHeaderAt(world.x(), world.y());
        if (!folded.isEmpty()) {
            m_canvas->toggleAdvanced(folded);
            return;
        }
    }
    const QString hit = m_canvas->nodeAt(world.x(), world.y());
    if (!hit.isEmpty()) {
        m_pressedNode = hit;
        emit pressedNodeChanged();
        m_pressedWasSelected = m_canvas->selectedIds().contains(hit);
        if (has(modifiers, Qt::AltModifier) && !has(modifiers, Qt::ControlModifier)) {
            setCurrent(QStringLiteral("alt")); // a clone once it travels
            return;
        }
        if (has(modifiers, Qt::ShiftModifier) || has(modifiers, Qt::ControlModifier))
            m_canvas->select({hit}, QStringLiteral("add"));
        else if (!m_pressedWasSelected)
            m_canvas->select({hit}, QStringLiteral("replace"));
        setCurrent(QStringLiteral("move"));
        m_canvas->beginMove(hit);
        return;
    }
    // A Frame's title band moves it with what it holds (5.1).
    const QString frame = button == Qt::LeftButton ? m_canvas->frameAt(world.x(), world.y(), m_frameBand) : QString();
    if (!frame.isEmpty()) {
        m_pressedNode.clear();
        emit pressedNodeChanged();
        setCurrent(QStringLiteral("move"));
        m_canvas->beginFrameMove(frame);
        return;
    }
    if (m_navigation == QLatin1String("standard") || has(modifiers, Qt::ControlModifier))
        setCurrent(QStringLiteral("marquee"));
    else
        startPan();
}

void CanvasGesture::move(const QPointF &screen, const QPointF &world, qreal portTolerance) {
    setPointer(screen);
    m_pointerWorld = world;
    m_tolerance = portTolerance;
    if (!m_travelled && distance(screen, m_pressAt) >= dragDistance())
        setTravelled(true);
    if (m_current == QLatin1String("alt") && m_travelled) {
        m_canvas->beginCloneMove(m_pressedNode);
        setCurrent(QStringLiteral("move"));
    }
    if (m_current == QLatin1String("move"))
        m_canvas->moveBy(world.x() - m_pressWorld.x(), world.y() - m_pressWorld.y());
    else if (m_current == QLatin1String("resize"))
        m_canvas->resizeBy(world.x() - m_pressWorld.x(), world.y() - m_pressWorld.y());
}

void CanvasGesture::release(const QPointF &screen, const QPointF &world, int button, int modifiers,
                            qreal portTolerance) {
    if (m_ignored & button) {
        m_ignored &= ~button;
        return;
    }
    // A right-click opens the context menu; a right-drag does nothing (12.3).
    if (button == Qt::RightButton) {
        if (m_current.isEmpty() && distance(screen, m_rightPressAt) < dragDistance())
            emit contextMenuRequested(screen);
        return;
    }
    finish(screen, world, modifiers, portTolerance);
}

void CanvasGesture::cancel() {
    m_ignored = 0;
    finish(m_pointer, m_pointerWorld, 0, m_tolerance);
}

void CanvasGesture::finish(const QPointF &screen, const QPointF &world, int modifiers, qreal portTolerance) {
    const QString kind = m_current;
    setCurrent(QString());
    if (kind == QLatin1String("pan")) {
        // A left click on empty canvas, not a drag, clears the selection.
        if (!m_travelled && !m_spaceHeld && m_navigation == QLatin1String("legacy"))
            m_canvas->select({}, QStringLiteral("replace"));
        emit panFinished();
    } else if (kind == QLatin1String("resize")) {
        m_canvas->endResize();
        m_resizeCorner.clear();
        emit resizeCornerChanged();
    } else if (kind == QLatin1String("move")) {
        m_canvas->endMove();
        if (!m_travelled && !m_pressedNode.isEmpty()) {
            if (!has(m_pressModifiers, Qt::ShiftModifier) && !has(m_pressModifiers, Qt::ControlModifier))
                m_canvas->select({m_pressedNode}, QStringLiteral("replace"));
            else if (m_pressedWasSelected)
                m_canvas->select({m_pressedNode}, QStringLiteral("subtract"));
        }
    } else if (kind == QLatin1String("alt")) {
        // Alt+click, not a drag: the node leaves the selection.
        m_canvas->select({m_pressedNode}, QStringLiteral("subtract"));
    } else if (kind == QLatin1String("link")) {
        const QVariantMap drag = m_canvas->linkDrag();
        // The model ended the drag already (an undo, an edit from a key): nothing to let go.
        if (drag.contains(QStringLiteral("node"))) {
            QVariantMap from;
            for (const char *key : {"node", "port", "output"})
                from.insert(QLatin1String(key), drag.value(QLatin1String(key)));
            // Links being moved (Shift+drag) let go on nothing stay as they were: no search.
            const bool moving = drag.contains(QStringLiteral("targets"));
            const QVariantMap port = portAt(world, portTolerance);
            const QString onNode = m_canvas->nodeAt(world.x(), world.y());
            if (port.contains(QStringLiteral("node"))) {
                m_canvas->endLinkDrag(port.value(QStringLiteral("node")).toString(),
                                      port.value(QStringLiteral("port")).toString(),
                                      port.value(QStringLiteral("output")).toBool());
            } else if (!onNode.isEmpty() && m_travelled && !moving) {
                emit dropRequested(onNode, screen);
            } else {
                m_canvas->endLinkDrag();
                if (m_travelled && !moving && onNode.isEmpty())
                    emit linkReleased(from, world);
            }
        }
    } else if (kind == QLatin1String("marquee")) {
        const QPointF a = toWorld({qMin(m_pressAt.x(), screen.x()), qMin(m_pressAt.y(), screen.y())});
        const QPointF b = toWorld({qMax(m_pressAt.x(), screen.x()), qMax(m_pressAt.y(), screen.y())});
        const QString mode = has(modifiers, Qt::ShiftModifier)   ? QStringLiteral("add")
                           : has(modifiers, Qt::AltModifier)     ? QStringLiteral("subtract")
                                                                 : QStringLiteral("replace");
        m_canvas->select(m_travelled ? m_canvas->nodesIn(QRectF(a.x(), a.y(), b.x() - a.x(), b.y() - a.y()))
                                     : QStringList(),
                         mode);
    }
    emit finished(kind);
}

void CanvasGesture::doubleClick(const QPointF &screen, const QPointF &world, int button, qreal portTolerance) {
    Q_UNUSED(screen)
    if (button != Qt::LeftButton)
        return;
    // A double click on a corner returns the node to auto-size.
    const QVariantMap corner = cornerAt(world);
    if (corner.contains(QStringLiteral("node"))) {
        m_canvas->endResize();
        m_canvas->autoSize(corner.value(QStringLiteral("node")).toString());
        return;
    }
    // On a group's port (its card's, or an exposed mark inside): rename it on the face (#72).
    const QVariantMap port = portAt(world, portTolerance);
    const bool onPort = port.contains(QStringLiteral("node"));
    if (onPort) {
        const QString node = port.value(QStringLiteral("node")).toString();
        const QString name = port.value(QStringLiteral("port")).toString();
        const bool output = port.value(QStringLiteral("output")).toBool();
        if (m_canvas->groups()->portToRename(node, name, output).value(QStringLiteral("name")).isValid()) {
            m_canvas->endLinkDrag();
            emit m_canvas->portRenameRequested(node, name, output);
            return;
        }
    }
    const QString hit = m_canvas->nodeAt(world.x(), world.y());
    const bool belowTitle = !hit.isEmpty() && world.y() > m_canvas->position(hit).y() + m_canvas->headerHeight();
    // On a node's title: rename it (#55).
    if (!hit.isEmpty() && !onPort && !belowTitle) {
        m_canvas->endMove();
        emit m_canvas->nodeRenameRequested(hit);
        return;
    }
    // A double click on a group's body enters it (5.2); its title stays for renaming.
    if (!hit.isEmpty() && m_canvas->isGroup(hit) && !onPort && belowTitle) {
        m_canvas->endMove();
        m_canvas->groups()->enterGroup(hit);
        return;
    }
    // On a Frame's title: rename it.
    const QString frame = hit.isEmpty() ? m_canvas->frameAt(world.x(), world.y(), m_frameBand) : QString();
    if (!frame.isEmpty()) {
        m_canvas->endMove();
        emit m_canvas->frameRenameRequested(frame);
        return;
    }
    // A double click on a picture opens it large (#62, #73): a Compare Images node's own viewer, a
    // Crop's crop window, a mask node's editor, any other node's large view.
    if (!hit.isEmpty() && !onPort) {
        if (m_canvas->isCompare(hit) && m_canvas->compareResult(hit).value(QStringLiteral("a")).isValid()) {
            emit viewerRequested(QStringLiteral("compare"), hit);
            return;
        }
        if (m_canvas->pictureAt(hit, world.x(), world.y())) {
            const QString kind = m_canvas->isCrop(hit)                                                ? QStringLiteral("crop")
                               : m_canvas->maskState(hit).value(QStringLiteral("over")).toBool() ? QStringLiteral("mask")
                                                                                                     : QStringLiteral("image");
            emit viewerRequested(kind, hit);
            return;
        }
    }
    if (hit.isEmpty() && !onPort)
        emit searchRequested(world);
}
