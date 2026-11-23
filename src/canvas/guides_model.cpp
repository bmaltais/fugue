#include "canvas/guides_model.h"

#include <algorithm>

#include "engine/canvas_store.h"

namespace fugue {

namespace {

QVariantMap mapOf(const crop::Rect &r) { return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}}; }
crop::Rect rectOf(const QVariantMap &map) {
    return {map.value("x").toInt(), map.value("y").toInt(), map.value("width").toInt(), map.value("height").toInt()};
}

QString textOf(const Errors &errors) {
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error)
            return error.message;
    return {};
}

} // namespace

void GuidesModel::setStoreRoot(const QString &root) {
    if (root == m_root)
        return;
    m_root = root;
    emit sourceChanged();
    load();
}

void GuidesModel::setCanvasId(const QString &id) {
    if (id == m_canvas)
        return;
    m_canvas = id;
    emit sourceChanged();
    load();
}

void GuidesModel::setCanvasSize(const QSize &size) {
    if (size == m_size)
        return;
    m_size = size;
    emit guidesChanged();
}

void GuidesModel::setSnapDistance(double pixels) {
    if (pixels == m_distance || pixels < 0)
        return;
    m_distance = pixels;
    emit guidesChanged();
}

QVariantList GuidesModel::guides() const {
    std::vector<canvas::Guide> sorted = m_set.guides;
    std::sort(sorted.begin(), sorted.end(), [](const canvas::Guide &a, const canvas::Guide &b) { return a.vertical != b.vertical ? a.vertical : a.at < b.at; });
    QVariantList rows;
    for (const canvas::Guide &guide : sorted)
        rows.append(QVariantMap{{"axis", guide.vertical ? QStringLiteral("x") : QStringLiteral("y")}, {"at", guide.at}});
    return rows;
}

void GuidesModel::load() {
    canvas::GuideSet set;
    QString problem;
    if (!m_canvas.isEmpty()) {
        const canvas::CanvasStore store = m_root.isEmpty() ? canvas::CanvasStore::forSession() : canvas::CanvasStore(m_root);
        const Result<canvas::GuideSet> read = store.guides(m_canvas);
        if (read)
            set = read.value();
        else
            problem = textOf(read.errors());
    }
    // Held in the order guides() lists them, so an index means the same thing here and in the view.
    std::sort(set.guides.begin(), set.guides.end(), [](const canvas::Guide &a, const canvas::Guide &b) { return a.vertical != b.vertical ? a.vertical : a.at < b.at; });
    m_set = set;
    m_problem = problem;
    emit guidesChanged();
}

bool GuidesModel::save(const canvas::GuideSet &next) {
    if (m_canvas.isEmpty())
        return false;
    canvas::GuideSet sorted = next;
    std::sort(sorted.guides.begin(), sorted.guides.end(), [](const canvas::Guide &a, const canvas::Guide &b) { return a.vertical != b.vertical ? a.vertical : a.at < b.at; });
    sorted.guides.erase(std::unique(sorted.guides.begin(), sorted.guides.end()), sorted.guides.end());
    canvas::CanvasStore store = m_root.isEmpty() ? canvas::CanvasStore::forSession() : canvas::CanvasStore(m_root);
    const Errors written = store.setGuides(m_canvas, sorted);
    if (hasErrors(written)) {
        m_problem = textOf(written);
        emit guidesChanged();
        return false;
    }
    m_set = sorted;
    m_problem.clear();
    emit guidesChanged();
    return true;
}

bool GuidesModel::onCanvas(bool vertical, int at) const {
    if (!m_size.isValid())
        return false;
    return at >= 0 && at <= (vertical ? m_size.width() : m_size.height());
}

bool GuidesModel::addGuide(const QString &axis, int at) {
    if (axis != QLatin1String("x") && axis != QLatin1String("y"))
        return false;
    const canvas::Guide guide{axis == QLatin1String("x"), at};
    if (!onCanvas(guide.vertical, at) || std::find(m_set.guides.begin(), m_set.guides.end(), guide) != m_set.guides.end() || int(m_set.guides.size()) >= canvas::maxGuides)
        return false;
    canvas::GuideSet next = m_set;
    next.guides.push_back(guide);
    return save(next);
}

bool GuidesModel::moveGuide(int index, int at) {
    if (index < 0 || index >= int(m_set.guides.size()))
        return false;
    canvas::GuideSet next = m_set;
    canvas::Guide &guide = next.guides[size_t(index)];
    if (!onCanvas(guide.vertical, at) || guide.at == at)
        return false;
    guide.at = at;
    return save(next);
}

bool GuidesModel::removeGuide(int index) {
    if (index < 0 || index >= int(m_set.guides.size()))
        return false;
    canvas::GuideSet next = m_set;
    next.guides.erase(next.guides.begin() + index);
    return save(next);
}

bool GuidesModel::clearGuides() {
    if (m_set.guides.empty())
        return false;
    canvas::GuideSet next = m_set;
    next.guides.clear();
    return save(next);
}

bool GuidesModel::setGridSpacing(int spacing) {
    const int kept = std::clamp(spacing, canvas::minGridSpacing, canvas::maxGridSpacing);
    if (kept == m_set.grid)
        return false;
    canvas::GuideSet next = m_set;
    next.grid = kept;
    return save(next);
}

snap::Targets GuidesModel::targets(const QVariantMap &options) const {
    snap::Targets out;
    out.multiple = options.value("multiple").toInt();
    const double zoom = options.value("zoom", 1).toDouble();
    const bool guideLines = options.value("guides").toBool(), gridLines = options.value("grid").toBool();
    if (guideLines || gridLines)
        out.threshold = zoom > 0 ? m_distance / zoom : 0;
    if (guideLines) {
        for (const canvas::Guide &guide : m_set.guides)
            (guide.vertical ? out.xs : out.ys).push_back(guide.at);
    }
    if (gridLines)
        out.gridX = out.gridY = m_set.grid;
    return out;
}

QVariantMap GuidesModel::snapRect(const QVariantMap &rect, bool keepSize, const QVariantMap &options) const {
    if (!m_size.isValid())
        return rect;
    return mapOf(snap::snapRect(rectOf(rect), targets(options), {m_size.width(), m_size.height()}, keepSize));
}

QVariantMap GuidesModel::snapFloating(const QVariantMap &rect, bool keepSize, const QVariantMap &options) const {
    const crop::RectF snapped = snap::snapFloating({rect.value("x").toDouble(), rect.value("y").toDouble(), rect.value("width").toDouble(), rect.value("height").toDouble()},
                                                   targets(options), keepSize);
    return {{"x", snapped.x}, {"y", snapped.y}, {"width", snapped.width}, {"height", snapped.height}};
}

QVariantMap GuidesModel::snapPoint(const QVariantMap &point, const QVariantMap &options) const {
    const crop::Point snapped = snap::snapPoint({point.value("x").toDouble(), point.value("y").toDouble()}, targets(options));
    return {{"x", snapped.x}, {"y", snapped.y}};
}

} // namespace fugue
