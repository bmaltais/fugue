#include "canvas/mask_layer.h"

#include <QFile>
#include <QImage>
#include <QPointF>
#include <QQuickWindow>
#include <QUrl>

#include <algorithm>
#include <cmath>

#include "engine/file_roots.h"
#include "engine/mask_ops.h"
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

void MaskLayer::setInverted(bool on) {
    if (on == m_inverted)
        return;
    m_inverted = on;
    markAllDirty();
    emit tintChanged();
}

void MaskLayer::setBrushOpacity(qreal opacity) {
    opacity = std::clamp(opacity, 0.0, 1.0);
    if (opacity == m_brushOpacity)
        return;
    m_brushOpacity = opacity;
    emit tintChanged();
}

void MaskLayer::setPaint(const mask::MaskPaint &paint) {
    const bool resized = paint.width() != m_paint->width() || paint.height() != m_paint->height();
    m_paint = std::make_unique<mask::MaskPaint>(paint.width(), paint.height(), paint.pixels());
    markAllDirty();
    if (resized)
        emit imageSizeChanged();
    emit historyChanged();
    emit edited();
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

void MaskLayer::setClipTo(MaskLayer *selection) {
    if (selection == this)
        selection = nullptr;
    if (m_clipTo == selection)
        return;
    m_clipTo = selection;
    emit clipChanged();
}

void MaskLayer::setClipRect(const QRectF &rect) {
    if (m_clipRect == rect)
        return;
    m_clipRect = rect;
    emit clipChanged();
}

// The one place a selection becomes a clip: every paint path of the layer (brush, eraser, shapes, invert, clear, fill) calls it
// first. A painted selection with something in it wins; otherwise the rectangle; otherwise nothing limits the paint.
void MaskLayer::applyClip() {
    std::shared_ptr<const std::vector<uint8_t>> clip;
    const int w = m_paint->width(), h = m_paint->height();
    if (m_clipTo && !m_clipTo->isEmpty() && m_clipTo->paint().width() == w && m_clipTo->paint().height() == h) {
        clip = std::make_shared<const std::vector<uint8_t>>(m_clipTo->paint().pixels());
    } else if (m_clipRect.width() > 0 && m_clipRect.height() > 0) {
        mask::MaskPaint area(w, h);
        area.fillRect(m_clipRect.x(), m_clipRect.y(), m_clipRect.width(), m_clipRect.height());
        clip = std::make_shared<const std::vector<uint8_t>>(area.pixels());
    }
    m_paint->setClip(std::move(clip));
}

void MaskLayer::beginStroke(bool erase, double size, double hardness, double x, double y, double pressure) {
    applyClip();
    m_paint->beginStroke(erase ? mask::Mode::Erase : mask::Mode::Paint, {size, hardness, m_brushOpacity, {}});
    m_paint->strokeTo({x, y}, pressure);
    changed(false);
    if (!m_stroking) {
        m_stroking = true;
        emit strokingChanged();
    }
}

void MaskLayer::strokeTo(double x, double y, double pressure) {
    m_paint->strokeTo({x, y}, pressure);
    changed(false);
}

void MaskLayer::endStroke() {
    m_paint->endStroke();
    changed(true);
    if (m_stroking) {
        m_stroking = false;
        emit strokingChanged();
    }
}

void MaskLayer::fillPolygon(const QVariantList &points, bool erase) {
    std::vector<mask::Point> outline;
    outline.reserve(size_t(points.size()));
    for (const QVariant &p : points)
        outline.push_back(pointOf(p));
    applyClip();
    m_paint->fillPolygon(outline, erase ? mask::Mode::Erase : mask::Mode::Paint);
    changed(true);
}

void MaskLayer::fillRect(double x, double y, double width, double height, bool erase) {
    applyClip();
    m_paint->fillRect(x, y, width, height, erase ? mask::Mode::Erase : mask::Mode::Paint);
    changed(true);
}

void MaskLayer::invert() {
    applyClip();
    m_paint->invert();
    changed(true);
}

void MaskLayer::clear() {
    applyClip();
    m_paint->clear();
    changed(true);
}

void MaskLayer::fill() {
    applyClip();
    m_paint->fill();
    changed(true);
}

bool MaskLayer::combine(const mask::MaskPaint &shape, const QString &how) {
    const std::vector<uint8_t> &now = m_paint->pixels();
    if (shape.width() != m_paint->width() || shape.height() != m_paint->height() || shape.channels() != 1)
        return false;
    std::vector<uint8_t> next = shape.pixels();
    if (how == QLatin1String("add"))
        for (size_t i = 0; i < next.size(); ++i)
            next[i] = std::max(now[i], next[i]);
    else if (how == QLatin1String("subtract"))
        for (size_t i = 0; i < next.size(); ++i)
            next[i] = std::min(now[i], uint8_t(255 - next[i]));
    else if (how != QLatin1String("replace"))
        return false;
    if (next == now)
        return false;
    if (!m_paint->assign(next))
        return false;
    changed(true);
    return true;
}

bool MaskLayer::selectPolygon(const QVariantList &points, const QString &how) {
    std::vector<mask::Point> outline;
    outline.reserve(size_t(points.size()));
    for (const QVariant &p : points)
        outline.push_back(pointOf(p));
    if (outline.size() < 3)
        return false;
    mask::MaskPaint shape(m_paint->width(), m_paint->height());
    shape.fillPolygon(outline);
    return combine(shape, how);
}

bool MaskLayer::modify(const QString &kind, int amount) {
    const std::vector<uint8_t> &now = m_paint->pixels();
    const int w = m_paint->width(), h = m_paint->height();
    std::vector<uint8_t> next;
    if (kind == QLatin1String("grow"))
        next = mask::ops::expanded(now, w, h, amount);
    else if (kind == QLatin1String("shrink"))
        next = mask::ops::expanded(now, w, h, -amount);
    else if (kind == QLatin1String("feather"))
        next = mask::ops::feathered(now, w, h, amount);
    else if (kind == QLatin1String("border"))
        next = mask::ops::border(now, w, h, amount);
    else if (kind == QLatin1String("smooth"))
        next = mask::ops::smoothed(now, w, h, amount);
    else
        return false;
    if (next == now || !m_paint->assign(next))
        return false;
    changed(true);
    return true;
}

bool MaskLayer::transformBy(double dx, double dy, double scaleX, double scaleY, double degrees) {
    const mask::ops::Placement placement{dx, dy, scaleX, scaleY, degrees};
    const std::vector<uint8_t> &now = m_paint->pixels();
    const std::vector<uint8_t> next = mask::ops::transformed(now, m_paint->width(), m_paint->height(), placement);
    if (next == now || !m_paint->assign(next))
        return false;
    changed(true);
    return true;
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
                const int stored = m_paint->at(cell.x() + x, cell.y() + y);
                const int v = m_inverted ? 255 - stored : stored;
                line[x] = m_maskOnly ? qRgb(stored, stored, stored) : qRgba(pr * v / 255, pg * v / 255, pb * v / 255, tint.alpha() * v / 255);
            }
        }
        return image;
    });
}

} // namespace fugue
