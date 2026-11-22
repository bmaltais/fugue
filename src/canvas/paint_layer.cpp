#include "canvas/paint_layer.h"

#include <QFile>
#include <QImage>
#include <QPointF>
#include <QUrl>

#include <algorithm>
#include <cmath>

#include "engine/file_roots.h"
#include "engine/paint_file.h"
#include "engine/paths.h"
#include "workflow/image_inputs.h"

namespace fugue {

namespace {

using mask::LayerKind;

mask::Point pointOf(const QVariant &value) {
    if (value.canConvert<QPointF>() && value.typeId() == QMetaType::QPointF)
        return {value.toPointF().x(), value.toPointF().y()};
    const QVariantMap map = value.toMap();
    return {map.value("x").toDouble(), map.value("y").toDouble()};
}

QString localPath(const QString &path) { return path.startsWith(QStringLiteral("file:")) ? QUrl(path).toLocalFile() : path; }

QString kindName(LayerKind kind) {
    return kind == LayerKind::Mask ? QStringLiteral("mask") : kind == LayerKind::Picture ? QStringLiteral("picture") : QStringLiteral("paint");
}

// The checkerboard under a canvas with no picture: 16 picture pixels a square, in two light grays.
int checkerAt(int x, int y) { return ((x / 16) + (y / 16)) % 2 == 0 ? 0xd2 : 0xf2; }

} // namespace

PaintLayer::PaintLayer(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    newDocument(1, 1);
}

PaintLayer::~PaintLayer() = default;

// A canvas to start from: the picture's placeholder and one paint layer to paint on at once.
void PaintLayer::newDocument(int width, int height) {
    m_document = std::make_unique<mask::PaintDocument>(width, height);
    m_document->loadLayer(LayerKind::Picture, tr("Picture"), true, 1, mask::MaskPaint(1, 1, 4));
    m_document->loadLayer(LayerKind::Paint, tr("Layer 1"), true, 1, mask::MaskPaint(1, 1, 4));
    applyPicture();
}

void PaintLayer::applyPicture() {
    if (!m_hasPicture)
        return;
    for (int i = 0; i < m_document->layerCount(); ++i)
        m_document->setPicture(m_document->layer(i).id, m_picture);
}

std::vector<int> PaintLayer::order() const {
    std::vector<int> ids;
    for (int i = 0; i < m_document->layerCount(); ++i)
        ids.push_back(m_document->layer(i).id);
    return ids;
}

void PaintLayer::changed(bool history) {
    const mask::IRect dirty = m_document->takeDirty();
    if (!dirty.empty()) {
        m_cells.markRect(QRect(dirty.x, dirty.y, dirty.width, dirty.height), imageSize());
        update();
    }
    emit edited();
    if (history)
        emit historyChanged();
}

void PaintLayer::markAllDirty() {
    m_cells.markAll();
    update();
}

// The stack or a layer's look changed: every cell shows it.
void PaintLayer::stackChanged() {
    m_document->takeDirty();
    markAllDirty();
    emit layersChanged();
    emit edited();
    emit historyChanged();
}

void PaintLayer::setImageSize(const QSize &size) {
    const QSize wanted(std::max(1, size.width()), std::max(1, size.height()));
    if (wanted == imageSize())
        return;
    newDocument(wanted.width(), wanted.height());
    markAllDirty();
    emit imageSizeChanged();
    emit layersChanged();
    emit historyChanged();
    emit edited();
}

void PaintLayer::setOriginX(qreal v) {
    if (v == m_originX)
        return;
    m_originX = v;
    update();
    emit viewChanged();
}

void PaintLayer::setOriginY(qreal v) {
    if (v == m_originY)
        return;
    m_originY = v;
    update();
    emit viewChanged();
}

void PaintLayer::setZoom(qreal v) {
    if (v == m_zoom)
        return;
    m_zoom = v;
    update();
    emit viewChanged();
}

void PaintLayer::setTint(const QColor &color) {
    if (color == m_tint)
        return;
    m_tint = color;
    markAllDirty();
    emit tintChanged();
}

void PaintLayer::setMaskOnly(bool on) {
    if (on == m_maskOnly)
        return;
    m_maskOnly = on;
    markAllDirty();
    emit tintChanged();
}

void PaintLayer::setChecker(bool on) {
    if (on == m_checker)
        return;
    m_checker = on;
    markAllDirty();
    emit tintChanged();
}

void PaintLayer::setColor(const QColor &color) {
    if (color == m_color)
        return;
    m_color = color;
    emit brushChanged();
}

void PaintLayer::setBrushOpacity(qreal v) {
    v = std::clamp(v, 0.0, 1.0);
    if (v == m_opacity)
        return;
    m_opacity = v;
    emit brushChanged();
}

bool PaintLayer::isEmpty() const {
    for (int i = 0; i < m_document->layerCount(); ++i) {
        const mask::LayerInfo layer = m_document->layer(i);
        if (layer.kind == LayerKind::Picture ? !layer.visible || layer.opacity < 1 : !m_document->pixels(layer.id)->isEmpty())
            return false;
    }
    return true;
}

QString PaintLayer::folder() const { return m_folder.isEmpty() ? inputsDir() : m_folder; }

void PaintLayer::setFolder(const QString &folder) {
    if (folder == m_folder)
        return;
    m_folder = folder;
    emit folderChanged();
}

QVariantList PaintLayer::layers() const {
    QVariantList out;
    for (int i = m_document->layerCount() - 1; i >= 0; --i) {
        const mask::LayerInfo layer = m_document->layer(i);
        out.push_back(QVariantMap{{"id", layer.id},
                                  {"name", layer.name},
                                  {"kind", kindName(layer.kind)},
                                  {"visible", layer.visible},
                                  {"opacity", layer.opacity},
                                  {"active", layer.id == m_document->activeLayer()}});
    }
    return out;
}

bool PaintLayer::loadPaint(const QString &path) {
    m_warning.clear();
    const QString local = localPath(path);
    Result<mask::PaintDocument> loaded = mask::readPaintFile(local);
    if (!loaded) {
        m_error = loaded.error().message;
        return false;
    }
    const int width = m_document->width(), height = m_document->height();
    if (loaded->width() != width || loaded->height() != height) {
        m_warning = tr("The layers were painted at %1 x %2 and are scaled to %3 x %4.")
                        .arg(loaded->width()).arg(loaded->height()).arg(width).arg(height);
        loaded = loaded->scaledTo(width, height);
    }
    m_document = std::make_unique<mask::PaintDocument>(std::move(loaded.value()));
    // A file that ends on the picture: paint on the topmost layer that takes paint.
    for (int i = m_document->layerCount() - 1; i >= 0 && !m_document->canPaint(); --i)
        m_document->setActiveLayer(m_document->layer(i).id);
    applyPicture();
    stackChanged();
    return true;
}

QSize PaintLayer::fileSize(const QString &path) const {
    const Result<mask::PaintDocument> loaded = mask::readPaintFile(localPath(path));
    return loaded ? QSize(loaded->width(), loaded->height()) : QSize();
}

bool PaintLayer::setPicture(const QString &path) {
    if (path.isEmpty()) {
        m_hasPicture = false;
        // The placeholders again (the next canvas has no picture to draw).
        for (int i = 0; i < m_document->layerCount(); ++i)
            if (m_document->layer(i).kind == LayerKind::Picture)
                m_document->setPicture(m_document->layer(i).id, mask::MaskPaint(1, 1, 4));
        stackChanged();
        return true;
    }
    QFile file(localPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = tr("Cannot read the picture %1.").arg(file.fileName());
        return false;
    }
    const Result<mask::MaskPaint> picture = mask::MaskPaint::fromPicture(file.read(qint64(256) << 20));
    if (!picture) {
        m_error = picture.error().message;
        return false;
    }
    m_picture = picture.value();
    m_hasPicture = true;
    applyPicture();
    stackChanged();
    return true;
}

void PaintLayer::reset() {
    newDocument(m_document->width(), m_document->height());
    m_warning.clear();
    stackChanged();
}

void PaintLayer::applyBrush(bool erase, double size, double hardness) {
    m_document->mode = erase ? mask::Mode::Erase : mask::Mode::Paint;
    m_document->brush = {size, hardness, m_opacity, {uint8_t(m_color.red()), uint8_t(m_color.green()), uint8_t(m_color.blue()), uint8_t(m_color.alpha())}};
}

void PaintLayer::beginStroke(bool erase, double size, double hardness, double x, double y, double pressure) {
    applyBrush(erase, size, hardness);
    if (!m_document->beginStroke())
        return; // the picture layer, or none: canPaint says so
    m_document->strokeTo({x, y}, pressure);
    changed(false);
}

void PaintLayer::strokeTo(double x, double y, double pressure) {
    if (!m_document->stroking())
        return;
    m_document->strokeTo({x, y}, pressure);
    changed(false);
}

void PaintLayer::endStroke() {
    if (!m_document->stroking())
        return;
    m_document->endStroke();
    changed(true);
}

void PaintLayer::fillPolygon(const QVariantList &points, bool erase) {
    std::vector<mask::Point> outline;
    outline.reserve(size_t(points.size()));
    for (const QVariant &p : points)
        outline.push_back(pointOf(p));
    applyBrush(erase, m_document->brush.size, m_document->brush.hardness);
    if (m_document->fillPolygon(outline))
        changed(true);
}

void PaintLayer::fillRect(double x, double y, double width, double height, bool erase) {
    applyBrush(erase, m_document->brush.size, m_document->brush.hardness);
    if (m_document->fillRect(x, y, width, height))
        changed(true);
}

void PaintLayer::invert() {
    if (m_document->invertLayer())
        changed(true);
}

void PaintLayer::clear() {
    if (m_document->clearLayer())
        changed(true);
}

void PaintLayer::fill() {
    applyBrush(false, m_document->brush.size, m_document->brush.hardness);
    if (m_document->fillLayer())
        changed(true);
}

// An undo or redo can be a stroke (the dirty cells) or a layer step (the order differs): both are a look.
void PaintLayer::undo() {
    const std::vector<int> before = order();
    const int active = m_document->activeLayer();
    m_document->undo();
    if (order() != before || m_document->activeLayer() != active)
        stackChanged();
    else
        changed(true);
}

void PaintLayer::redo() {
    const std::vector<int> before = order();
    const int active = m_document->activeLayer();
    m_document->redo();
    if (order() != before || m_document->activeLayer() != active)
        stackChanged();
    else
        changed(true);
}

int PaintLayer::addLayer(const QString &kind) {
    const int id = m_document->addLayer(kind == QLatin1String("mask") ? LayerKind::Mask : LayerKind::Paint,
                                        kind == QLatin1String("mask") ? tr("Mask %1").arg(m_document->layerCount())
                                                                      : tr("Layer %1").arg(m_document->layerCount()));
    if (id)
        stackChanged();
    return id;
}

bool PaintLayer::removeLayer(int id) {
    const int at = m_document->positionOf(id);
    if (at < 0 || m_document->layer(at).kind == LayerKind::Picture || !m_document->removeLayer(id))
        return false;
    stackChanged();
    return true;
}

bool PaintLayer::moveLayer(int id, int delta) {
    const int at = m_document->positionOf(id);
    if (at < 0 || at + delta < 0 || at + delta >= m_document->layerCount() || !m_document->moveLayer(id, at + delta))
        return false;
    stackChanged();
    return true;
}

bool PaintLayer::selectLayer(int id) {
    if (!m_document->setActiveLayer(id))
        return false;
    emit layersChanged();
    return true;
}

bool PaintLayer::setLayerVisible(int id, bool visible) {
    if (!m_document->setVisible(id, visible))
        return false;
    markAllDirty();
    emit layersChanged();
    emit edited();
    emit historyChanged();
    return true;
}

bool PaintLayer::setLayerOpacity(int id, double opacity) {
    if (!m_document->setOpacity(id, opacity))
        return false;
    markAllDirty();
    emit layersChanged();
    emit edited();
    emit historyChanged();
    return true;
}

bool PaintLayer::setLayerName(int id, const QString &name) {
    if (!m_document->setName(id, name))
        return false;
    emit layersChanged();
    return true;
}

int PaintLayer::valueAt(int x, int y) const {
    const mask::MaskPaint *pixels = m_document->pixels(m_document->activeLayer());
    if (!pixels || x < 0 || y < 0 || x >= pixels->width() || y >= pixels->height())
        return -1;
    const int at = m_document->positionOf(m_document->activeLayer());
    if (m_document->layer(at).kind == LayerKind::Picture)
        return -1;
    return pixels->channels() == 1 ? pixels->at(x, y) : pixels->rgbaAt(x, y).a;
}

QColor PaintLayer::colorAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= m_document->width() || y >= m_document->height())
        return {};
    const mask::Rgba c = m_document->flattenArea({x, y, 1, 1}).rgbaAt(0, 0);
    return QColor(c.r, c.g, c.b, c.a);
}

QByteArray PaintLayer::png() const {
    const Result<QByteArray> bytes = m_document->flattenToPng();
    return bytes ? bytes.value() : QByteArray();
}

QString PaintLayer::store(const QString &stem) {
    const Result<QByteArray> bytes = mask::paintToOra(*m_document);
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

void PaintLayer::geometryChange(const QRectF &now, const QRectF &before) {
    QQuickItem::geometryChange(now, before);
    update();
}

QSGNode *PaintLayer::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    const QColor tint = m_tint;
    const int tintR = tint.red() * tint.alpha() / 255, tintG = tint.green() * tint.alpha() / 255, tintB = tint.blue() * tint.alpha() / 255;
    return m_cells.update(old, window(), imageSize(), QRectF(0, 0, width(), height()), {m_originX, m_originY}, m_zoom,
                          [&](const QRect &cell) {
        const mask::IRect area{cell.x(), cell.y(), cell.width(), cell.height()};
        const mask::MaskPaint color = m_document->flattenArea(area);
        const mask::MaskPaint maskPart = m_document->flattenMaskArea(area);
        QImage image(cell.size(), QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < cell.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < cell.width(); ++x) {
                const int v = maskPart.at(x, y);
                if (m_maskOnly) {
                    line[x] = qRgb(v, v, v);
                    continue;
                }
                const mask::Rgba c = color.rgbaAt(x, y);
                // The paint over the checkerboard (or nothing), premultiplied.
                const int back = m_checker ? checkerAt(cell.x() + x, cell.y() + y) : 0;
                const int backAlpha = m_checker ? 255 : 0;
                int a = c.a + backAlpha * (255 - c.a) / 255;
                int r = c.r * c.a / 255 + back * (255 - c.a) / 255;
                int g = c.g * c.a / 255 + back * (255 - c.a) / 255;
                int b = c.b * c.a / 255 + back * (255 - c.a) / 255;
                if (v > 0) { // the mask layers, tinted, over it
                    const int ta = tint.alpha() * v / 255;
                    r = tintR * v / 255 + r * (255 - ta) / 255;
                    g = tintG * v / 255 + g * (255 - ta) / 255;
                    b = tintB * v / 255 + b * (255 - ta) / 255;
                    a = ta + a * (255 - ta) / 255;
                }
                line[x] = qRgba(r, g, b, a);
            }
        }
        return image;
    });
}

} // namespace fugue
