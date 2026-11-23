#include "canvas/canvas_view_model.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLocale>
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
    // The strokes of a mask, batched: one revision once the brush has rested this long (#230).
    m_maskIdle.setSingleShot(true);
    m_maskIdle.setInterval(400);
    connect(&m_maskIdle, &QTimer::timeout, this, [this] { commitMask(); });
    connect(this, &CanvasHistory::revisionsChanged, this, [this] { rebuild(); });
    connect(this, &CanvasHistory::compareChanged, this, [this] {
        rebuildLayers();
        emit pictureChanged();
        updateRegion();
        // Anything else shown (an undo of the layer included) ends the Head's notice.
        if (!m_headNotice.isEmpty() && revisionB() != m_noticeHead)
            clearHeadNotice();
    });
}

void CanvasViewModel::watch() {
    // The store's canvases folder, and the canvas's own with its commits and resolved folders: a new revision is a file
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
    const QString headBefore = m_head;
    QStringList namesBefore;
    if (onHead)
        for (const QVariant &row : std::as_const(m_layers))
            namesBefore << row.toMap().value("name").toString();
    reload();
    if (onHead && !m_head.isEmpty() && revisionB() != m_head)
        select(m_head); // a view on the head goes on to the new one
    // Moved by something else (this view's own accept, edits, undo and redo reload at once, so the watch finds no move):
    // say which layers came, as a notice with Undo.
    if (onHead && !headBefore.isEmpty() && m_head != headBefore && revisionB() == m_head) {
        QStringList added;
        for (const QVariant &row : std::as_const(m_layers))
            if (const QString name = row.toMap().value("name").toString(); !namesBefore.removeOne(name))
                added << name;
        m_noticeHead = m_head;
        m_headNotice = added.size() == 1 ? tr("Layer %1 added").arg(added.first())
                     : added.size() > 1 ? tr("%n layers added", nullptr, int(added.size())) : QString();
        emit headNoticeChanged();
    }
    watch();
    emit storeChanged(); // the canvas list too
    // A read that failed (a run is writing, a slow disk) is tried again a few times before it is left to the person.
    if (!error().isEmpty() && !canvasId().isEmpty() && m_retries < 3) {
        ++m_retries;
        m_settle.start(400);
    } else if (error().isEmpty()) {
        m_retries = 0;
    }
}

void CanvasViewModel::clearHeadNotice() {
    if (m_headNotice.isEmpty())
        return;
    m_headNotice.clear();
    emit headNoticeChanged();
}

void CanvasViewModel::rebuild() {
    watch();
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
    // The History panel's steps: the Head's revisions in words; with Show all revisions the candidates first.
    m_steps.clear();
    m_allSteps.clear();
    QVariantList candidates;
    for (const QVariant &value : revisions()) {
        const QVariantMap row = value.toMap();
        const QString kind = row.value("kind").toString();
        const QString id = row.value("id").toString();
        const QDateTime time = QDateTime::fromString(row.value("time").toString(), Qt::ISODateWithMs).toLocalTime();
        QVariantMap step{{"id", id}, {"time", row.value("time")}, {"kind", kind}, {"latest", kind == QLatin1String("head")},
                         {"timeText", time.isValid() ? QLocale().toString(time, QLocale::ShortFormat) : QString()}};
        const QString name = stepName(id);
        if (kind == QLatin1String("candidate") || kind == QLatin1String("dropped")) {
            step.insert("name", kind == QLatin1String("candidate") ? tr("Candidate: %1").arg(name) : tr("Dropped: %1").arg(name));
            candidates.append(step);
        } else {
            step.insert("name", name);
            m_steps.append(step);
        }
    }
    m_allSteps = candidates + m_steps;
    m_headSource = QUrl();
    m_headSize = QSize();
    if (!m_head.isEmpty())
        picture(m_head, &m_headSource, &m_headSize);
    emit choicesChanged();
    emit pictureChanged();
}

QString CanvasViewModel::stepName(const QString &revisionId) {
    if (const auto known = m_stepNames.constFind(revisionId); known != m_stepNames.constEnd())
        return *known;
    const canvas::CanvasStore s = store();
    const Result<canvas::Revision> revision = s.revision(canvasId(), revisionId);
    if (!revision)
        return tr("A change");
    const Result<canvas::Tree> tree = s.tree(revision->tree);
    if (revision->parent.isEmpty() || !tree) {
        const QString first = revision->producer == QLatin1String("import") ? tr("Picture imported") : tr("Canvas made");
        m_stepNames.insert(revisionId, first);
        return first;
    }
    const Result<canvas::Revision> parentRevision = s.revision(canvasId(), revision->parent);
    const Result<canvas::Tree> parent = parentRevision ? s.tree(parentRevision->tree) : Result<canvas::Tree>(parentRevision.errors());
    QString name = tr("A change");
    if (parent) {
        const std::vector<canvas::LayerRecord> &now = tree->layers, &was = parent->layers;
        QStringList before, after;
        for (const auto &layer : was)
            before << layer.name;
        for (const auto &layer : now)
            after << layer.name;
        QStringList added = after, removed = before;
        for (const QString &n : before)
            added.removeOne(n);
        for (const QString &n : after)
            removed.removeOne(n);
        if (tree->width != parent->width || tree->height != parent->height)
            name = tr("Canvas size %1 × %2").arg(tree->width).arg(tree->height);
        else if (now.size() > was.size())
            name = added.size() == 1 ? tr("Layer added: %1").arg(added.first()) : tr("%n layers added", nullptr, int(now.size() - was.size()));
        else if (now.size() < was.size())
            name = removed.size() == 1 ? tr("Layer deleted: %1").arg(removed.first()) : tr("%n layers deleted", nullptr, int(was.size() - now.size()));
        else if (added.size() == 1 && removed.size() == 1)
            name = tr("Renamed: %1").arg(added.first());
        else if (before != after)
            name = tr("Layers reordered");
        else {
            for (size_t i = 0; i < now.size(); ++i) {
                const canvas::LayerRecord &a = was[i], &b = now[i];
                if (a == b)
                    continue;
                if (a.visible != b.visible)
                    name = b.visible ? tr("Shown: %1").arg(b.name) : tr("Hidden: %1").arg(b.name);
                else if (a.opacity != b.opacity)
                    name = tr("Opacity %1%: %2").arg(qRound(b.opacity * 100)).arg(b.name);
                else if (a.compositeOp != b.compositeOp)
                    name = tr("Blend mode: %1").arg(b.name);
                else if (a.hasMask != b.hasMask)
                    name = b.hasMask ? tr("Mask added: %1").arg(b.name) : tr("Mask deleted: %1").arg(b.name);
                else if (a.maskEnabled != b.maskEnabled)
                    name = b.maskEnabled ? tr("Mask on: %1").arg(b.name) : tr("Mask off: %1").arg(b.name);
                else
                    name = tr("Painted: %1").arg(b.name);
                break;
            }
        }
    }
    m_stepNames.insert(revisionId, name);
    return name;
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
    // The view goes to the node's canvas, when the node changes (the same one set again leaves a canvas the person chose).
    if (!same && m_edits && !m_follow.isEmpty()) {
        const QVariantMap state = m_edits->regionState(m_follow);
        if (!state.isEmpty() && !state.value("canvasLinked").toBool() && !state.value("canvas").toString().isEmpty()) {
            if (!state.value("storeRoot").toString().isEmpty() && state.value("storeRoot").toString() != storeRoot())
                setStoreRoot(state.value("storeRoot").toString());
            // Only to a canvas this root has: an unknown id is said in plain words over the canvas shown (#228).
            if (store().info(state.value("canvas").toString()))
                setCanvasId(state.value("canvas").toString());
        }
    }
    updateRegion();
}

QUrl CanvasViewModel::pictureSource() const {
    if (m_solo >= 0 && !m_soloSource.isEmpty())
        return m_soloSource;
    return sourceB();
}

QSize CanvasViewModel::pictureSize() const { return sizeB(); }

void CanvasViewModel::show(const QString &revision) { select(revision); }

void CanvasViewModel::updateRegion() {
    QVariantMap out{{"valid", false}, {"reason", QString()}};
    if (m_edits && !m_follow.isEmpty()) {
        const QVariantMap state = m_edits->regionState(m_follow);
        const QSize size = pictureSize();
        // The node's canvas: empty is none yet, and the canvas shown is adopted at the first edit (#228); another canvas
        // is said in plain words with what to do (the view offers both answers); a link is changed where it comes from.
        const QString nodeCanvas = state.value("canvas").toString();
        if (state.isEmpty()) {
            out.insert("reason", tr("The selected node is not a Canvas Region."));
            out.insert("problem", QStringLiteral("notRegion"));
        } else if (state.value("canvasLinked").toBool()) {
            out.insert("reason", tr("This Canvas Region's canvas comes from a linked node: change it there."));
            out.insert("problem", QStringLiteral("linked"));
        } else if (!nodeCanvas.isEmpty() && nodeCanvas != canvasId()) {
            const Result<canvas::CanvasInfo> info = store().info(nodeCanvas);
            out.insert("problem", QStringLiteral("otherCanvas"));
            out.insert("nodeCanvas", nodeCanvas);
            out.insert("known", bool(info));
            out.insert("nodeCanvasName", info ? info->name : QString());
            out.insert("reason", info ? tr("This Canvas Region reads %1, not the canvas shown.").arg(info->name)
                                      : tr("This Canvas Region reads a canvas this root does not have (%1).").arg(nodeCanvas));
        }
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
                              {"adopts", nodeCanvas.isEmpty()},
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
                              {"target", state.value("target").toDouble()},
                              {"feather", state.value("feather").toInt()},
                              {"multiple", state.value("multiple").toInt()},
                              {"selection", state.value("selection").toString()},
                              {"selectionWritable", state.value("selectionWritable").toBool()},
                              // The node's numbers are all 0 (and no painted selection): it reads the whole canvas and the
                              // view shows no selection (Select > Deselect leaves it so, #232).
                              {"unset", state.value("x").toInt() == 0 && state.value("y").toInt() == 0 && state.value("width").toInt() == 0
                                            && state.value("height").toInt() == 0 && state.value("selection").toString().isEmpty()}};
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
                                    m_region.value("multiple").toInt(), m_region.value("target").toDouble(), m_region.value("feather").toInt()};
    const crop::Rect context = region::context(r, settings.padding, canvas, settings.feather);
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
    // A node with no canvas yet takes the one shown, in the same undo step as the rectangle.
    const bool adopts = m_region.value("adopts").toBool();
    QUndoStack *stack = m_edits->undoStack();
    if (adopts) {
        stack->beginMacro(tr("Pick region"));
        m_edits->setText(m_follow, QStringLiteral("canvas"), canvasId());
    }
    const bool written = m_edits->setRegion(m_follow, r.x, r.y, r.width, r.height) || adopts;
    if (adopts)
        stack->endMacro();
    if (written)
        updateRegion();
    return written;
}

bool CanvasViewModel::useCanvasForNode() {
    if (!m_edits || m_follow.isEmpty() || canvasId().isEmpty() || m_region.value("problem").toString() != QLatin1String("otherCanvas"))
        return false;
    const QString problem = m_edits->setText(m_follow, QStringLiteral("canvas"), canvasId());
    if (!problem.isEmpty()) {
        fail({makeError(QStringLiteral("region_canvas"), problem)});
        return false;
    }
    updateRegion();
    return true;
}

void CanvasViewModel::showNodeCanvas() {
    if (m_region.value("problem").toString() == QLatin1String("otherCanvas") && m_region.value("known").toBool())
        setCanvasId(m_region.value("nodeCanvas").toString());
}

bool CanvasViewModel::useForRegion() {
    if (!m_edits || m_follow.isEmpty() || revisionB().isEmpty() || !m_region.value("valid").toBool() || !m_region.value("writable").toBool()
        || m_region.value("readsShown").toBool())
        return false;
    const QString wanted = revisionB() == m_head ? QStringLiteral("head") : revisionB();
    const bool adopts = m_region.value("adopts").toBool(); // the canvas too, in the same undo step
    QUndoStack *stack = m_edits->undoStack();
    if (adopts) {
        stack->beginMacro(tr("Use for region"));
        m_edits->setText(m_follow, QStringLiteral("canvas"), canvasId());
    }
    const QString problem = m_edits->setText(m_follow, QStringLiteral("revision"), wanted);
    if (adopts)
        stack->endMacro();
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
    syncMask();
}

void CanvasViewModel::selectLayer(int position, const QString &mode) {
    int count = int(m_layers.size());
    if (position < 0 || position >= count)
        return;
    if (m_maskTarget >= 0 && (position != m_maskTarget || mode != QLatin1String("only")))
        untargetMask(); // the brush paints the mask of the one layer it was on
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
    commitMask(); // the strokes painted so far are a step of their own, before this one
    if (!layersEditable())
        return false;
    return land(store().editLayers(canvasId(), revisionB(), edit, QStringLiteral("canvas-view:") + canvas::layerOpName(edit.op)), select);
}

bool CanvasViewModel::land(const Result<canvas::Commit> &done, const QList<int> &select) {
    if (!error().isEmpty()) {
        m_error.clear();
        emit errorChanged();
    }
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

bool CanvasViewModel::clearCanvas() {
    commitMask(); // the strokes painted so far are a step of their own, before this one
    if (!layersEditable())
        return false;
    return land(store().clear(canvasId(), revisionB(), QStringLiteral("canvas-view:clear")), {});
}

bool CanvasViewModel::undo() {
    commitMask(); // a stroke not stored yet is the step undone
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
    commitMask();
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

// ---- the mask brush (#230) -----------------------------------------------------------------------------------------

void CanvasViewModel::setMaskLayer(MaskLayer *layer) {
    if (layer == m_maskLayer)
        return;
    m_maskLayer = layer;
    if (m_maskTarget >= 0 && !loadMask(m_maskTarget))
        m_maskTarget = -1;
    emit maskChanged();
}

void CanvasViewModel::setMaskIdle(int ms) {
    if (ms == m_maskIdle.interval())
        return;
    m_maskIdle.setInterval(std::max(0, ms));
    emit maskChanged();
}

bool CanvasViewModel::loadMask(int position) {
    const canvas::CanvasStore s = store();
    const Result<canvas::Revision> revision = s.revision(canvasId(), revisionB());
    const Result<canvas::Tree> tree = revision ? s.tree(revision->tree) : Result<canvas::Tree>(revision.errors());
    if (!tree || position < 0 || position >= int(tree->layers.size()) || !tree->layers[size_t(position)].hasMask)
        return false;
    if (!m_maskLayer) {
        m_maskOf = revisionB();
        return true; // read when the view gives its layer
    }
    const int width = tree->width, height = tree->height;
    const Result<mask::MaskPaint> pixels = canvas::assembleLayer(width, height, 1, tree->layers[size_t(position)].maskTiles,
                                                                 [&](const canvas::TileKey &key, const QString &hash) {
        const mask::IRect rect = canvas::tileRect(key, width, height);
        return s.tilePixels(hash, rect.width, rect.height, 1);
    });
    if (!pixels) {
        fail(pixels.errors());
        return false;
    }
    m_maskLayer->setPaint(pixels.value());
    m_maskOf = revisionB();
    return true;
}

bool CanvasViewModel::targetMask(int position) {
    if (!layersEditable())
        return false;
    if (position == m_maskTarget && m_maskOf == revisionB())
        return true;
    commitMask();
    if (!loadMask(position))
        return false;
    m_maskTarget = position;
    m_selected = {position};
    m_anchor = position;
    emit layersChanged();
    emit maskChanged();
    return true;
}

void CanvasViewModel::untargetMask() {
    if (m_maskTarget < 0)
        return;
    commitMask();
    m_maskTarget = -1;
    m_maskOf.clear();
    emit maskChanged();
}

void CanvasViewModel::syncMask() {
    if (m_maskTarget < 0 || m_maskOf == revisionB())
        return;
    // Another revision is shown (an undo, a run's layer, a step of History): the target goes when it cannot be painted
    // there, else its mask is read again from it. Strokes not stored yet were committed before (undo, redo) or are lost
    // with a head that moved under them (commitMask places them when it can).
    m_maskDirty = false;
    m_maskIdle.stop();
    if (!layersEditable() || !loadMask(m_maskTarget)) {
        m_maskTarget = -1;
        m_maskOf.clear();
    }
    emit maskChanged();
}

void CanvasViewModel::maskPainted() {
    if (m_maskTarget < 0)
        return;
    const bool was = m_maskDirty;
    m_maskDirty = true;
    m_maskIdle.start();
    if (!was)
        emit maskChanged();
}

bool CanvasViewModel::commitMask(const QString &what) {
    m_maskIdle.stop();
    if (!m_maskDirty)
        return false;
    m_maskDirty = false;
    emit maskChanged();
    if (m_maskTarget < 0 || !m_maskLayer || !layersEditable())
        return false;
    const QString producer = QStringLiteral("canvas-view:") + what;
    Result<canvas::Commit> done = store().setLayerMask(canvasId(), revisionB(), m_maskTarget, m_maskLayer->paint(), producer);
    if (done && !done->move.moved && done->move.head != revisionB()) {
        // A run's layer landed while the strokes rested: they go onto the head as it is now when the layer is still
        // there (a run adds its layer on top, so the positions below stay).
        canvas::CanvasStore s = store();
        const Result<canvas::Revision> was = s.revision(canvasId(), revisionB()), now = s.revision(canvasId(), done->move.head);
        const Result<canvas::Tree> before = was ? s.tree(was->tree) : Result<canvas::Tree>(was.errors());
        const Result<canvas::Tree> after = now ? s.tree(now->tree) : Result<canvas::Tree>(now.errors());
        if (before && after && m_maskTarget < int(after->layers.size()) && after->layers[size_t(m_maskTarget)].name == before->layers[size_t(m_maskTarget)].name)
            done = s.setLayerMask(canvasId(), done->move.head, m_maskTarget, m_maskLayer->paint(), producer);
    }
    if (!done || !done->move.moved)
        return land(done, m_selected);
    m_maskOf = done->revision.id; // the layer holds this revision's mask already: no read back
    return land(done, {m_maskTarget});
}

bool CanvasViewModel::addMask(int position) {
    if (!layersEditable() || position < 0 || position >= int(m_layers.size()))
        return false;
    for (const QVariant &row : std::as_const(m_layers)) {
        const QVariantMap layer = row.toMap();
        if (layer.value("position").toInt() != position)
            continue;
        if (layer.value("kind").toString() == QLatin1String("mask"))
            return false;
        if (layer.value("hasMask").toBool())
            return targetMask(position);
    }
    commitMask();
    const QSize size = sizeB();
    mask::MaskPaint full(std::max(1, size.width()), std::max(1, size.height()));
    full.fill();
    if (!land(store().setLayerMask(canvasId(), revisionB(), position, full, QStringLiteral("canvas-view:mask-add")), {position}))
        return false;
    return targetMask(position);
}

bool CanvasViewModel::invertMask() {
    if (m_maskTarget < 0 || !m_maskLayer || !layersEditable())
        return false;
    commitMask();
    m_maskLayer->invert();
    m_maskDirty = true;
    return commitMask(QStringLiteral("mask-invert"));
}

bool CanvasViewModel::fillMask() {
    if (m_maskTarget < 0 || !m_maskLayer || !layersEditable())
        return false;
    commitMask();
    m_maskLayer->fill();
    m_maskDirty = true;
    return commitMask(QStringLiteral("mask-fill"));
}

bool CanvasViewModel::clearMask() {
    if (m_maskTarget < 0 || !m_maskLayer || !layersEditable())
        return false;
    commitMask();
    m_maskLayer->clear();
    m_maskDirty = true;
    return commitMask(QStringLiteral("mask-clear"));
}

QVariantMap CanvasViewModel::useSelection(MaskLayer *selection) {
    const auto no = [](const QString &why) { return QVariantMap{{"ok", false}, {"reason", why}}; };
    if (!selection)
        return no(tr("Nothing is selected."));
    if (!m_edits || m_follow.isEmpty() || !m_region.value("valid").toBool())
        return no(tr("No Canvas Region reads this canvas: add one to use the selection."));
    if (!m_region.value("selectionWritable").toBool())
        return no(tr("This Canvas Region takes no selection: its Selection input is linked, or the node is an older version without one."));
    const mask::MaskPaint &painted = selection->paint();
    int left = painted.width(), top = painted.height(), right = -1, bottom = -1;
    for (int y = 0; y < painted.height(); ++y)
        for (int x = 0; x < painted.width(); ++x)
            if (painted.at(x, y) > 0) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    if (right < 0)
        return no(tr("Nothing is selected: paint the part to change first."));
    const Result<QString> hash = store().storeSelection(painted);
    if (!hash) {
        fail(hash.errors());
        return no(hash.error().message);
    }
    const bool adopts = m_region.value("adopts").toBool();
    QUndoStack *stack = m_edits->undoStack();
    if (adopts) { // the canvas too, in the same undo step
        stack->beginMacro(tr("Use selection"));
        m_edits->setText(m_follow, QStringLiteral("canvas"), canvasId());
    }
    const bool written = m_edits->setSelection(m_follow, hash.value(), left, top, right - left + 1, bottom - top + 1) || adopts;
    if (adopts)
        stack->endMacro();
    updateRegion();
    return written ? QVariantMap{{"ok", true}, {"hash", hash.value()}} : no(tr("The Canvas Region uses this selection already."));
}

} // namespace fugue
