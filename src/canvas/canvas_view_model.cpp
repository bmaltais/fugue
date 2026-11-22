#include "canvas/canvas_view_model.h"

#include <QDir>
#include <QFile>
#include <QPointer>
#include <QUndoStack>

#include <algorithm>

#include "canvas/region_pick.h"

namespace fugue {

namespace {

// A chip of the strip: the row, its `label` the row's `name` (CanvasHistory names every revision in one place).
QVariantMap choice(const QVariantMap &row) {
    QVariantMap out = row;
    out.insert("label", row.value("name"));
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
        rebuildLayers();
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
            // A person reads the node's title (#228), never its id: the Canvas Write of the open document that made it,
            // and nothing when the document has no such node (a run of another workflow, the command line).
            const QString producer = row.toMap().value("producer").toString();
            if (!producer.startsWith(QLatin1String("canvas-write:")) || !m_edits)
                return {};
            return m_edits->nodeName(producer.mid(13));
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

QUrl CanvasViewModel::pictureSource() const {
    if (m_solo >= 0 && !m_soloSource.isEmpty())
        return m_soloSource;
    return showsCandidate() && !m_overlay && !m_headSource.isEmpty() ? m_headSource : sourceB();
}

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

// ---- the layers panel (#226) --------------------------------------------------------------------------------------

namespace fugue {

namespace {

constexpr int kThumbSide = 64;

QString hexOf(const QString &revision) { return revision.mid(QStringLiteral("sha256:").size()); }

// A small copy of a layer's pixels (or its mask) as a PNG, its longest side kThumbSide.
bool writeThumbnail(const mask::MaskPaint &pixels, const QString &path) {
    const double k = double(kThumbSide) / std::max(pixels.width(), pixels.height());
    const int w = std::max(1, int(std::lround(pixels.width() * std::min(1.0, k)))), h = std::max(1, int(std::lround(pixels.height() * std::min(1.0, k))));
    const Result<QByteArray> png = pixels.scaled(w, h).toPng(1);
    if (!png)
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(png.value()) == png->size();
}

QString fromText(const QString &producer) {
    if (producer.startsWith(QLatin1String("canvas-write:")))
        return QObject::tr("Canvas Write %1").arg(producer.mid(13));
    if (producer.startsWith(QLatin1String("canvas-view:")) || producer.startsWith(QLatin1String("canvas-cli:")))
        return QObject::tr("an edit");
    return producer;
}

} // namespace

QVariantList CanvasViewModel::blendModes() {
    using mask::Blend;
    const std::pair<Blend, QString> rows[] = {{Blend::Normal, tr("Normal")},     {Blend::Multiply, tr("Multiply")}, {Blend::Screen, tr("Screen")},
                                              {Blend::Overlay, tr("Overlay")},   {Blend::SoftLight, tr("Soft light")}, {Blend::Darken, tr("Darken")},
                                              {Blend::Lighten, tr("Lighten")},   {Blend::Add, tr("Add")}};
    QVariantList out;
    for (const auto &[blend, label] : rows)
        out.append(QVariantMap{{"id", mask::blendId(blend)}, {"label", label}});
    return out;
}

QVariantList CanvasViewModel::selection() const {
    QVariantList out;
    for (int p : m_selected)
        out.append(p);
    return out;
}

bool CanvasViewModel::layersEditable() const { return !revisionB().isEmpty() && revisionB() == m_head && kindB() == QLatin1String("head"); }

QList<int> CanvasViewModel::positionsOf(const QVariantList &list) const {
    QList<int> out;
    for (const QVariant &v : list)
        if (!out.contains(v.toInt()))
            out.append(v.toInt());
    return out;
}

void CanvasViewModel::rebuildLayers() {
    const QString shown = revisionB();
    const bool sameRevision = shown == m_layersOf;
    m_layers.clear();
    m_canUndo = m_canRedo = false;
    if (!sameRevision) {
        m_solo = -1;
        m_soloSource = QUrl();
    }
    const canvas::CanvasStore s = store();
    int count = 0;
    if (!shown.isEmpty()) {
        const Result<canvas::Revision> revision = s.revision(canvasId(), shown);
        const Result<canvas::Tree> tree = revision ? s.tree(revision->tree) : Result<canvas::Tree>(revision.errors());
        if (tree) {
            count = int(tree->layers.size());
            // The thumbnails, made once per revision (a revision never changes).
            const auto thumb = [&](int position, bool ofMask) { return m_folder.filePath(QStringLiteral("t-%1-%2%3.png").arg(hexOf(shown)).arg(position).arg(ofMask ? "m" : "")); };
            bool missing = false;
            for (int i = 0; i < count && !missing; ++i)
                missing = (tree->layers[size_t(i)].kind != mask::LayerKind::Picture && !QFile::exists(thumb(i, false))) ||
                          (tree->layers[size_t(i)].hasMask && !QFile::exists(thumb(i, true)));
            if (missing) {
                const Result<mask::PaintDocument> document = s.load(canvasId(), shown);
                if (document)
                    for (int i = 0; i < document->layerCount(); ++i) {
                        const mask::LayerInfo info = document->layer(i);
                        if (const mask::MaskPaint *pixels = info.kind != mask::LayerKind::Picture ? document->pixels(info.id) : nullptr)
                            writeThumbnail(*pixels, thumb(i, false));
                        if (const mask::MaskPaint *maskPixels = document->maskPixels(info.id))
                            writeThumbnail(*maskPixels, thumb(i, true));
                    }
            }
            for (int i = count - 1; i >= 0; --i) { // top first
                const canvas::LayerRecord &layer = tree->layers[size_t(i)];
                bool known = true;
                const mask::Blend blend = mask::blendOfOp(layer.compositeOp, &known);
                const QString kind = layer.kind == mask::LayerKind::Mask ? QStringLiteral("mask") : layer.kind == mask::LayerKind::Picture ? QStringLiteral("picture") : QStringLiteral("paint");
                m_layers.append(QVariantMap{{"position", i},
                                            {"name", layer.name},
                                            {"kind", kind},
                                            {"visible", layer.visible},
                                            {"opacity", layer.opacity},
                                            {"blend", known ? mask::blendId(blend) : QString()},
                                            {"compositeOp", layer.compositeOp.isEmpty() ? QStringLiteral("svg:src-over") : layer.compositeOp},
                                            {"hasMask", layer.hasMask},
                                            {"maskEnabled", layer.maskEnabled},
                                            {"producer", layer.producer},
                                            {"from", fromText(layer.producer)},
                                            {"created", layer.created},
                                            {"depth", 0},
                                            {"thumbnail", QFile::exists(thumb(i, false)) ? QUrl::fromLocalFile(thumb(i, false)) : QUrl()},
                                            {"maskThumbnail", layer.hasMask && QFile::exists(thumb(i, true)) ? QUrl::fromLocalFile(thumb(i, true)) : QUrl()}});
            }
            if (layersEditable()) {
                m_canUndo = !revision->parent.isEmpty();
                const Result<QString> target = s.redoTarget(canvasId());
                m_canRedo = target && !target->isEmpty();
            }
        }
    }
    // The selection: what an edit chose, else kept on the same revision, else kept where it still fits.
    if (m_editing)
        m_selected = m_keepSelection;
    else if (canvasId() != m_layersCanvas)
        m_selected.clear();
    m_selected.removeIf([count](int p) { return p < 0 || p >= count; });
    if (m_selected.isEmpty() && count > 0 && !m_editing)
        m_selected = {count - 1}; // as in Photoshop, a layer is always the current one: the top, to start
    m_layersCanvas = canvasId();
    if (m_anchor >= count)
        m_anchor = -1;
    if (m_solo >= count)
        m_solo = -1;
    m_layersOf = shown;
    emit layersChanged();
}

void CanvasViewModel::selectLayer(int position, const QString &mode) {
    int count = int(m_layers.size());
    if (position < 0 || position >= count)
        return;
    if (mode == QLatin1String("toggle")) {
        if (!m_selected.removeOne(position))
            m_selected.append(position);
        m_anchor = position;
    } else if (mode == QLatin1String("range") && m_anchor >= 0) {
        m_selected.clear();
        for (int p = std::min(m_anchor, position); p <= std::max(m_anchor, position); ++p)
            m_selected.append(p);
    } else {
        m_selected = {position};
        m_anchor = position;
    }
    emit layersChanged();
}

void CanvasViewModel::selectNone() {
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    emit layersChanged();
}

void CanvasViewModel::setSolo(int position) {
    const int want = position >= 0 && position < int(m_layers.size()) && position != m_solo ? position : -1;
    if (want == m_solo)
        return;
    m_solo = want;
    updateSoloPicture();
    emit layersChanged();
    emit pictureChanged();
}

void CanvasViewModel::updateSoloPicture() {
    m_soloSource = QUrl();
    if (m_solo < 0 || revisionB().isEmpty())
        return;
    const QString path = m_folder.filePath(QStringLiteral("solo-%1-%2.png").arg(hexOf(revisionB())).arg(m_solo));
    if (!QFile::exists(path)) {
        Result<mask::PaintDocument> document = store().load(canvasId(), revisionB());
        if (!document) {
            fail(document.errors());
            return;
        }
        for (int i = 0; i < document->layerCount(); ++i)
            document->setVisible(document->layer(i).id, i == m_solo);
        const Result<QByteArray> png = document->flattenToPng();
        QFile file(path);
        if (!png || !file.open(QIODevice::WriteOnly) || file.write(png.value()) != png->size())
            return;
    }
    m_soloSource = QUrl::fromLocalFile(path);
}

bool CanvasViewModel::applyEdit(const canvas::LayerEdit &edit, const QList<int> &select) {
    if (!layersEditable() || edit.layers.empty())
        return false;
    if (!error().isEmpty()) {
        m_error.clear();
        emit errorChanged();
    }
    const Result<canvas::Commit> done = store().editLayers(canvasId(), revisionB(), edit, QStringLiteral("canvas-view:") + canvas::layerOpName(edit.op));
    if (!done) {
        fail(done.errors());
        return false;
    }
    if (!done->move.moved) {
        if (done->revision.id == revisionB() && done->move.head == revisionB())
            return false; // nothing to change
        fail({makeError(QStringLiteral("canvas_head_moved"), tr("The canvas changed meanwhile; nothing was changed."), tr("Look at it again and do it once more."))});
        refresh();
        return false;
    }
    m_keepSelection = select;
    m_editing = true;
    m_a.clear();
    m_b.clear();
    reload(); // on the new head
    m_editing = false;
    return true;
}

namespace {

canvas::LayerEdit makeEdit(canvas::LayerOp op, const QList<int> &positions) {
    canvas::LayerEdit edit;
    edit.op = op;
    edit.layers.assign(positions.begin(), positions.end());
    return edit;
}

} // namespace

bool CanvasViewModel::setVisible(const QVariantList &positions, bool visible) {
    canvas::LayerEdit edit = makeEdit(canvas::LayerOp::Visible, positionsOf(positions));
    edit.flag = visible;
    return applyEdit(edit, m_selected);
}

bool CanvasViewModel::toggleVisible(const QVariantList &positions) {
    const QList<int> chosen = positionsOf(positions);
    bool anyShown = false;
    for (const QVariant &row : std::as_const(m_layers))
        if (chosen.contains(row.toMap().value("position").toInt()) && row.toMap().value("visible").toBool())
            anyShown = true;
    return setVisible(positions, !anyShown);
}

bool CanvasViewModel::setOpacity(const QVariantList &positions, double opacity) {
    canvas::LayerEdit edit = makeEdit(canvas::LayerOp::Opacity, positionsOf(positions));
    edit.number = std::clamp(opacity, 0.0, 1.0);
    return applyEdit(edit, m_selected);
}

bool CanvasViewModel::setBlend(const QVariantList &positions, const QString &blend) {
    canvas::LayerEdit edit = makeEdit(canvas::LayerOp::Blend, positionsOf(positions));
    edit.text = blend;
    return applyEdit(edit, m_selected);
}

bool CanvasViewModel::rename(int position, const QString &name) {
    canvas::LayerEdit edit = makeEdit(canvas::LayerOp::Rename, {position});
    edit.text = name;
    return applyEdit(edit, {position});
}

bool CanvasViewModel::removeLayers(const QVariantList &positions) {
    const QList<int> chosen = positionsOf(positions);
    if (chosen.isEmpty())
        return false;
    // The layer under the lowest one removed is selected next (the bottom one when that was the bottom).
    const int lowest = *std::min_element(chosen.begin(), chosen.end());
    const int left = int(m_layers.size()) - int(chosen.size());
    QList<int> next;
    if (left > 0)
        next = {std::clamp(lowest - 1, 0, left - 1)};
    return applyEdit(makeEdit(canvas::LayerOp::Remove, chosen), next);
}

bool CanvasViewModel::duplicate(const QVariantList &positions) {
    QList<int> chosen = positionsOf(positions);
    std::sort(chosen.begin(), chosen.end());
    QList<int> copies; // each copy lands right above its layer, after the copies below it
    for (int i = 0; i < chosen.size(); ++i)
        copies.append(chosen[i] + i + 1);
    return applyEdit(makeEdit(canvas::LayerOp::Duplicate, chosen), copies);
}

bool CanvasViewModel::moveLayers(const QVariantList &positions, int to) {
    const QList<int> chosen = positionsOf(positions);
    const int others = int(m_layers.size()) - int(chosen.size());
    if (chosen.isEmpty() || others < 0)
        return false;
    to = std::clamp(to, 0, others);
    canvas::LayerEdit edit = makeEdit(canvas::LayerOp::Move, chosen);
    edit.to = to;
    QList<int> block;
    for (int i = 0; i < chosen.size(); ++i)
        block.append(to + i);
    return applyEdit(edit, block);
}

namespace {

// How many of the layers that are not chosen lie under the lowest chosen one.
int othersBelow(const QList<int> &chosen) {
    const int lowest = *std::min_element(chosen.begin(), chosen.end());
    int below = 0;
    for (int p = 0; p < lowest; ++p)
        if (!chosen.contains(p))
            ++below;
    return below;
}

} // namespace

bool CanvasViewModel::raise(const QVariantList &positions) {
    const QList<int> chosen = positionsOf(positions);
    return !chosen.isEmpty() && moveLayers(positions, othersBelow(chosen) + 1);
}

bool CanvasViewModel::lower(const QVariantList &positions) {
    const QList<int> chosen = positionsOf(positions);
    return !chosen.isEmpty() && othersBelow(chosen) > 0 && moveLayers(positions, othersBelow(chosen) - 1);
}

bool CanvasViewModel::dropAt(const QVariantList &positions, int gap) {
    const QList<int> chosen = positionsOf(positions);
    const int count = int(m_layers.size());
    if (chosen.isEmpty())
        return false;
    const int line = count - std::clamp(gap, 0, count); // layers under the line, bottom first
    int to = 0;
    for (int p = 0; p < line; ++p)
        if (!chosen.contains(p))
            ++to;
    return moveLayers(positions, to);
}

bool CanvasViewModel::setMaskEnabled(const QVariantList &positions, bool enabled) {
    canvas::LayerEdit edit = makeEdit(canvas::LayerOp::MaskEnabled, positionsOf(positions));
    edit.flag = enabled;
    return applyEdit(edit, m_selected);
}

bool CanvasViewModel::removeMask(const QVariantList &positions) { return applyEdit(makeEdit(canvas::LayerOp::RemoveMask, positionsOf(positions)), m_selected); }

bool CanvasViewModel::undo() {
    if (!canUndo())
        return false;
    const Result<canvas::HeadMove> moved = store().undo(canvasId(), m_head);
    if (!moved || !moved->moved) {
        if (!moved)
            fail(moved.errors());
        refresh();
        return false;
    }
    m_keepSelection = m_selected;
    m_editing = true;
    m_a.clear();
    m_b.clear();
    reload();
    m_editing = false;
    return true;
}

bool CanvasViewModel::redo() {
    if (!canRedo())
        return false;
    const Result<canvas::HeadMove> moved = store().redo(canvasId(), m_head);
    if (!moved || !moved->moved) {
        if (!moved)
            fail(moved.errors());
        refresh();
        return false;
    }
    m_keepSelection = m_selected;
    m_editing = true;
    m_a.clear();
    m_b.clear();
    reload();
    m_editing = false;
    return true;
}

} // namespace fugue
