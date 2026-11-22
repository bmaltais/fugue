#include "canvas/canvas_view_model.h"

#include <QDir>
#include <QPointer>
#include <QUndoStack>

#include "canvas/region_pick.h"

namespace fugue {

namespace {

QVariantMap choice(const QVariantMap &row) {
    QVariantMap out = row;
    out.insert("label", row.value("kind").toString() == QLatin1String("head") ? QObject::tr("Head") : row.value("short").toString());
    return out;
}

} // namespace

CanvasViewModel::CanvasViewModel(QObject *parent) : CanvasHistory(parent) {
    // A burst of file events (a revision is several files) is one refresh.
    m_settle.setSingleShot(true);
    m_settle.setInterval(120);
    connect(&m_settle, &QTimer::timeout, this, &CanvasViewModel::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_settle.start(); });
    connect(this, &CanvasHistory::revisionsChanged, this, [this] { rebuild(); });
    connect(this, &CanvasHistory::compareChanged, this, [this] {
        emit pictureChanged();
        updateRegion();
        // Looking at anything else than the result shown by itself (or it, once decided) ends its banner.
        if (!m_newResult.isEmpty() && (revisionB() != m_newResult || kindB() != QLatin1String("candidate")))
            clearNewResult();
    });
}

void CanvasViewModel::watch() {
    // The store's canvases folder, and the canvas's own with its commits and resolved folders: a new candidate is a file
    // in commits, a head moved is a rename in the canvas folder.
    const QString base = store().root() + QStringLiteral("/canvases");
    QStringList want;
    if (QDir(base).exists()) {
        want << base;
        if (!canvasId().isEmpty())
            for (const QString &sub : {QString(), QStringLiteral("/commits"), QStringLiteral("/resolved")})
                if (QDir(base + QLatin1Char('/') + canvasId() + sub).exists())
                    want << base + QLatin1Char('/') + canvasId() + sub;
    } else {
        QDir above(store().root());
        while (!above.exists() && above.cdUp()) {
        }
        if (above.exists())
            want << above.path();
    }
    const QStringList have = m_watcher.directories();
    for (const QString &path : have)
        if (!want.contains(path))
            m_watcher.removePath(path);
    for (const QString &path : std::as_const(want))
        if (!have.contains(path))
            m_watcher.addPath(path);
}

void CanvasViewModel::refresh() {
    const bool onHead = !m_head.isEmpty() && revisionB() == m_head;
    reload();
    if (onHead && !m_head.isEmpty() && revisionB() != m_head)
        select(m_head); // a view on the head goes on to the new one
    watch();
    emit storeChanged(); // the canvas list too
    showArrivals();
    // A read that failed (a run is writing, a slow disk) is tried again a few times before it is left to the person.
    if (!error().isEmpty() && !canvasId().isEmpty() && m_retries < 3) {
        ++m_retries;
        m_settle.start(400);
    } else if (error().isEmpty()) {
        m_retries = 0;
    }
}

QString CanvasViewModel::newResultFrom() const {
    for (const QVariant &row : revisions())
        if (row.toMap().value("id").toString() == m_newResult) {
            QString producer = row.toMap().value("producer").toString();
            if (producer.startsWith(QLatin1String("canvas-write:")))
                producer = producer.mid(13);
            return producer;
        }
    return {};
}

void CanvasViewModel::clearNewResult() {
    if (m_newResult.isEmpty() && m_newOthers == 0)
        return;
    m_newResult.clear();
    m_newOthers = 0;
    emit newResultChanged();
}

void CanvasViewModel::setShowNewResults(bool on) {
    if (m_showNew == on)
        return;
    m_showNew = on;
    if (!on)
        m_arrived.clear();
    emit newResultChanged();
}

void CanvasViewModel::setViewLocked(bool locked) {
    if (m_locked == locked)
        return;
    m_locked = locked;
    emit newResultChanged();
    if (!locked)
        showArrivals();
}

// Moves the view to the newest candidate that arrived, when that takes nothing from the person (see the header).
void CanvasViewModel::showArrivals() {
    if (m_arrived.isEmpty() || m_locked || !m_showNew)
        return;
    const bool onHead = !m_head.isEmpty() && revisionB() == m_head;
    const bool onOwn = !m_newResult.isEmpty() && revisionB() == m_newResult;
    if (!onHead && !onOwn) {
        m_arrived.clear(); // looking at something the person chose: the strip lists the new one, nothing more
        return;
    }
    const QString newest = m_arrived.first();
    const int others = m_arrived.size() - 1 + (onOwn ? m_newOthers + 1 : 0);
    m_arrived.clear();
    show(newest); // ends the banner of an earlier result (compareChanged), the new one starts below
    if (revisionB() != newest)
        return;
    m_newResult = newest;
    m_newOthers = others;
    emit newResultChanged();
}

void CanvasViewModel::rebuild() {
    watch();
    // The candidates that were not here at the last read are the arrivals; a canvas read for the first time has none.
    QStringList open;
    for (const QVariant &value : revisions())
        if (value.toMap().value("kind").toString() == QLatin1String("candidate"))
            open << value.toMap().value("id").toString();
    if (m_knownCanvas != canvasId()) {
        m_knownCanvas = canvasId();
        m_arrived.clear();
        m_known = open;
        clearNewResult();
    } else if (error().isEmpty()) { // a failed read lists nothing: it says nothing about what arrived
        QStringList fresh;
        for (const QString &id : std::as_const(open))
            if (!m_known.contains(id))
                fresh << id;
        if (!fresh.isEmpty()) {
            fresh << m_arrived;
            m_arrived = fresh; // newest first, as the rows are
        }
        m_known = open;
        m_arrived.removeIf([&open](const QString &id) { return !open.contains(id); }); // resolved before it was shown
    }
    m_choices.clear();
    m_earlier.clear();
    int recent = 0;
    for (const QVariant &value : revisions()) {
        const QVariantMap row = value.toMap();
        const QString kind = row.value("kind").toString();
        if (kind == QLatin1String("dropped"))
            continue; // a dropped candidate is still reachable from the history dialog
        if (kind == QLatin1String("history") && ++recent > recentCount)
            m_earlier.append(choice(row));
        else
            m_choices.append(choice(row));
    }
    // The head first, then the candidates, then the latest history (the order of the rows, with the head moved up).
    std::stable_sort(m_choices.begin(), m_choices.end(), [](const QVariant &a, const QVariant &b) {
        return (a.toMap().value("kind").toString() == QLatin1String("head")) && (b.toMap().value("kind").toString() != QLatin1String("head"));
    });
    m_headSource = QUrl();
    m_headSize = QSize();
    if (!m_head.isEmpty())
        picture(m_head, &m_headSource, &m_headSize);
    emit choicesChanged();
    emit pictureChanged();
}

void CanvasViewModel::setEdits(WidgetEdit *edits) {
    if (m_edits == edits)
        return;
    QObject::disconnect(m_undoLink);
    m_edits = edits;
    if (m_edits && m_edits->undoStack())
        m_undoLink = connect(m_edits->undoStack(), &QUndoStack::indexChanged, this, [this] { updateRegion(); });
    emit followChanged();
    setFollowNode(m_follow);
}

void CanvasViewModel::setFollowNode(const QString &node) {
    const bool same = node == m_follow;
    m_follow = node;
    if (!same)
        emit followChanged();
    // The view goes to the node's canvas.
    if (m_edits && !m_follow.isEmpty()) {
        const QVariantMap state = m_edits->regionState(m_follow);
        if (!state.isEmpty() && !state.value("canvasLinked").toBool() && !state.value("canvas").toString().isEmpty()) {
            if (!state.value("storeRoot").toString().isEmpty() && state.value("storeRoot").toString() != storeRoot())
                setStoreRoot(state.value("storeRoot").toString());
            setCanvasId(state.value("canvas").toString());
        }
    }
    updateRegion();
}

void CanvasViewModel::setOverlay(bool on) {
    if (m_overlay == on)
        return;
    m_overlay = on;
    emit pictureChanged();
    updateRegion();
}

bool CanvasViewModel::showsCandidate() const {
    const QString kind = kindB();
    return kind == QLatin1String("candidate") || kind == QLatin1String("dropped");
}

QUrl CanvasViewModel::pictureSource() const { return showsCandidate() && !m_overlay && !m_headSource.isEmpty() ? m_headSource : sourceB(); }

QSize CanvasViewModel::pictureSize() const { return showsCandidate() && !m_overlay && m_headSize.isValid() ? m_headSize : sizeB(); }

void CanvasViewModel::show(const QString &revision) {
    for (const QVariant &row : revisions())
        if (row.toMap().value("id").toString() == revision) {
            const QString kind = row.toMap().value("kind").toString();
            if ((kind == QLatin1String("candidate") || kind == QLatin1String("dropped")) && !m_head.isEmpty()) {
                compare(m_head, revision);
                return;
            }
            break;
        }
    select(revision);
}

void CanvasViewModel::updateRegion() {
    QVariantMap out{{"valid", false}, {"reason", QString()}};
    if (m_edits && !m_follow.isEmpty()) {
        const QVariantMap state = m_edits->regionState(m_follow);
        const QSize size = pictureSize();
        if (state.isEmpty())
            out.insert("reason", tr("The selected node is not a Canvas Region."));
        else if (state.value("canvasLinked").toBool())
            out.insert("reason", tr("This region's canvas comes from another node."));
        else if (state.value("canvas").toString() != canvasId())
            out.insert("reason", tr("This region reads another canvas."));
        else if (!size.isValid())
            out.insert("reason", tr("There is no picture to draw the region on."));
        else {
            const crop::Size canvas{size.width(), size.height()};
            const crop::Rect rect = region::initial(state.value("x").toInt(), state.value("y").toInt(), state.value("width").toInt(),
                                                    state.value("height").toInt(), canvas);
            const crop::Rect context = region::context(rect, state.value("padding").toInt(), canvas);
            const QString reads = state.value("revision").toString();
            const Result<QString> resolved = store().resolve(canvasId(), reads);
            const QString readsId = resolved ? resolved.value() : QString();
            // "head" is read as the head; a pinned revision as the one it names.
            const bool shownIsHead = !m_head.isEmpty() && revisionB() == m_head;
            const bool readsShown = shownIsHead ? reads == QLatin1String("head") : (!readsId.isEmpty() && readsId == revisionB());
            out = QVariantMap{{"valid", true},
                              {"reason", QString()},
                              {"x", rect.x},
                              {"y", rect.y},
                              {"width", rect.width},
                              {"height", rect.height},
                              {"context", QVariantMap{{"x", context.x}, {"y", context.y}, {"width", context.width}, {"height", context.height}}},
                              {"reads", reads},
                              {"readsId", readsId},
                              {"readsShown", readsShown},
                              {"writable", !state.value("revisionLinked").toBool()},
                              {"editable", state.value("editable").toBool()},
                              {"padding", state.value("padding").toInt()},
                              {"scale", state.value("scale").toDouble()},
                              {"cap", state.value("cap").toDouble()},
                              {"multiple", state.value("multiple").toInt()}};
        }
    }
    if (out != m_region) {
        m_region = out;
        emit regionChanged();
    }
}

namespace {
crop::Rect rectOf(const QVariantMap &map) {
    return {map.value("x").toInt(), map.value("y").toInt(), map.value("width").toInt(), map.value("height").toInt()};
}
QVariantMap mapOf(const crop::Rect &r) { return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}}; }
} // namespace

QVariantMap CanvasViewModel::snapRegion(const QVariantMap &rect, bool keepSize) const {
    const QSize size = pictureSize();
    if (!m_region.value("valid").toBool() || !size.isValid())
        return rect;
    return mapOf(region::snapped(rectOf(rect), m_region.value("multiple").toInt(), {size.width(), size.height()}, keepSize));
}

QVariantMap CanvasViewModel::previewRegion(const QVariantMap &rect) const {
    const QSize size = pictureSize();
    if (!m_region.value("valid").toBool() || !size.isValid())
        return {};
    const crop::Size canvas{size.width(), size.height()};
    const crop::Rect r = rectOf(rect);
    const region::Settings settings{m_region.value("padding").toInt(), m_region.value("scale").toDouble(), m_region.value("cap").toDouble(),
                                    m_region.value("multiple").toInt()};
    const crop::Rect context = region::context(r, settings.padding, canvas);
    const crop::Size model = region::modelSize(r, settings, canvas);
    QVariantMap out = mapOf(r);
    out.insert("context", mapOf(context));
    out.insert("modelWidth", model.width);
    out.insert("modelHeight", model.height);
    out.insert("capped", region::capped(r, settings, canvas));
    return out;
}

bool CanvasViewModel::setRegion(const QVariantMap &rect, bool snap) {
    if (!m_edits || m_follow.isEmpty() || !m_region.value("valid").toBool() || !m_region.value("editable").toBool())
        return false;
    const crop::Rect r = rectOf(snap ? snapRegion(rect) : rect);
    const bool written = m_edits->setRegion(m_follow, r.x, r.y, r.width, r.height);
    if (written)
        updateRegion();
    return written;
}

bool CanvasViewModel::useForRegion() {
    if (!m_edits || m_follow.isEmpty() || revisionB().isEmpty() || !m_region.value("valid").toBool() || !m_region.value("writable").toBool()
        || m_region.value("readsShown").toBool())
        return false;
    const QString wanted = revisionB() == m_head ? QStringLiteral("head") : revisionB();
    const QString problem = m_edits->setText(m_follow, QStringLiteral("revision"), wanted);
    if (!problem.isEmpty()) {
        fail({makeError(QStringLiteral("region_revision"), problem)});
        return false;
    }
    updateRegion();
    return true;
}

} // namespace fugue
