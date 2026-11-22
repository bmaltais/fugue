#include "canvas/canvas_history.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>

#include "canvas/canvas_choices.h"
#include "engine/atomic_file.h"
#include "engine/canvas_diff.h"

namespace fugue {

using canvas::CanvasStore;

namespace {

QString shortOf(const QString &revision) { return revision.mid(QStringLiteral("sha256:").size(), 8); }

// The one place a revision is named for a person (#228, the cohesion audit's C2): the strip's chips, the picture's label,
// History's rows and the menus all read `name` and `tag` from here. Head is capitalized as a name (D3).
QVariantMap revisionRow(const QString &id, const QString &time, const QString &producer, const QString &kind, bool onHead) {
    const QString hash = shortOf(id);
    const QString name = kind == QLatin1String("head") ? QObject::tr("Head")
                       : kind == QLatin1String("candidate") ? QObject::tr("Candidate %1").arg(hash)
                       : kind == QLatin1String("dropped") ? QObject::tr("Dropped %1").arg(hash) : hash;
    const QString tag = kind == QLatin1String("head") ? QObject::tr("Head")
                      : kind == QLatin1String("candidate") ? QObject::tr("Candidate")
                      : kind == QLatin1String("dropped") ? QObject::tr("Dropped") : QString();
    return QVariantMap{{"id", id}, {"short", hash}, {"time", time}, {"producer", producer}, {"kind", kind}, {"onHead", onHead},
                       {"name", name}, {"tag", tag}};
}

QString rectText(const mask::IRect &rect) { return QStringLiteral("%1, %2 · %3 × %4").arg(rect.x).arg(rect.y).arg(rect.width).arg(rect.height); }

QString describe(const canvas::LayerDiff &layer) {
    QString line;
    switch (layer.change) {
    case canvas::LayerChange::Added: line = QObject::tr("Layer %1 added").arg(layer.name); break;
    case canvas::LayerChange::Removed: line = QObject::tr("Layer %1 removed").arg(layer.name); break;
    case canvas::LayerChange::Changed: line = QObject::tr("Layer %1 changed").arg(layer.name); break;
    }
    if (layer.tilesChanged > 0)
        line += QStringLiteral(": ") + QObject::tr("%n tiles in %1", nullptr, layer.tilesChanged).arg(rectText(layer.bounds));
    if (!layer.properties.isEmpty())
        line += QStringLiteral(" (%1)").arg(layer.properties.join(QStringLiteral(", ")));
    return line;
}

} // namespace

CanvasStore CanvasHistory::store() const { return m_root.isEmpty() ? CanvasStore::forSession() : CanvasStore(m_root); }

void CanvasHistory::setStoreRoot(const QString &root) {
    if (root == m_root)
        return;
    m_root = root;
    emit storeChanged();
    reload();
}

void CanvasHistory::setCanvasId(const QString &id) {
    if (id == m_canvas)
        return;
    m_canvas = id;
    emit canvasChanged();
    reload();
}

QVariantList CanvasHistory::canvases() const { return canvasRows(store()); }

QString CanvasHistory::kindB() const {
    for (const QVariant &row : m_revisions)
        if (row.toMap().value("id").toString() == m_b)
            return row.toMap().value("kind").toString();
    return {};
}

QString CanvasHistory::exportName() const {
    if (m_b.isEmpty())
        return {};
    const Result<canvas::CanvasInfo> info = store().info(m_canvas);
    const QString name = info ? info->name.trimmed() : QString();
    QString safe = name;
    safe.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
    return (safe.isEmpty() ? m_canvas : safe) + QLatin1Char('-') + shortOf(m_b) + QStringLiteral(".png");
}

bool CanvasHistory::exportPicture(const QString &target) {
    if (m_b.isEmpty())
        return false;
    QString path = target.startsWith(QStringLiteral("file:")) ? QUrl(target).toLocalFile() : target;
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().toLower() != QLatin1String("png"))
        path += QStringLiteral(".png");
    if (!m_error.isEmpty()) {
        m_error.clear();
        emit errorChanged();
    }
    const Result<mask::PaintDocument> document = store().load(m_canvas, m_b);
    if (!document) {
        fail(document.errors());
        return false;
    }
    const Result<QByteArray> png = document->flattenToPng(); // the same flatten the viewer's picture is made with
    if (!png) {
        fail(png.errors());
        return false;
    }
    const Errors written = createFile(path, png.value(), tr("the picture of revision %1").arg(shortOf(m_b)));
    if (hasErrors(written)) {
        fail(written);
        return false;
    }
    m_notice = tr("Saved %1").arg(QFileInfo(path).fileName());
    emit noticeChanged();
    return true;
}

void CanvasHistory::fail(const Errors &errors) {
    QString text;
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error) {
            text = error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint;
            break;
        }
    m_error = text;
    emit errorChanged();
}

void CanvasHistory::clearCompare() {
    m_a.clear();
    m_b.clear();
    m_sourceA = m_sourceB = m_difference = QUrl();
    m_sizeA = m_sizeB = QSize();
    m_summary.clear();
    emit compareChanged();
}

void CanvasHistory::reload() {
    if (!m_error.isEmpty()) {
        m_error.clear();
        emit errorChanged();
    }
    m_revisions.clear();
    m_head.clear();
    if (m_canvas.isEmpty()) {
        emit revisionsChanged();
        clearCompare();
        return;
    }
    const CanvasStore s = store();
    const Result<canvas::CanvasInfo> info = s.info(m_canvas);
    if (!info) {
        fail(info.errors());
        emit revisionsChanged();
        clearCompare();
        return;
    }
    m_head = info->head;
    const Result<std::vector<canvas::Candidate>> candidates = s.candidates(m_canvas, true);
    if (!candidates) {
        fail(candidates.errors());
    } else {
        for (const canvas::Candidate &candidate : candidates.value()) {
            const QString kind = candidate.dropped ? QStringLiteral("dropped") : QStringLiteral("candidate");
            m_revisions.append(revisionRow(candidate.revision.id, candidate.revision.time, candidate.revision.producer, kind, candidate.onHead));
        }
    }
    const Result<std::vector<canvas::Revision>> history = info->head.isEmpty() ? Result<std::vector<canvas::Revision>>(std::vector<canvas::Revision>{})
                                                                              : s.log(m_canvas, QStringLiteral("head"), 500);
    if (!history) {
        fail(history.errors());
    } else {
        for (const canvas::Revision &revision : history.value())
            m_revisions.append(revisionRow(revision.id, revision.time, revision.producer,
                                           revision.id == m_head ? QStringLiteral("head") : QStringLiteral("history"), false));
    }
    emit revisionsChanged();
    // Keep the comparison when both of its revisions are still there, else start from the head.
    const auto known = [this](const QString &id) {
        return std::any_of(m_revisions.begin(), m_revisions.end(), [&id](const QVariant &row) { return row.toMap().value("id").toString() == id; });
    };
    if (!m_a.isEmpty() && !m_b.isEmpty() && known(m_a) && known(m_b))
        compare(m_a, m_b);
    else if (!m_head.isEmpty())
        select(m_head);
    else
        clearCompare();
}

bool CanvasHistory::picture(const QString &revision, QUrl *url, QSize *size) {
    const QString path = m_folder.filePath(revision.mid(QStringLiteral("sha256:").size()) + QStringLiteral(".png"));
    const Result<mask::PaintDocument> document = store().load(m_canvas, revision);
    if (!document) {
        fail(document.errors());
        return false;
    }
    *size = QSize(document->width(), document->height());
    if (!QFile::exists(path)) {
        const Result<QByteArray> png = document->flattenToPng();
        if (!png) {
            fail(png.errors());
            return false;
        }
        QFile file(path);
        if (!m_folder.isValid() || !file.open(QIODevice::WriteOnly) || file.write(png.value()) != png->size()) {
            fail({makeError(QStringLiteral("io_error"), tr("The picture of revision %1 could not be written.").arg(shortOf(revision)))});
            return false;
        }
    }
    *url = QUrl::fromLocalFile(path);
    return true;
}

void CanvasHistory::select(const QString &revision) {
    const Result<canvas::Revision> found = store().revision(m_canvas, revision);
    if (!found) {
        fail(found.errors());
        return;
    }
    // A candidate (or a dropped one) is compared with the head it would change, as the Canvas view does; any other
    // revision with its parent. One meaning of Compare in every window (the cohesion audit's D1).
    for (const QVariant &row : m_revisions) {
        const QVariantMap map = row.toMap();
        const QString kind = map.value(QStringLiteral("kind")).toString();
        if (map.value(QStringLiteral("id")).toString() == revision && !m_head.isEmpty() && revision != m_head
            && (kind == QLatin1String("candidate") || kind == QLatin1String("dropped"))) {
            compare(m_head, revision);
            return;
        }
    }
    compare(found->parent.isEmpty() ? revision : found->parent, revision);
}

void CanvasHistory::setA(const QString &revision) { compare(revision, m_b.isEmpty() ? m_head : m_b); }

void CanvasHistory::compare(const QString &a, const QString &b) {
    const CanvasStore s = store();
    const Result<canvas::Revision> ra = s.revision(m_canvas, a), rb = s.revision(m_canvas, b);
    if (!ra || !rb) {
        fail(!ra ? ra.errors() : rb.errors());
        return;
    }
    const Result<canvas::Tree> ta = s.tree(ra->tree), tb = s.tree(rb->tree);
    if (!ta || !tb) {
        fail(!ta ? ta.errors() : tb.errors());
        return;
    }
    QUrl urlA, urlB;
    QSize sizeA, sizeB;
    if (!picture(a, &urlA, &sizeA) || !picture(b, &urlB, &sizeB))
        return;
    m_a = a;
    m_b = b;
    m_sourceA = urlA;
    m_sourceB = urlB;
    m_sizeA = sizeA;
    m_sizeB = sizeB;
    m_difference = QUrl();
    if (!m_notice.isEmpty()) {
        m_notice.clear();
        emit noticeChanged();
    }
    // |A - B| as a picture of its own, when they are the same size (CompareView's difference mode).
    if (sizeA == sizeB && a != b) {
        QImage first(urlA.toLocalFile()), second(urlB.toLocalFile());
        if (!first.isNull() && first.size() == second.size()) {
            first = first.convertToFormat(QImage::Format_ARGB32);
            second = second.convertToFormat(QImage::Format_ARGB32);
            QImage out(first.size(), QImage::Format_ARGB32);
            for (int y = 0; y < out.height(); ++y) {
                const QRgb *p = reinterpret_cast<const QRgb *>(first.constScanLine(y));
                const QRgb *q = reinterpret_cast<const QRgb *>(second.constScanLine(y));
                QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
                for (int x = 0; x < out.width(); ++x)
                    o[x] = qRgba(qAbs(qRed(p[x]) - qRed(q[x])), qAbs(qGreen(p[x]) - qGreen(q[x])), qAbs(qBlue(p[x]) - qBlue(q[x])), 255);
            }
            const QString path = m_folder.filePath(QStringLiteral("diff-") + shortOf(a) + QLatin1Char('-') + shortOf(b) + QStringLiteral(".png"));
            if (out.save(path, "PNG"))
                m_difference = QUrl::fromLocalFile(path);
        }
    }
    canvas::TreeDiff diff = canvas::diffTrees(ta.value(), tb.value());
    canvas::tightenBounds(&diff, ta.value(), tb.value(), [&](const QString &h, int w, int ht, int c) { return s.tilePixels(h, w, ht, c); });
    m_summary.clear();
    for (const canvas::LayerDiff &layer : diff.layers)
        m_summary << describe(layer);
    if (diff.sizeChanged)
        m_summary << tr("Size %1 × %2 to %3 × %4").arg(diff.widthA).arg(diff.heightA).arg(diff.widthB).arg(diff.heightB);
    if (diff.identical())
        m_summary << tr("No difference");
    emit compareChanged();
}

bool CanvasHistory::accept(const QString &revision) {
    const Result<canvas::Accepted> done = store().accept(m_canvas, revision);
    if (!done) {
        fail(done.errors());
        return false;
    }
    if (!done->moved) {
        fail({makeError(QStringLiteral("canvas_head_moved"), tr("The canvas changed meanwhile; nothing was changed."), tr("Look at it again and accept once more."))});
        reload();
        return false;
    }
    m_a.clear();
    m_b.clear();
    reload();
    return true;
}

bool CanvasHistory::drop(const QString &revision) {
    const Result<canvas::Revision> dropped = store().drop(m_canvas, revision);
    if (!dropped) {
        fail(dropped.errors());
        return false;
    }
    reload();
    return true;
}

QString CanvasHistory::newCanvas(const QString &name, int width, int height) {
    CanvasChoices choices;
    choices.setStoreRoot(m_root);
    QString id;
    if (const QString why = choices.makeBlank(name, width, height, &id); !why.isEmpty())
        return why;
    emit storeChanged();
    setCanvasId(id);
    return {};
}

QString CanvasHistory::importPicture(const QString &path, const QString &name) {
    CanvasChoices choices;
    choices.setStoreRoot(m_root);
    QString id;
    if (const QString why = choices.makeFromFile(path, name, &id); !why.isEmpty())
        return why;
    emit storeChanged();
    setCanvasId(id);
    return {};
}

QString CanvasHistory::pictureSize(const QString &path) const { return CanvasChoices().pictureSize(path); }

} // namespace fugue
