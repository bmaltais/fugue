#include "canvas/cell_grid.h"

#include <QQuickWindow>
#include <QSGNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <algorithm>

namespace fugue {

namespace {

// One cell of the layer: its texture node.
class CellNode : public QSGSimpleTextureNode {
public:
    CellNode() { setOwnsTexture(true); }
};

class GridNode : public QSGNode {
public:
    int across = 0;
    int down = 0;
};

} // namespace

void CellGrid::markRect(const QRect &area, const QSize &picture) {
    if (area.isEmpty())
        return;
    const int across = (picture.width() + cellSize - 1) / cellSize;
    for (int cy = area.top() / cellSize; cy <= area.bottom() / cellSize; ++cy)
        for (int cx = area.left() / cellSize; cx <= area.right() / cellSize; ++cx)
            m_dirty.insert(cy * across + cx);
}

void CellGrid::markAll() {
    m_rebuild = true;
    m_dirty.clear();
}

QSGNode *CellGrid::update(QSGNode *old, QQuickWindow *window, const QSize &picture, const QRectF &view, QPointF origin, qreal zoom,
                          const std::function<QImage(const QRect &)> &render) {
    auto *root = static_cast<GridNode *>(old);
    const int across = (picture.width() + cellSize - 1) / cellSize, down = (picture.height() + cellSize - 1) / cellSize;
    if (root && (root->across != across || root->down != down || m_rebuild)) {
        delete root;
        root = nullptr;
    }
    if (!root) {
        root = new GridNode;
        root->across = across;
        root->down = down;
        for (int i = 0; i < across * down; ++i)
            root->appendChildNode(new CellNode);
        m_dirty.clear();
        for (int i = 0; i < across * down; ++i)
            m_dirty.insert(i);
        m_rebuild = false;
    }
    m_uploaded = 0;
    const int width = picture.width(), height = picture.height();
    int index = 0;
    for (QSGNode *child = root->firstChild(); child; child = child->nextSibling(), ++index) {
        auto *node = static_cast<CellNode *>(child);
        const int x = (index % across) * cellSize, y = (index / across) * cellSize;
        const QRect cell(x, y, std::min(cellSize, width - x), std::min(cellSize, height - y));
        const QRectF where(origin.x() + cell.x() * zoom, origin.y() + cell.y() * zoom, cell.width() * zoom, cell.height() * zoom);
        const bool visible = where.intersects(view);
        const bool stale = m_dirty.count(index) > 0 || !node->texture();
        // The cells in view get a texture when they have none or are dirty: cells scrolled away wait.
        if (visible && stale && window) {
            node->setTexture(window->createTextureFromImage(render(cell), QQuickWindow::TextureHasAlphaChannel));
            node->setOwnsTexture(true);
            m_dirty.erase(index);
            ++m_uploaded;
        }
        // Pixels as they are once enlarged; smooth when it is shown smaller than it is.
        node->setFiltering(zoom >= 1 ? QSGTexture::Nearest : QSGTexture::Linear);
        node->setRect(visible && node->texture() ? where : QRectF());
    }
    return root;
}

} // namespace fugue
