#include "canvas/mask_layer.h"

#include <QFile>
#include <QImage>
#include <QPointF>
#include <QQuickWindow>
#include <QUrl>

#include <cmath>

#include "engine/file_roots.h"
#include "engine/paths.h"
#include "workflow/image_inputs.h"

namespace fugue {

namespace {

mask::Point pointOf(const QVariant &value) {
    if (value.canConvert<QPointF>() && value.typeId() == QMetaType::QPointF)
        return {value.toPointF().x(), value.toPointF().y()};
    const QVariantMap map = value.toMap();
    return {map.value("x").toDouble(), map.value("y").toDouble()};
}

} // namespace

MaskLayer::MaskLayer(QQuickItem *parent) : QQuickItem(parent), m_paint(std::make_unique<mask::MaskPaint>(1, 1)) {
    setFlag(ItemHasContents);
}

MaskLayer::~MaskLayer() = default;

void MaskLayer::changed(bool history) {
    const mask::IRect dirty = m_paint->takeDirty();
    if (!dirty.empty()) {
        m_cells.markRect(QRect(dirty.x, dirty.y, dirty.width, dirty.height), imageSize());
        update();
    }
    emit edited();
    if (history)
        emit historyChanged();
}

void MaskLayer::markAllDirty() {
    m_cells.markAll();
    update();
}

void MaskLayer::setImageSize(const QSize &size) {
    const QSize wanted(std::max(1, size.width()), std::max(1, size.height()));
    if (wanted == imageSize())
        return;
    m_paint = std::make_unique<mask::MaskPaint>(wanted.width(), wanted.height());
    markAllDirty();
    emit imageSizeChanged();
    emit historyChanged();
    emit edited();
}

void MaskLayer::setOriginX(qreal v) {
    if (v == m_originX)
        return;
    m_originX = v;
    update();
    emit viewChanged();
}

void MaskLayer::setOriginY(qreal v) {
    if (v == m_originY)
        return;
    m_originY = v;
    update();
    emit viewChanged();
}

void MaskLayer::setZoom(qreal v) {
    if (v == m_zoom)
        return;
    m_zoom = v;
    update();
    emit viewChanged();
}

void MaskLayer::setTint(const QColor &color) {
    if (color == m_tint)
        return;
    m_tint = color;
    markAllDirty();
    emit tintChanged();
}

void MaskLayer::setMaskOnly(bool on) {
    if (on == m_maskOnly)
        return;
    m_maskOnly = on;
    markAllDirty();
    emit tintChanged();
}

QString MaskLayer::folder() const { return m_folder.isEmpty() ? inputsDir() : m_folder; }

void MaskLayer::setFolder(const QString &folder) {
    if (folder == m_folder)
        return;
    m_folder = folder;
    emit folderChanged();
}

bool MaskLayer::loadMask(const QString &path) {
    const QString local = path.startsWith(QStringLiteral("file:")) ? QUrl(path).toLocalFile() : path;
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = tr("Cannot read the mask %1.").arg(local);
        return false;
    }
    const Result<mask::MaskPaint> loaded = mask::MaskPaint::fromPng(file.read(64 << 20));
    if (!loaded) {
        m_error = loaded.error().message;
        return false;
    }
    m_paint = std::make_unique<mask::MaskPaint>(loaded->scaled(m_paint->width(), m_paint->height()));
    markAllDirty();
    emit historyChanged();
    emit edited();
    return true;
}

void MaskLayer::reset() {
    m_paint = std::make_unique<mask::MaskPaint>(m_paint->width(), m_paint->height());
    markAllDirty();
    emit historyChanged();
    emit edited();
}

void MaskLayer::beginStroke(bool erase, double size, double hardness, double x, double y, double pressure) {
    m_paint->beginStroke(erase ? mask::Mode::Erase : mask::Mode::Paint, {size, hardness, 1.0, {}});
    m_paint->strokeTo({x, y}, pressure);
    changed(false);
}

void MaskLayer::strokeTo(double x, double y, double pressure) {
    m_paint->strokeTo({x, y}, pressure);
    changed(false);
}

void MaskLayer::endStroke() {
    m_paint->endStroke();
    changed(true);
}

void MaskLayer::fillPolygon(const QVariantList &points, bool erase) {
    std::vector<mask::Point> outline;
    outline.reserve(size_t(points.size()));
    for (const QVariant &p : points)
        outline.push_back(pointOf(p));
    m_paint->fillPolygon(outline, erase ? mask::Mode::Erase : mask::Mode::Paint);
    changed(true);
}

void MaskLayer::fillRect(double x, double y, double width, double height, bool erase) {
    m_paint->fillRect(x, y, width, height, erase ? mask::Mode::Erase : mask::Mode::Paint);
    changed(true);
}

void MaskLayer::invert() {
    m_paint->invert();
    changed(true);
}

void MaskLayer::clear() {
    m_paint->clear();
    changed(true);
}

void MaskLayer::fill() {
    m_paint->fill();
    changed(true);
}

void MaskLayer::undo() {
    m_paint->undo();
    changed(true);
}

void MaskLayer::redo() {
    m_paint->redo();
    changed(true);
}

int MaskLayer::valueAt(int x, int y) const {
    return x < 0 || y < 0 || x >= m_paint->width() || y >= m_paint->height() ? -1 : m_paint->at(x, y);
}

QByteArray MaskLayer::png() const {
    const Result<QByteArray> bytes = m_paint->toPng();
    return bytes ? bytes.value() : QByteArray();
}

QString MaskLayer::store(const QString &stem) {
    const Result<QByteArray> bytes = m_paint->toPng();
    if (!bytes) {
        m_error = bytes.error().message;
        return {};
    }
    FileRoots roots = FileRoots::forInputs();
    if (!m_folder.isEmpty())
        roots.read = roots.write = {m_folder};
    const Result<QString> stored = storeImage(bytes.value(), folder(), roots, stem);
    if (!stored) {
        m_error = stored.error().message;
        return {};
    }
    return stored.value();
}

void MaskLayer::geometryChange(const QRectF &now, const QRectF &before) {
    QQuickItem::geometryChange(now, before);
    update();
}

QSGNode *MaskLayer::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    const QColor tint = m_tint;
    const int pr = tint.red() * tint.alpha() / 255, pg = tint.green() * tint.alpha() / 255, pb = tint.blue() * tint.alpha() / 255;
    return m_cells.update(old, window(), imageSize(), QRectF(0, 0, width(), height()), {m_originX, m_originY}, m_zoom,
                          [&](const QRect &cell) {
        QImage image(cell.size(), QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < cell.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < cell.width(); ++x) {
                const int v = m_paint->at(cell.x() + x, cell.y() + y);
                line[x] = m_maskOnly ? qRgb(v, v, v) : qRgba(pr * v / 255, pg * v / 255, pb * v / 255, tint.alpha() * v / 255);
            }
        }
        return image;
    });
}

} // namespace fugue
