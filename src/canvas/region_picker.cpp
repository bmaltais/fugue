#include "canvas/region_picker.h"

#include <QImage>

#include "canvas/region_pick.h"

namespace fugue {

namespace {

crop::Rect rectOf(const QVariantMap &map) {
    return {map.value("x").toInt(), map.value("y").toInt(), map.value("width").toInt(), map.value("height").toInt()};
}

QVariantMap mapOf(const crop::Rect &r) {
    return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}};
}

} // namespace

canvas::CanvasStore RegionPicker::store() const { return m_root.isEmpty() ? canvas::CanvasStore::forSession() : canvas::CanvasStore(m_root); }

namespace {
QString sentence(const Errors &errors) {
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error)
            return error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint;
    return {};
}
} // namespace

bool RegionPicker::fail(const QString &message) {
    m_resolved.clear();
    m_source.clear();
    m_size = QSize();
    m_error = message;
    emit loaded();
    return false;
}

bool RegionPicker::load() {
    m_error.clear();
    const canvas::CanvasStore s = store();
    const Result<QString> found = s.resolve(m_canvas, m_revision);
    if (!found) {
        return fail(sentence(found.errors()));
    }
    const Result<mask::PaintDocument> document = s.load(m_canvas, found.value());
    if (!document) {
        return fail(sentence(document.errors()));
    }
    const mask::MaskPaint flat = document->flatten();
    const QImage whole(flat.pixels().data(), flat.width(), flat.height(), flat.width() * 4, QImage::Format_RGBA8888);
    const bool shrink = qMax(flat.width(), flat.height()) > maxSide;
    const QImage drawn = shrink ? whole.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation) : whole;
    const QString path = m_folder.filePath(found.value().mid(QStringLiteral("sha256:").size()) + QStringLiteral(".png"));
    if (!m_folder.isValid() || !drawn.save(path, "PNG"))
        return fail(tr("The picture of the canvas could not be written."));
    m_resolved = found.value();
    m_size = QSize(flat.width(), flat.height());
    m_source = QUrl::fromLocalFile(path);
    emit loaded();
    return true;
}

QVariantMap RegionPicker::initial(int x, int y, int width, int height) const {
    return mapOf(region::initial(x, y, width, height, {m_size.width(), m_size.height()}));
}

QVariantMap RegionPicker::snapped(const QVariantMap &rect, bool keepSize) const {
    return mapOf(region::snapped(rectOf(rect), m_multiple, {m_size.width(), m_size.height()}, keepSize));
}

QVariantMap RegionPicker::context(const QVariantMap &rect) const {
    return mapOf(region::context(rectOf(rect), m_padding, {m_size.width(), m_size.height()}));
}

QVariantMap RegionPicker::modelSize(const QVariantMap &rect) const {
    const region::Settings settings{m_padding, m_scale, m_cap, m_multiple};
    const crop::Size canvas{m_size.width(), m_size.height()};
    const crop::Size size = region::modelSize(rectOf(rect), settings, canvas);
    return {{"width", size.width}, {"height", size.height}, {"capped", region::capped(rectOf(rect), settings, canvas)}};
}

} // namespace fugue
