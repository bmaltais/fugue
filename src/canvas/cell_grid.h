#pragma once

// The scene graph half of a painted layer (MaskLayer, PaintLayer): a picture cut into 256 px cells,
// each its own texture node, so a stroke uploads only the cells it touched, never the whole picture,
// and cells scrolled out of view wait. The owner says which cells changed (markRect, markAll) and, when
// the scene graph asks, how to draw one (`render`: the picture rectangle of a cell as an image).

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>

#include <functional>
#include <set>

class QQuickWindow;
class QSGNode;

namespace fugue {

class CellGrid {
public:
    static constexpr int cellSize = 256;

    // A picture rectangle changed: its cells are drawn again.
    void markRect(const QRect &area, const QSize &picture);
    // Everything changed (a new picture, a new look): the nodes are made again.
    void markAll();

    // Called from updatePaintNode: makes or reuses the cells' nodes, draws the visible ones that are
    // stale, and places each at `origin` + its position * `zoom` in the item (`view` is its size).
    QSGNode *update(QSGNode *old, QQuickWindow *window, const QSize &picture, const QRectF &view, QPointF origin, qreal zoom,
                    const std::function<QImage(const QRect &)> &render);

    // How many cells the last update uploaded, for tests.
    int uploaded() const { return m_uploaded; }

private:
    std::set<int> m_dirty;
    bool m_rebuild = true;
    int m_uploaded = 0;
};

} // namespace fugue
