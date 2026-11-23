#pragma once

// Marching ants (#313): the outline of the selection, an animated dashed line in black and white so it reads on any
// picture. A QQuickPaintedItem over the picture's view; it never takes the pointer. The outline is the rectangle (`rect`,
// picture pixels) and/or the contour of a mask (`mask`, a MaskLayer: the painted, lasso or layer-mask selection),
// computed at the view's resolution for the part of the picture the item shows (selection_contour.h), cached until the
// mask, the zoom or the visible part changes, and drawn in screen pixels so it follows pan and zoom. The dashes move
// about 8 px a second, redrawn eight times a second while the item is visible and has an outline; a static view
// otherwise repaints nothing.

#include <QQuickPaintedItem>
#include <QRectF>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include <vector>

#include "canvas/mask_layer.h"
#include "canvas/selection_contour.h"

namespace fugue {



class SelectionAnts : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QRectF rect READ rect WRITE setRect NOTIFY outlineChanged FINAL)
    Q_PROPERTY(fugue::MaskLayer *mask READ mask WRITE setMask NOTIFY maskChanged FINAL)
    Q_PROPERTY(qreal originX READ originX WRITE setOriginX NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal originY READ originY WRITE setOriginY NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewChanged FINAL)
    // Pixels the dashes have moved (for tests; the timer advances it).
    Q_PROPERTY(qreal phase READ phase NOTIFY phaseChanged FINAL)
    // The outline has something to draw: a rectangle or a mask with a contour in view.
    Q_PROPERTY(bool outlined READ outlined NOTIFY outlineChanged FINAL)
    // How many polylines and points the outline has (the bound a huge canvas keeps), and their bounds in picture pixels.
    Q_PROPERTY(int pathCount READ pathCount NOTIFY outlineChanged FINAL)
    Q_PROPERTY(int pointCount READ pointCount NOTIFY outlineChanged FINAL)
    Q_PROPERTY(QRectF outlineBounds READ outlineBounds NOTIFY outlineChanged FINAL)

public:
    explicit SelectionAnts(QQuickItem *parent = nullptr);

    QRectF rect() const { return m_rect; }
    void setRect(const QRectF &rect);
    MaskLayer *mask() const { return m_mask; }
    void setMask(MaskLayer *mask);
    qreal originX() const { return m_originX; }
    void setOriginX(qreal v);
    qreal originY() const { return m_originY; }
    void setOriginY(qreal v);
    qreal zoom() const { return m_zoom; }
    void setZoom(qreal v);
    qreal phase() const { return m_phase; }
    bool outlined() const { return !m_paths.empty(); }
    int pathCount() const;
    int pointCount() const;
    QRectF outlineBounds() const;
    // The polylines (picture pixels), for tests; a closed one ends on its first point.
    const std::vector<ContourPath> &paths() const { return m_paths; }
    // True when the first polyline is closed (last point = first), for QML tests.
    Q_INVOKABLE bool closed(int path) const;

    void paint(QPainter *painter) override;

signals:
    void outlineChanged();
    void maskChanged();
    void viewChanged();
    void phaseChanged();

protected:
    void geometryChange(const QRectF &now, const QRectF &before) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void invalidate();
    void rebuild();
    void sync(); // the timer runs while there is something to animate

    QRectF m_rect;
    MaskLayer *m_mask = nullptr;
    qreal m_originX = 0, m_originY = 0, m_zoom = 1;
    qreal m_phase = 0;
    bool m_dirty = false; // a rebuild is queued
    QRect m_window; // what the cached contour covers (picture pixels) and its step
    int m_cell = 0;
    std::vector<ContourPath> m_paths;
    QTimer m_timer;
};

} // namespace fugue
