#include "canvas/selection_ants.h"

#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

#include "canvas/mask_layer.h"

namespace fugue {

namespace {
constexpr int frameMs = 125;    // eight redraws a second
constexpr qreal speed = 8;      // dash pixels a second
constexpr qreal dash = 4;       // black and white dash length, screen pixels
} // namespace

SelectionAnts::SelectionAnts(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
    setAntialiasing(false);
    m_timer.setInterval(frameMs);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_phase = std::fmod(m_phase + speed * frameMs / 1000.0, 2 * dash);
        emit phaseChanged();
        update();
    });
}

void SelectionAnts::setRect(const QRectF &rect) {
    if (rect == m_rect)
        return;
    m_rect = rect;
    invalidate();
}

void SelectionAnts::setMask(MaskLayer *mask) {
    if (mask == m_mask)
        return;
    if (m_mask)
        disconnect(m_mask, nullptr, this, nullptr);
    m_mask = mask;
    if (m_mask) {
        connect(m_mask, &MaskLayer::edited, this, &SelectionAnts::invalidate);
        connect(m_mask, &MaskLayer::historyChanged, this, &SelectionAnts::invalidate);
        connect(m_mask, &MaskLayer::imageSizeChanged, this, &SelectionAnts::invalidate);
        connect(m_mask, &QObject::destroyed, this, [this] { m_mask = nullptr; invalidate(); });
    }
    emit maskChanged();
    invalidate();
}

void SelectionAnts::setOriginX(qreal v) {
    if (v == m_originX)
        return;
    m_originX = v;
    invalidate();
    emit viewChanged();
}

void SelectionAnts::setOriginY(qreal v) {
    if (v == m_originY)
        return;
    m_originY = v;
    invalidate();
    emit viewChanged();
}

void SelectionAnts::setZoom(qreal v) {
    if (v == m_zoom || v <= 0)
        return;
    m_zoom = v;
    invalidate();
    emit viewChanged();
}

void SelectionAnts::geometryChange(const QRectF &now, const QRectF &before) {
    QQuickPaintedItem::geometryChange(now, before);
    if (now.size() != before.size())
        invalidate();
}

void SelectionAnts::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged || change == ItemSceneChange)
        invalidate();
}

// The contour is computed lazily: once per change, when the item shows. Not here, in a setter, so a drag of the zoom
// computes once per frame.
void SelectionAnts::invalidate() {
    if (!m_dirty) {
        m_dirty = true;
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    }
}

void SelectionAnts::rebuild() {
    m_dirty = false;
    std::vector<ContourPath> paths;
    if (isVisible() && width() > 0 && height() > 0) {
        if (!m_rect.isEmpty()) {
            const QRectF r = m_rect;
            paths.push_back({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), r.topLeft()});
        }
        if (m_mask && !m_mask->isEmpty()) {
            const mask::MaskPaint &paint = m_mask->paint();
            // The part of the picture the item shows, a cell beyond each side.
            const QRectF seen((0 - m_originX) / m_zoom, (0 - m_originY) / m_zoom, width() / m_zoom, height() / m_zoom);
            m_cell = contourCell(m_zoom);
            m_window = seen.toAlignedRect().adjusted(-m_cell, -m_cell, m_cell, m_cell);
            auto contour = selectionContour(paint.width(), paint.height(),
                                            [&paint](int x, int y) { return paint.at(x, y); }, m_window, m_cell);
            for (auto &path : contour)
                paths.push_back(std::move(path));
        }
    }
    m_paths = std::move(paths);
    const bool run = isVisible() && outlined() && window();
    if (run != m_timer.isActive())
        run ? m_timer.start() : m_timer.stop();
    emit outlineChanged();
    update();
}

int SelectionAnts::pathCount() const { return int(m_paths.size()); }

int SelectionAnts::pointCount() const {
    int n = 0;
    for (const ContourPath &p : m_paths)
        n += int(p.size());
    return n;
}

QRectF SelectionAnts::outlineBounds() const {
    if (m_paths.empty())
        return {};
    double l = m_paths[0][0].x(), r = l, t = m_paths[0][0].y(), b = t;
    for (const ContourPath &path : m_paths)
        for (const QPointF &p : path) {
            l = std::min(l, p.x()); r = std::max(r, p.x());
            t = std::min(t, p.y()); b = std::max(b, p.y());
        }
    return QRectF(QPointF(l, t), QPointF(r, b));
}

bool SelectionAnts::closed(int path) const {
    return path >= 0 && path < int(m_paths.size()) && m_paths[size_t(path)].size() > 2
           && m_paths[size_t(path)].front() == m_paths[size_t(path)].back();
}

void SelectionAnts::paint(QPainter *painter) {
    if (m_paths.empty())
        return;
    painter->setRenderHint(QPainter::Antialiasing, false);
    for (const ContourPath &path : m_paths) {
        QPolygonF screen;
        screen.reserve(qsizetype(path.size()));
        for (const QPointF &p : path)
            screen << QPointF(m_originX + p.x() * m_zoom, m_originY + p.y() * m_zoom);
        QPen white(Qt::white, 1);
        white.setCosmetic(true);
        painter->setPen(white);
        painter->drawPolyline(screen);
        QPen black(Qt::black, 1);
        black.setCosmetic(true);
        black.setDashPattern({dash, dash});
        black.setDashOffset(m_phase);
        painter->setPen(black);
        painter->drawPolyline(screen);
    }
}

} // namespace fugue
