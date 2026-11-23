#include "canvas/canvas_view_model.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QMimeData>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QSet>
#include <QLocale>
#include <QPointer>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "canvas/picture_import.h"
#include "engine/canvas_copy.h"
#include "canvas/region_pick.h"
#include "canvas/snap.h"

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
        const QString producer = revision->producer;
        if (tree->width != parent->width || tree->height != parent->height)
            name = tr("Canvas size %1 × %2").arg(tree->width).arg(tree->height);
        else if (producer == QLatin1String("canvas-view:merge-down") || producer == QLatin1String("canvas-cli:merge-down"))
            name = removed.size() == 1 ? tr("Merged down: %1").arg(removed.first()) : tr("Merged down");
        else if (producer == QLatin1String("canvas-view:flatten") || producer == QLatin1String("canvas-cli:flatten"))
            name = tr("Flattened");
        else if (producer == QLatin1String("canvas-view:group") || producer == QLatin1String("canvas-cli:group"))
            name = added.size() == 1 ? tr("Grouped: %1").arg(added.first()) : tr("Grouped");
        else if (producer == QLatin1String("canvas-view:ungroup") || producer == QLatin1String("canvas-cli:ungroup"))
            name = removed.size() == 1 ? tr("Ungrouped: %1").arg(removed.first()) : tr("Ungrouped");
        else if (producer == QLatin1String("canvas-view:transform")) {
            // The layer whose placement changed, and in what way: moved, scaled, turned, mirrored, or several of them.
            QString changed;
            enum { Moved, Scaled, Rotated, Flipped, Resampled, Several };
            QList<int> how;
            for (size_t i = 0; i < now.size() && i < was.size() && changed.isEmpty(); ++i) {
                if (now[i] == was[i])
                    continue;
                changed = now[i].name;
                if (!now[i].source)
                    break;
                const canvas::LayerTransform &after = now[i].source->transform;
                if (!was[i].source || !canvas::hasCurrentOriginal(was[i])) {
                    // The first one, from the layer's own pixels: what it was before is the original's size at no turn.
                    if (after.flipX || after.flipY)
                        how << Flipped;
                    else if (after.angle != 0)
                        how << Rotated;
                    if (after.width != now[i].source->width || after.height != now[i].source->height)
                        how << Scaled;
                    if (how.isEmpty())
                        how << Moved;
                    break;
                }
                const canvas::LayerTransform &before = was[i].source->transform;
                if (before.flipX != after.flipX || before.flipY != after.flipY)
                    how << Flipped;
                else if (before.angle != after.angle)
                    how << Rotated;
                if (before.width != after.width || before.height != after.height)
                    how << Scaled;
                if (before.cx != after.cx || before.cy != after.cy)
                    how << Moved;
                if (before.filter != after.filter)
                    how << Resampled;
            }
            if (changed.isEmpty())
                name = tr("Layer transformed");
            else if (how.size() != 1)
                name = tr("Layer transformed: %1").arg(changed);
            else
                name = how.first() == Moved ? tr("Layer moved: %1").arg(changed)
                       : how.first() == Scaled ? tr("Layer scaled: %1").arg(changed)
                       : how.first() == Rotated ? tr("Layer rotated: %1").arg(changed)
                       : how.first() == Flipped ? tr("Layer flipped: %1").arg(changed)
                       : how.first() == Resampled ? tr("Layer resampled: %1").arg(changed) : tr("Layer transformed: %1").arg(changed);
        }
        else if (producer == QLatin1String("canvas-view:paste"))
            name = added.size() == 1 ? tr("Layer pasted: %1").arg(added.first()) : tr("Layer pasted");
        else if (producer == QLatin1String("canvas-view:properties")) {
            // The layer the dialog changed: the first one that differs (its new name when it was renamed).
            QString changed;
            for (size_t i = 0; i < now.size() && i < was.size() && changed.isEmpty(); ++i)
                if (!(now[i] == was[i]))
                    changed = now[i].name;
            name = tr("Properties: %1").arg(changed);
        }
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
                else if (a.locked != b.locked)
                    name = b.locked ? tr("Locked: %1").arg(b.name) : tr("Unlocked: %1").arg(b.name);
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
    if (m_xPosition >= 0 && !m_xBase.isEmpty() && m_xOf == revisionB())
        return m_xBase;
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
        // The node's canvas: empty is the Active canvas (#246), which edits here keep empty (the view reports this canvas as
        // the active one when it is used); a pinned canvas that is not the one shown is said in plain words with what to do
        // (the view offers both answers); a link is changed where it comes from.
        const QString nodeCanvas = state.value("canvas").toString();
        out.insert("pinned", !nodeCanvas.isEmpty());
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
                              {"active", nodeCanvas.isEmpty()},
                              {"pinned", !nodeCanvas.isEmpty()},
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
    // The same snap the Canvas view's frame uses (canvas/snap.h, GuidesModel::snapRect), with the node's Multiple of alone.
    snap::Targets targets;
    targets.multiple = m_region.value("multiple").toInt();
    return mapOf(snap::snapRect(rectOf(rect), targets, {size.width(), size.height()}, keepSize));
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
    const bool written = m_edits->setRegion(m_follow, r.x, r.y, r.width, r.height);
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

bool CanvasViewModel::useActiveCanvasForNode() {
    if (!m_edits || m_follow.isEmpty() || m_region.value("pinned").toBool() == false)
        return false;
    const QString problem = m_edits->setText(m_follow, QStringLiteral("canvas"), QString());
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

QString CanvasViewModel::currentLayer() const {
    if (m_selected.size() != 1)
        return {};
    QSet<QString> above; // top first: the first layer of a name is the one Region reads
    for (const canvas::OutlineRow &row : m_outline.allRows()) { // the top level only: Region reads a layer of the top level by name
        if (row.depth > 0)
            continue;
        if (row.id == m_selected.first()) {
            const canvas::LayerRecord *layer = layerAt(row.path);
            return layer && !layer->group && layer->kind == mask::LayerKind::Paint && !above.contains(row.name) ? row.name : QString();
        }
        above.insert(row.name);
    }
    return {};
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
        endTransform(); // a transform in progress was of the revision before
        m_solo = -1;
        m_soloSource = QUrl();
    }
    const canvas::CanvasStore s = store();
    m_tree = canvas::Tree();
    if (!shown.isEmpty()) {
        const Result<canvas::Revision> revision = s.revision(canvasId(), shown);
        const Result<canvas::Tree> tree = revision ? s.tree(revision->tree) : Result<canvas::Tree>(revision.errors());
        if (tree) {
            m_tree = tree.value();
            m_outline.setTree(m_tree);
            writeThumbnails(s, shown);
            if (layersEditable()) {
                m_canUndo = !revision->parent.isEmpty();
                const Result<QString> target = s.redoTarget(canvasId());
                m_canRedo = target && !target->isEmpty();
            }
        }
    }
    if (m_tree.layers.empty())
        m_outline.setTree(m_tree);
    const int count = m_outline.topCount();
    const int total = int(m_outline.allRows().size());
    // The selection: what an edit chose, else kept on the same revision, else kept where it still fits.
    if (m_editing) {
        m_selected.clear();
        for (const canvas::LayerPath &path : std::as_const(m_keepSelection))
            if (const int id = m_outline.idOf(path); id >= 0 && !m_selected.contains(id))
                m_selected.append(id);
    } else if (canvasId() != m_layersCanvas)
        m_selected.clear();
    // The layer a Generate reads: kept while the shown revision has a paint layer of that name, and never across canvases.
    if (canvasId() != m_layersCanvas || std::none_of(m_outline.allRows().cbegin(), m_outline.allRows().cend(), [this](const canvas::OutlineRow &row) {
            const canvas::LayerRecord *layer = layerAt(row.path);
            return row.name == m_regionLayer && layer && !layer->group && layer->kind == mask::LayerKind::Paint;
        }))
        m_regionLayer.clear();
    m_selected.removeIf([total](int p) { return p < 0 || p >= total; });
    if (m_selected.isEmpty() && count > 0 && !m_editing)
        m_selected = {count - 1}; // as in Photoshop, a layer is always the current one: the top, to start
    m_layersCanvas = canvasId();
    m_layersOf = shown;
    buildRows(); // fits the selection to the open groups, and the anchor and the solo to the rows there are
    if (m_anchor >= total)
        m_anchor = -1;
    if (m_solo >= count)
        m_solo = -1;
    emit layersChanged();
    syncMask();
}

const canvas::LayerRecord *CanvasViewModel::layerAt(const canvas::LayerPath &path) const {
    const std::vector<canvas::LayerRecord> *level = &m_tree.layers;
    const canvas::LayerRecord *found = nullptr;
    for (int index : path) {
        if (index < 0 || index >= int(level->size()))
            return nullptr;
        found = &(*level)[size_t(index)];
        level = &found->children;
    }
    return found;
}

QString CanvasViewModel::thumbnailPath(const QString &revision, int id, bool ofMask) const {
    return m_folder.filePath(QStringLiteral("t-%1-%2%3.png").arg(hexOf(revision)).arg(id).arg(ofMask ? "m" : ""));
}

// The thumbnails of a revision, made once (a revision never changes): the top level's from the document (a group is its composite),
// the layers inside a group from their own tiles (a group inside a group has none: the row draws a folder).
void CanvasViewModel::writeThumbnails(const canvas::CanvasStore &s, const QString &revision) {
    const int count = m_outline.topCount();
    bool missing = false;
    for (int i = 0; i < count && !missing; ++i) {
        const canvas::LayerRecord &layer = m_tree.layers[size_t(i)];
        missing = (layer.kind != mask::LayerKind::Picture && !QFile::exists(thumbnailPath(revision, i, false))) || (layer.hasMask && !QFile::exists(thumbnailPath(revision, i, true)));
    }
    if (missing) {
        const Result<mask::PaintDocument> document = s.load(canvasId(), revision);
        if (document)
            for (int i = 0; i < document->layerCount(); ++i) {
                const mask::LayerInfo info = document->layer(i);
                if (const mask::MaskPaint *pixels = info.kind != mask::LayerKind::Picture ? document->pixels(info.id) : nullptr)
                    writeThumbnail(*pixels, thumbnailPath(revision, i, false));
                if (const mask::MaskPaint *maskPixels = document->maskPixels(info.id))
                    writeThumbnail(*maskPixels, thumbnailPath(revision, i, true));
            }
    }
    const int width = m_tree.width, height = m_tree.height;
    const auto assemble = [&](const canvas::TileMap &tiles, int channels) {
        return canvas::assembleLayer(width, height, channels, tiles, [&](const canvas::TileKey &key, const QString &hash) {
            const mask::IRect rect = canvas::tileRect(key, width, height);
            return s.tilePixels(hash, rect.width, rect.height, channels);
        });
    };
    for (const canvas::OutlineRow &row : m_outline.allRows()) {
        const canvas::LayerRecord *layer = row.id >= count ? layerAt(row.path) : nullptr;
        if (!layer)
            continue;
        if (!layer->group && layer->kind != mask::LayerKind::Picture && !QFile::exists(thumbnailPath(revision, row.id, false)))
            if (const Result<mask::MaskPaint> pixels = assemble(layer->tiles, layer->channels()))
                writeThumbnail(pixels.value(), thumbnailPath(revision, row.id, false));
        if (layer->hasMask && !QFile::exists(thumbnailPath(revision, row.id, true)))
            if (const Result<mask::MaskPaint> pixels = assemble(layer->maskTiles, 1))
                writeThumbnail(pixels.value(), thumbnailPath(revision, row.id, true));
    }
}

// The rows the list shows, top first: the open groups' children right under them.
void CanvasViewModel::buildRows() {
    m_layers.clear();
    const QString shown = revisionB();
    std::vector<int> fitted = m_outline.fitSelection(std::vector<int>(m_selected.cbegin(), m_selected.cend()));
    m_selected = QList<int>(fitted.cbegin(), fitted.cend());
    for (const canvas::OutlineRow &row : m_outline.rows()) {
        const canvas::LayerRecord *found = layerAt(row.path);
        if (!found)
            continue;
        const canvas::LayerRecord &layer = *found;
        bool known = true;
        const mask::Blend blend = mask::blendOfOp(layer.compositeOp, &known);
        const QString kind = layer.group ? QStringLiteral("group") : layer.kind == mask::LayerKind::Mask ? QStringLiteral("mask") : layer.kind == mask::LayerKind::Picture ? QStringLiteral("picture") : QStringLiteral("paint");
        const QString thumb = thumbnailPath(shown, row.id, false), maskThumb = thumbnailPath(shown, row.id, true);
        m_layers.append(QVariantMap{{"position", row.id},
                                    {"name", layer.name},
                                    {"kind", kind},
                                    {"visible", layer.visible},
                                    {"shown", row.shown},
                                    {"opacity", layer.opacity},
                                    {"blend", known ? mask::blendId(blend) : QString()},
                                    {"compositeOp", layer.compositeOp.isEmpty() ? QStringLiteral("svg:src-over") : layer.compositeOp},
                                    {"hasMask", layer.hasMask},
                                    {"maskEnabled", layer.maskEnabled},
                                    {"locked", layer.locked},
                                    {"lockedAbove", row.lockedAbove},
                                    {"producer", layer.producer},
                                    {"from", fromText(layer.producer)},
                                    {"created", layer.created},
                                    {"depth", row.depth},
                                    {"children", int(layer.children.size())},
                                    {"open", row.open},
                                    {"thumbnail", QFile::exists(thumb) ? QUrl::fromLocalFile(thumb) : QUrl()},
                                    {"maskThumbnail", layer.hasMask && QFile::exists(maskThumb) ? QUrl::fromLocalFile(maskThumb) : QUrl()}});
    }
}

bool CanvasViewModel::setOpen(int position, bool open) {
    const canvas::OutlineRow *row = m_outline.row(position);
    if (!row || !m_outline.setOpen(row->path, open))
        return false;
    buildRows();
    if (m_anchor >= 0 && !m_outline.row(m_anchor))
        m_anchor = -1;
    emit layersChanged();
    return true;
}

bool CanvasViewModel::toggleOpen(int position) {
    const canvas::OutlineRow *row = m_outline.row(position);
    return row && setOpen(position, !row->open);
}

void CanvasViewModel::setAllOpen(bool open) {
    m_outline.setAllOpen(open);
    buildRows();
    emit layersChanged();
}

void CanvasViewModel::selectLayer(int position, const QString &mode) {
    if (!m_outline.row(position))
        return;
    if (m_maskTarget >= 0 && (position != m_maskTarget || mode != QLatin1String("only")))
        untargetMask(); // the brush paints the mask of the one layer it was on
    if (mode == QLatin1String("toggle")) {
        if (!m_selected.removeOne(position))
            m_selected.append(position);
        m_anchor = position;
    } else if (const std::vector<int> range = mode == QLatin1String("range") && m_anchor >= 0 ? m_outline.rangeBetween(m_anchor, position) : std::vector<int>(); !range.empty()) {
        m_selected.clear();
        for (auto it = range.rbegin(); it != range.rend(); ++it) // bottom first, as the positions are
            m_selected.append(*it);
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
    const int want = position >= 0 && position < m_outline.topCount() && position != m_solo ? position : -1; // a layer of the top level
    if (want == m_solo)
        return;
    m_solo = want;
    updateSoloPicture();
    emit layersChanged();
    emit pictureChanged();
}

bool CanvasViewModel::setRegionLayer(int position) {
    QString want;
    if (position >= 0) {
        QString problem = tr("There is no such layer.");
        QSet<QString> above; // top first: the first layer of a name is the one Region reads
        for (const canvas::OutlineRow &row : m_outline.allRows()) {
            if (row.depth > 0 && row.id == position) {
                problem = tr("Generate reads a layer of the top level: take it out of its group first.");
                break;
            }
            if (row.depth > 0)
                continue;
            const QString name = row.name;
            if (row.id == position) {
                const canvas::LayerRecord *layer = layerAt(row.path);
                if (!layer || layer->group || layer->kind != mask::LayerKind::Paint)
                    problem = tr("Only a paint layer can be read by Generate, not a group, a mask or a picture layer.");
                else if (above.contains(name))
                    problem = tr("A layer above has the same name: rename one, so Generate reads this layer.");
                else {
                    problem.clear();
                    want = name;
                }
                break;
            }
            above.insert(name);
        }
        if (!problem.isEmpty()) {
            fail({makeError(QStringLiteral("input_invalid"), problem)});
            return false;
        }
        if (want == m_regionLayer)
            want.clear(); // the same layer again: back to the whole picture
    }
    if (want == m_regionLayer)
        return true;
    m_regionLayer = want;
    emit layersChanged();
    return true;
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

QList<canvas::LayerPath> CanvasViewModel::topPaths(const QList<int> &positions) {
    QList<canvas::LayerPath> out;
    for (int p : positions)
        out.append(canvas::LayerPath{p});
    return out;
}

QList<canvas::LayerPath> CanvasViewModel::selectedPaths() const {
    QList<canvas::LayerPath> out;
    for (int id : m_selected)
        if (const canvas::LayerPath path = m_outline.pathOf(id); !path.empty())
            out.append(path);
    return out;
}

bool CanvasViewModel::applyEdit(const canvas::LayerEdit &edit, const QList<int> &select) { return applyEditPaths(edit, topPaths(select)); }

bool CanvasViewModel::applyEditPaths(const canvas::LayerEdit &edit, const QList<canvas::LayerPath> &select) {
    const bool namesNoLayer = edit.op == canvas::LayerOp::New || edit.op == canvas::LayerOp::Flatten;
    if (!layersEditable() || (edit.layers.empty() && !namesNoLayer))
        return false;
    commitMask(); // the strokes painted so far are a step of their own, before this one
    if (!layersEditable())
        return false;
    return landPaths(store().editLayers(canvasId(), revisionB(), edit, QStringLiteral("canvas-view:") + canvas::layerOpName(edit.op)), select);
}

bool CanvasViewModel::placePicture(const mask::MaskPaint &picture, double cx, double cy, const QString &name) {
    if (!layersEditable())
        return false;
    commitMask(); // the strokes painted so far are a step of their own, before this one
    if (!layersEditable())
        return false;
    const QSize size = pictureSize();
    // Above the current layer (the last one picked), else on top.
    // (a layer inside a group counts as its group: a picture is placed at the top level.)
    const canvas::LayerPath current = m_selected.isEmpty() ? canvas::LayerPath() : m_outline.pathOf(m_selected.last());
    const int above = current.empty() ? -1 : current.front();
    const int next = above < 0 ? m_outline.topCount() : above + 1;
    return land(store().pasteLayer(canvasId(), revisionB(), picture, cx < 0 ? size.width() / 2.0 : cx, cy < 0 ? size.height() / 2.0 : cy, above, name,
                                   QStringLiteral("canvas-view:paste")),
                {next});
}

Result<canvas::CurrentPlacement> CanvasViewModel::placementOf(int position, QString *why) const {
    why->clear();
    if (!layersEditable()) {
        *why = tr("Go to the latest version to change a layer.");
        return Result<canvas::CurrentPlacement>(makeError(QStringLiteral("input_invalid"), *why));
    }
    const canvas::CanvasStore s = store();
    const Result<canvas::Tree> tree = s.loadTree(canvasId(), revisionB());
    if (!tree) {
        *why = tr("The canvas could not be read.");
        return Result<canvas::CurrentPlacement>(tree.errors());
    }
    if (position >= int(tree->layers.size()) && m_outline.row(position)) {
        *why = tr("This layer is inside a group: moving and transforming work on the layers of the top level for now.");
        return Result<canvas::CurrentPlacement>(makeError(QStringLiteral("input_invalid"), *why));
    }
    if (position < 0 || position >= int(tree->layers.size())) {
        *why = tr("There is no such layer.");
        return Result<canvas::CurrentPlacement>(makeError(QStringLiteral("input_invalid"), *why));
    }
    const canvas::LayerRecord &layer = tree->layers[size_t(position)];
    if (layer.group || layer.kind != mask::LayerKind::Paint) {
        *why = layer.group ? tr("A group cannot be moved or transformed: do it on its layers, or merge it into one layer first.")
                           : tr("Only a paint layer can be moved or transformed.");
        return Result<canvas::CurrentPlacement>(makeError(QStringLiteral("input_invalid"), *why));
    }
    const Result<canvas::CurrentPlacement> placement = canvas::currentPlacement(layer, tree->width, tree->height, [&s](const QString &hash, int w, int h, int c) { return s.tilePixels(hash, w, h, c); });
    if (!placement) {
        *why = tr("%1 has no pixels to move or transform: it is empty.").arg(layer.name);
        return placement;
    }
    return placement;
}

QVariantMap CanvasViewModel::layerBox(int position) const {
    QString why;
    const Result<canvas::CurrentPlacement> placement = placementOf(position, &why);
    if (!placement)
        return QVariantMap{{"valid", false}, {"reason", why}};
    const canvas::LayerTransform &t = placement->transform;
    const double w = std::round(t.width), h = std::round(t.height);
    const double a = t.angle * M_PI / 180.0, c = std::cos(a), sn = std::sin(a);
    QVariantList corners;
    for (const auto &[dx, dy] : {std::pair{-w / 2, -h / 2}, std::pair{w / 2, -h / 2}, std::pair{w / 2, h / 2}, std::pair{-w / 2, h / 2}})
        corners.append(QVariantMap{{"x", t.cx + dx * c - dy * sn}, {"y", t.cy + dx * sn + dy * c}});
    return QVariantMap{{"valid", true}, {"reason", QString()}, {"original", placement->original}, {"x", t.cx - w / 2}, {"y", t.cy - h / 2}, {"width", w}, {"height", h},
                       {"cx", t.cx}, {"cy", t.cy}, {"angle", t.angle}, {"flipX", t.flipX}, {"flipY", t.flipY}, {"filter", canvas::filterName(t.filter)},
                       {"sourceWidth", placement->sourceWidth}, {"sourceHeight", placement->sourceHeight}, {"corners", corners}};
}

bool CanvasViewModel::applyTransform(int position, const canvas::LayerTransform &placement, const canvas::CurrentPlacement &seen) {
    if (!layersEditable())
        return false;
    // `seen` is the placement the caller worked from; the store refuses when the layer is no longer that (a stale guard).
    const canvas::SeenPlacement guard{seen.transform, seen.original, seen.sourceWidth, seen.sourceHeight};
    return land(store().transformLayer(canvasId(), revisionB(), position, placement, QStringLiteral("canvas-view:transform"), &guard), {position});
}

bool CanvasViewModel::beginTransform(int position) {
    commitMask(); // the box is of the revision after any stroke not stored yet
    QString why;
    const Result<canvas::CurrentPlacement> now = placementOf(position, &why);
    if (!now) {
        fail({makeError(QStringLiteral("input_invalid"), why)});
        return false;
    }
    const QString rev = revisionB();
    if (m_xPosition == position && m_xOf == rev)
        return true;
    const canvas::CanvasStore s = store();
    const Result<canvas::Tree> tree = s.loadTree(canvasId(), rev);
    if (!tree)
        return false;
    const canvas::LayerRecord &layer = tree->layers[size_t(position)];
    const QString imagePath = m_folder.filePath(QStringLiteral("xf-layer-%1-%2.png").arg(hexOf(rev)).arg(position));
    const QString basePath = m_folder.filePath(QStringLiteral("xf-base-%1-%2.png").arg(hexOf(rev)).arg(position));
    const auto write = [](const QString &path, const Result<QByteArray> &png) {
        QFile file(path);
        return png && file.open(QIODevice::WriteOnly) && file.write(png.value()) == png->size();
    };
    const canvas::TileReader read = [&s](const QString &hash, int w, int h, int c) { return s.tilePixels(hash, w, h, c); };
    if (!QFile::exists(imagePath)) {
        // The layer's original: its kept pixels while they are current, else its own pixels cut to their bounds.
        Result<mask::MaskPaint> pixels = Result<mask::MaskPaint>(makeError(QStringLiteral("internal"), QStringLiteral("no layer")));
        if (canvas::hasCurrentOriginal(layer)) {
            const canvas::LayerSource &origin = *layer.source;
            pixels = canvas::assembleLayer(origin.width, origin.height, 4, origin.tiles, [&](const canvas::TileKey &key, const QString &hash) {
                const mask::IRect rect = canvas::tileRect(key, origin.width, origin.height);
                return read(hash, rect.width, rect.height, 4);
            });
        } else {
            Result<canvas::LayerContent> content = canvas::contentOf(layer, tree->width, tree->height, read);
            if (!content) {
                fail(content.errors());
                return false;
            }
            pixels = std::move(content->pixels);
        }
        if (!pixels || !write(imagePath, pixels->toPng(1))) {
            fail({makeError(QStringLiteral("internal"), tr("The layer could not be read."))});
            return false;
        }
    }
    if (!QFile::exists(basePath)) {
        Result<mask::PaintDocument> document = s.load(canvasId(), rev);
        if (!document) {
            fail(document.errors());
            return false;
        }
        for (int i = 0; i < document->layerCount(); ++i)
            if (i == position)
                document->setVisible(document->layer(i).id, false);
        if (!write(basePath, document->flattenToPng())) {
            fail({makeError(QStringLiteral("internal"), tr("The picture could not be made."))});
            return false;
        }
    }
    m_xPosition = position;
    m_xOf = rev;
    m_xImage = QUrl::fromLocalFile(imagePath);
    m_xBase = QUrl::fromLocalFile(basePath);
    m_xOpacity = layer.opacity;
    emit transformChanged();
    emit pictureChanged();
    return true;
}

void CanvasViewModel::endTransform() {
    if (m_xPosition < 0)
        return;
    m_xPosition = -1;
    m_xImage = m_xBase = QUrl();
    m_xOf.clear();
    emit transformChanged();
    emit pictureChanged();
}

bool CanvasViewModel::transformLayer(int position, const QVariantMap &spec) {
    commitMask(); // a stroke not stored yet is a revision of its own: the placement below is read from the revision after it
    QString why;
    const Result<canvas::CurrentPlacement> now = placementOf(position, &why);
    if (!now) {
        fail({makeError(QStringLiteral("input_invalid"), why)});
        return false;
    }
    canvas::LayerTransform t = now->transform;
    const auto number = [&spec](const char *key, double *to) {
        if (spec.contains(QLatin1String(key)))
            *to = spec.value(QLatin1String(key)).toDouble();
    };
    number("cx", &t.cx);
    number("cy", &t.cy);
    number("width", &t.width);
    number("height", &t.height);
    number("angle", &t.angle);
    if (spec.contains("flipX"))
        t.flipX = spec.value("flipX").toBool();
    if (spec.contains("flipY"))
        t.flipY = spec.value("flipY").toBool();
    if (spec.contains("filter") && !canvas::filterFromName(spec.value("filter").toString(), &t.filter)) {
        fail({makeError(QStringLiteral("input_invalid"), tr("The resampling is Lanczos, Bicubic or Nearest."))});
        return false;
    }
    if (const QString problem = canvas::transformProblem(t); !problem.isEmpty()) {
        fail({makeError(QStringLiteral("input_invalid"), tr("The layer cannot be placed there: %1").arg(problem))});
        return false;
    }
    return applyTransform(position, canvas::snappedToPixels(t), now.value());
}

bool CanvasViewModel::flipLayer(int position, bool horizontal) {
    commitMask(); // a stroke not stored yet is a revision of its own: the placement below is read from the revision after it
    QString why;
    const Result<canvas::CurrentPlacement> now = placementOf(position, &why);
    if (!now) {
        fail({makeError(QStringLiteral("input_invalid"), why)});
        return false;
    }
    return applyTransform(position, canvas::flippedOnScreen(now->transform, horizontal), now.value());
}

bool CanvasViewModel::turnLayer(int position, bool clockwise) {
    commitMask(); // a stroke not stored yet is a revision of its own: the placement below is read from the revision after it
    QString why;
    const Result<canvas::CurrentPlacement> now = placementOf(position, &why);
    if (!now) {
        fail({makeError(QStringLiteral("input_invalid"), why)});
        return false;
    }
    return applyTransform(position, canvas::turnedQuarter(now->transform, clockwise), now.value());
}

bool CanvasViewModel::moveLayerBy(int position, double dx, double dy) {
    commitMask(); // a stroke not stored yet is a revision of its own: the placement below is read from the revision after it
    QString why;
    const Result<canvas::CurrentPlacement> now = placementOf(position, &why);
    if (!now) {
        fail({makeError(QStringLiteral("input_invalid"), why)});
        return false;
    }
    canvas::LayerTransform t = now->transform;
    t.cx += std::round(dx);
    t.cy += std::round(dy);
    return applyTransform(position, t, now.value());
}

bool CanvasViewModel::clipboardHasPicture() const {
    const QClipboard *board = QGuiApplication::instance() ? QGuiApplication::clipboard() : nullptr;
    return board && board->mimeData() && board->mimeData()->hasImage();
}

bool CanvasViewModel::pasteLayer(double cx, double cy) {
    if (!layersEditable())
        return false;
    const ImportedPicture picture = pictureFromClipboard();
    if (!picture.ok()) {
        fail({makeError(QStringLiteral("input_invalid"), picture.problem)});
        return false;
    }
    return placePicture(picture.pixels, cx, cy, {});
}

bool CanvasViewModel::copyPicture(const QString &what, MaskLayer *selection, int position, double x, double y, double width, double height) {
    if (revisionB().isEmpty())
        return false;
    commitMask(); // a layer-mask stroke not stored yet is part of what is copied
    const Result<mask::PaintDocument> document = store().load(canvasId(), revisionB());
    if (!document) {
        fail(document.errors());
        return false;
    }
    const bool asMask = what == QLatin1String("mask");
    const bool painted = selection && !selection->isEmpty() && selection->imageSize() == QSize(document->width(), document->height());
    const canvas::CopyArea area{painted ? &selection->paint() : nullptr,
                                width > 0 && height > 0 ? mask::IRect{int(std::floor(x)), int(std::floor(y)), int(std::ceil(width)), int(std::ceil(height))}
                                                        : mask::IRect{}};
    QImage image;
    QString named;
    if (asMask) {
        const Result<mask::MaskPaint> picture = canvas::areaAsMask(document->width(), document->height(), area);
        if (!picture) {
            fail(picture.errors());
            return false;
        }
        QImage gray(picture->width(), picture->height(), QImage::Format_RGB888); // opaque, so every node and app reads it as a picture
        for (int row = 0; row < gray.height(); ++row) {
            uchar *out = gray.scanLine(row);
            for (int column = 0; column < gray.width(); ++column)
                out[column * 3] = out[column * 3 + 1] = out[column * 3 + 2] = picture->at(column, row);
        }
        image = gray;
        named = tr("Copied the selection as a mask (%1 × %2)").arg(image.width()).arg(image.height());
    } else {
        const bool merged = what == QLatin1String("merged");
        if (!merged && (position < 0 || position >= m_outline.topCount())) {
            fail({makeError(QStringLiteral("input_invalid"), tr("Pick a layer to copy."), tr("Copy Merged copies every shown layer."))});
            return false;
        }
        const Result<mask::MaskPaint> source = merged ? Result<mask::MaskPaint>(document->flatten()) : canvas::layerPixels(document.value(), position);
        if (!source) {
            fail(source.errors());
            return false;
        }
        const Result<canvas::CopiedPicture> copy = canvas::copyArea(source.value(), area);
        if (!copy) {
            fail(copy.errors());
            return false;
        }
        const mask::MaskPaint &pixels = copy->pixels;
        image = QImage(pixels.width(), pixels.height(), QImage::Format_RGBA8888);
        for (int row = 0; row < image.height(); ++row)
            std::memcpy(image.scanLine(row), pixels.pixels().data() + size_t(row) * size_t(pixels.width()) * 4, size_t(pixels.width()) * 4);
        named = (merged ? tr("Copied merged (%1 × %2)") : tr("Copied (%1 × %2)")).arg(image.width()).arg(image.height());
    }
    QClipboard *board = QGuiApplication::instance() ? QGuiApplication::clipboard() : nullptr;
    if (!board) {
        fail({makeError(QStringLiteral("input_invalid"), tr("There is no clipboard to copy to."))});
        return false;
    }
    board->setImage(image);
    if (!error().isEmpty()) {
        m_error.clear();
        emit errorChanged();
    }
    showNotice(named);
    return true;
}

bool CanvasViewModel::isPictureFile(const QString &pathOrUrl) const { return fugue::isPictureFile(pathOrUrl); }

bool CanvasViewModel::placeFile(const QString &pathOrUrl, double cx, double cy) {
    if (!layersEditable())
        return false;
    const ImportedPicture picture = pictureFromFile(pathOrUrl);
    if (!picture.ok()) {
        fail({makeError(QStringLiteral("input_invalid"), picture.problem)});
        return false;
    }
    const QUrl url(pathOrUrl);
    return placePicture(picture.pixels, cx, cy, QFileInfo(url.isLocalFile() ? url.toLocalFile() : pathOrUrl).completeBaseName());
}

bool CanvasViewModel::land(const Result<canvas::Commit> &done, const QList<int> &select) { return landPaths(done, topPaths(select)); }

bool CanvasViewModel::landPaths(const Result<canvas::Commit> &done, const QList<canvas::LayerPath> &select) {
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

// An edit of layers that sit in one group (the top level is one): `parent` and the places in it. Layers of different groups cannot be
// edited at once (the edit is empty, and `error` says so), as Photoshop's own commands work on one level of the list.
canvas::LayerEdit CanvasViewModel::editOf(canvas::LayerOp op, const QList<int> &positions) {
    canvas::LayerEdit edit;
    edit.op = op;
    canvas::LayerPath parent;
    std::vector<int> at;
    if (positions.isEmpty())
        return edit;
    if (!m_outline.levelOf(std::vector<int>(positions.cbegin(), positions.cend()), &parent, &at)) {
        fail({makeError(QStringLiteral("input_invalid"), tr("Pick layers of one group at a time."), tr("Layers inside different groups are changed one group at a time."))});
        return edit;
    }
    edit.parent = parent;
    edit.layers = at;
    return edit;
}

namespace {

// A step of Properties: the same edit as editOf makes, for the layer of `path`.
canvas::LayerEdit stepAt(canvas::LayerOp op, const canvas::LayerPath &path) {
    canvas::LayerEdit step;
    step.op = op;
    step.parent.assign(path.begin(), path.end() - 1);
    step.layers = {path.back()};
    return step;
}

} // namespace

bool CanvasViewModel::setVisible(const QVariantList &positions, bool visible) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Visible, positionsOf(positions));
    edit.flag = visible;
    return applyEditPaths(edit, selectedPaths());
}

bool CanvasViewModel::setLocked(const QVariantList &positions, bool locked) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Lock, positionsOf(positions));
    edit.flag = locked;
    return applyEditPaths(edit, selectedPaths());
}

bool CanvasViewModel::toggleLocked(const QVariantList &positions) {
    const QList<int> chosen = positionsOf(positions);
    bool anyOpen = false;
    for (const QVariant &row : std::as_const(m_layers))
        if (chosen.contains(row.toMap().value("position").toInt()) && !row.toMap().value("locked").toBool())
            anyOpen = true;
    return setLocked(positions, anyOpen);
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
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Opacity, positionsOf(positions));
    edit.number = std::clamp(opacity, 0.0, 1.0);
    return applyEditPaths(edit, selectedPaths());
}

bool CanvasViewModel::setBlend(const QVariantList &positions, const QString &blend) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Blend, positionsOf(positions));
    edit.text = blend;
    return applyEditPaths(edit, selectedPaths());
}

bool CanvasViewModel::rename(int position, const QString &name) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Rename, {position});
    edit.text = name;
    const canvas::LayerPath path = m_outline.pathOf(position);
    const QString was = m_outline.row(position) ? m_outline.row(position)->name : QString();
    if (!applyEditPaths(edit, {path}))
        return false;
    m_outline.renameOpen(path, was); // an open group stays open under its new name
    buildRows();
    emit layersChanged();
    return true;
}

bool CanvasViewModel::removeLayers(const QVariantList &positions) {
    const QList<int> chosen = positionsOf(positions);
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Remove, chosen);
    if (edit.layers.empty())
        return false;
    // The layer under the lowest one removed is selected next (the bottom one when that was the bottom); the group when none is left.
    const int lowest = *std::min_element(edit.layers.begin(), edit.layers.end());
    const int left = m_outline.levelSize(edit.parent) - int(chosen.size());
    QList<canvas::LayerPath> next;
    if (left > 0) {
        canvas::LayerPath path = edit.parent;
        path.push_back(std::clamp(lowest - 1, 0, left - 1));
        next = {path};
    } else if (!edit.parent.empty()) {
        next = {edit.parent};
    }
    return applyEditPaths(edit, next);
}

bool CanvasViewModel::duplicate(const QVariantList &positions) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Duplicate, positionsOf(positions));
    if (edit.layers.empty())
        return false;
    std::vector<int> chosen = edit.layers;
    std::sort(chosen.begin(), chosen.end());
    QList<canvas::LayerPath> copies; // each copy lands right above its layer, after the copies below it
    for (size_t i = 0; i < chosen.size(); ++i) {
        canvas::LayerPath path = edit.parent;
        path.push_back(chosen[i] + int(i) + 1);
        copies.append(path);
    }
    return applyEditPaths(edit, copies);
}

bool CanvasViewModel::moveLayers(const QVariantList &positions, int to) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Move, positionsOf(positions));
    const int others = m_outline.levelSize(edit.parent) - int(edit.layers.size());
    if (edit.layers.empty() || others < 0)
        return false;
    edit.to = std::clamp(to, 0, others);
    QList<canvas::LayerPath> block;
    for (size_t i = 0; i < edit.layers.size(); ++i) {
        canvas::LayerPath path = edit.parent;
        path.push_back(edit.to + int(i));
        block.append(path);
    }
    return applyEditPaths(edit, block);
}

namespace {

// How many of the layers that are not chosen lie under the lowest chosen one.
int othersBelow(const std::vector<int> &chosen) {
    const int lowest = *std::min_element(chosen.begin(), chosen.end());
    int below = 0;
    for (int p = 0; p < lowest; ++p)
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end())
            ++below;
    return below;
}

} // namespace

bool CanvasViewModel::raise(const QVariantList &positions) {
    const canvas::LayerEdit edit = editOf(canvas::LayerOp::Move, positionsOf(positions));
    return !edit.layers.empty() && moveLayers(positions, othersBelow(edit.layers) + 1);
}

bool CanvasViewModel::lower(const QVariantList &positions) {
    const canvas::LayerEdit edit = editOf(canvas::LayerOp::Move, positionsOf(positions));
    return !edit.layers.empty() && othersBelow(edit.layers) > 0 && moveLayers(positions, othersBelow(edit.layers) - 1);
}

bool CanvasViewModel::dropAt(const QVariantList &positions, int gap) {
    const QList<int> chosen = positionsOf(positions);
    if (chosen.isEmpty())
        return false;
    const canvas::DropPlace place = m_outline.dropAt(std::vector<int>(chosen.cbegin(), chosen.cend()), gap);
    if (!place.ok) {
        fail({makeError(QStringLiteral("input_invalid"), place.why)});
        return false;
    }
    return moveLayers(positions, place.to);
}

bool CanvasViewModel::canDropAt(const QVariantList &positions, int gap) const {
    const QList<int> chosen = positionsOf(positions);
    return !chosen.isEmpty() && m_outline.dropAt(std::vector<int>(chosen.cbegin(), chosen.cend()), gap).ok;
}

bool CanvasViewModel::setMaskEnabled(const QVariantList &positions, bool enabled) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::MaskEnabled, positionsOf(positions));
    edit.flag = enabled;
    return applyEditPaths(edit, selectedPaths());
}

bool CanvasViewModel::removeMask(const QVariantList &positions) { return applyEditPaths(editOf(canvas::LayerOp::RemoveMask, positionsOf(positions)), selectedPaths()); }

bool CanvasViewModel::newLayer() {
    // Above the current layer (in its group); on top when none is current.
    canvas::LayerEdit edit = editOf(canvas::LayerOp::New, {});
    canvas::LayerPath at{m_outline.topCount()};
    if (m_selected.size() == 1 && m_outline.row(m_selected.first())) {
        const canvas::LayerPath current = m_outline.pathOf(m_selected.first());
        edit.parent.assign(current.begin(), current.end() - 1);
        edit.layers = {current.back()};
        at = current;
        ++at.back();
    }
    return applyEditPaths(edit, {at});
}

bool CanvasViewModel::mergeDown(int position) {
    if (position < 1 || position >= m_outline.topCount())
        return false; // Merge down reads pixels, which it does for the top level only (a layer inside a group: ungroup it first)
    return applyEdit(editOf(canvas::LayerOp::MergeDown, {position}), {position - 1});
}

bool CanvasViewModel::flatten() { return applyEdit(editOf(canvas::LayerOp::Flatten, {}), {0}); }

bool CanvasViewModel::groupLayers(const QVariantList &positions) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Group, positionsOf(positions));
    if (edit.layers.empty())
        return false;
    // The group lands where the highest chosen layer was, once the others are out of the way.
    canvas::LayerPath group = edit.parent;
    group.push_back(*std::max_element(edit.layers.begin(), edit.layers.end()) - int(edit.layers.size()) + 1);
    return applyEditPaths(edit, {group});
}

bool CanvasViewModel::ungroup(const QVariantList &positions) {
    canvas::LayerEdit edit = editOf(canvas::LayerOp::Ungroup, positionsOf(positions));
    if (edit.layers.empty())
        return false;
    std::vector<int> chosen = edit.layers;
    std::sort(chosen.begin(), chosen.end());
    // The children take the place of their group: the selection is all of them, counted layer by layer from the bottom.
    QList<canvas::LayerPath> children;
    int shift = 0;
    for (int p = 0; p < m_outline.levelSize(edit.parent); ++p) {
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end())
            continue;
        canvas::LayerPath group = edit.parent;
        group.push_back(p);
        const int held = m_outline.levelSize(group);
        for (int c = 0; c < held; ++c) {
            canvas::LayerPath child = edit.parent;
            child.push_back(p + shift + c);
            children.append(child);
        }
        shift += held - 1;
    }
    return applyEditPaths(edit, children);
}

bool CanvasViewModel::setProperties(int position, const QVariantMap &changes) {
    const canvas::LayerPath path = m_outline.pathOf(position);
    if (path.empty())
        return false;
    canvas::LayerEdit batch;
    batch.op = canvas::LayerOp::Properties;
    batch.layers = {position};
    if (changes.contains("name")) {
        canvas::LayerEdit step = stepAt(canvas::LayerOp::Rename, path);
        step.text = changes.value("name").toString();
        batch.steps.push_back(step);
    }
    if (changes.contains("opacity")) {
        canvas::LayerEdit step = stepAt(canvas::LayerOp::Opacity, path);
        step.number = std::clamp(changes.value("opacity").toDouble(), 0.0, 1.0);
        batch.steps.push_back(step);
    }
    if (changes.contains("blend")) {
        canvas::LayerEdit step = stepAt(canvas::LayerOp::Blend, path);
        step.text = changes.value("blend").toString();
        batch.steps.push_back(step);
    }
    if (changes.value("removeMask").toBool()) {
        batch.steps.push_back(stepAt(canvas::LayerOp::RemoveMask, path));
    } else if (changes.contains("maskEnabled")) {
        canvas::LayerEdit step = stepAt(canvas::LayerOp::MaskEnabled, path);
        step.flag = changes.value("maskEnabled").toBool();
        batch.steps.push_back(step);
    }
    if (batch.steps.empty())
        return false;
    return applyEditPaths(batch, {path});
}

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
    m_keepSelection = selectedPaths();
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
    m_keepSelection = selectedPaths();
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

bool CanvasViewModel::lockedAt(int position) const {
    for (const QVariant &row : std::as_const(m_layers))
        if (row.toMap().value("position").toInt() == position)
            return row.toMap().value("locked").toBool();
    return false;
}

bool CanvasViewModel::insideGroup(int position) {
    if (position < m_outline.topCount() || !m_outline.row(position))
        return false;
    fail({makeError(QStringLiteral("input_invalid"), tr("The mask of a layer inside a group cannot be painted or selected from yet."), tr("Take the layer out of its group (Ungroup) first."))});
    return true;
}

bool CanvasViewModel::targetMask(int position) {
    if (!layersEditable() || insideGroup(position))
        return false;
    if (lockedAt(position)) { // the store refuses the paint when it lands; say so now, before the strokes
        fail({makeError(QStringLiteral("input_invalid"), tr("The layer is locked: unlock it to paint its mask."))});
        return false;
    }
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
        return landPaths(done, selectedPaths());
    m_maskOf = done->revision.id; // the layer holds this revision's mask already: no read back
    return land(done, {m_maskTarget});
}

bool CanvasViewModel::addMask(int position) {
    if (!layersEditable() || position < 0 || !m_outline.row(position) || insideGroup(position))
        return false;
    if (lockedAt(position)) {
        fail({makeError(QStringLiteral("input_invalid"), tr("The layer is locked: unlock it to add a mask."))});
        return false;
    }
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
    const bool written = m_edits->setSelection(m_follow, hash.value(), left, top, right - left + 1, bottom - top + 1);
    updateRegion();
    return written ? QVariantMap{{"ok", true}, {"hash", hash.value()}} : no(tr("The Canvas Region uses this selection already."));
}

bool CanvasViewModel::selectFromLayer(int position, bool fromMask, MaskLayer *selection, const QString &how) {
    if (!selection || insideGroup(position))
        return false;
    const canvas::CanvasStore s = store();
    const Result<canvas::Revision> revision = s.revision(canvasId(), revisionB());
    const Result<canvas::Tree> tree = revision ? s.tree(revision->tree) : Result<canvas::Tree>(revision.errors());
    if (!tree || position < 0 || position >= int(tree->layers.size()))
        return false;
    const canvas::LayerRecord &layer = tree->layers[size_t(position)];
    if (fromMask ? !layer.hasMask : (layer.kind == mask::LayerKind::Mask || layer.tiles.empty())) {
        showNotice(fromMask ? tr("This layer has no mask.") : tr("Only a paint layer has pixels to select."));
        return false;
    }
    const int width = tree->width, height = tree->height, channels = fromMask ? 1 : 4;
    const Result<mask::MaskPaint> pixels = canvas::assembleLayer(width, height, channels, fromMask ? layer.maskTiles : layer.tiles,
                                                                 [&](const canvas::TileKey &key, const QString &hash) {
        const mask::IRect rect = canvas::tileRect(key, width, height);
        return s.tilePixels(hash, rect.width, rect.height, channels);
    });
    if (!pixels) {
        fail(pixels.errors());
        return false;
    }
    if (selection->imageSize() != QSize(width, height))
        selection->setImageSize({width, height});
    if (fromMask)
        return selection->combine(pixels.value(), how);
    std::vector<uint8_t> alpha(size_t(width) * size_t(height));
    for (size_t i = 0; i < alpha.size(); ++i)
        alpha[i] = pixels->pixels()[i * 4 + 3];
    return selection->combine(mask::MaskPaint(width, height, alpha), how);
}

} // namespace fugue
