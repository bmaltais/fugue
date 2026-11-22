// `fugue canvas new|import|export|log|show|list|candidates|accept|drop|diff` (spec 13.1, 14.5, epic #206): the canvas revision store
// from the command line. The work is engine/canvas_store.h's; this reads the arguments and prints.
// A canvas lives in the session's store (a project's own under FUGUE_HOME); `<canvas>` is its id,
// `<canvas>@<revision>` a revision of it (a full hash, the start of one, or `head`).

#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include "cli/commands.h"
#include "engine/atomic_file.h"
#include "engine/canvas_diff.h"
#include "engine/canvas_store.h"
#include "engine/paint_file.h"

namespace fugue::cli {

namespace {

using canvas::CanvasStore;
using mask::LayerKind;

struct Ref {
    QString canvas;
    QString revision; // "head" when none was written
    bool written = false; // an `@revision` was there
};

Ref parseRef(const QString &text) {
    const qsizetype at = text.indexOf(QLatin1Char('@'));
    return at < 0 ? Ref{text, QStringLiteral("head"), false} : Ref{text.left(at), text.mid(at + 1), true};
}

QString kindText(LayerKind kind) {
    return kind == LayerKind::Mask ? QStringLiteral("mask") : kind == LayerKind::Picture ? QStringLiteral("picture") : QStringLiteral("paint");
}

QJsonObject revisionJson(const canvas::Revision &revision) {
    return QJsonObject{{"revision", revision.id},
                       {"tree", revision.tree},
                       {"parent", revision.parent.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(revision.parent)},
                       {"producer", revision.producer},
                       {"time", revision.time}};
}

QJsonObject canvasJson(const canvas::CanvasInfo &info) {
    return QJsonObject{{"canvas", info.id}, {"name", info.name}, {"created", info.created}, {"head", info.head}};
}

Error usageError(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("usage"), message, hint); }

// The first revision of a canvas just made, as the commands answer it.
Reply madeReply(const canvas::CanvasInfo &info, const QString &verb, int width, int height) {
    QJsonObject data = canvasJson(info);
    data.insert(QStringLiteral("width"), width);
    data.insert(QStringLiteral("height"), height);
    return Reply::success(data, QStringLiteral("%1 canvas %2 (%3 x %4), head %5.").arg(verb, info.id).arg(width).arg(height).arg(info.head));
}

} // namespace

Reply canvasNew(const Command &command, Context &) {
    int width = 1024, height = 1024;
    if (command.has("size")) {
        static const QRegularExpression pattern(QStringLiteral("^([0-9]{1,5})x([0-9]{1,5})$"));
        const QRegularExpressionMatch match = pattern.match(command.value("size"));
        if (!match.hasMatch())
            return Reply::failure(usageError(QStringLiteral("--size is <width>x<height>, such as 1024x768."), command.value("size")));
        width = match.captured(1).toInt();
        height = match.captured(2).toInt();
        if (width < 1 || height < 1 || width > mask::paintFileMaxSide || height > mask::paintFileMaxSide || qint64(width) * height > mask::paintFileMaxPixels)
            return Reply::failure(makeError(QStringLiteral("input_invalid"), QStringLiteral("A canvas is at most %1 px a side and %2 megapixels.").arg(mask::paintFileMaxSide).arg(mask::paintFileMaxPixels / 1000000)));
    }
    mask::PaintDocument document(width, height);
    document.addLayer(LayerKind::Paint, QStringLiteral("Layer 1"));
    CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasInfo> made = store.create(command.has("name") ? command.value("name") : QStringLiteral("Untitled"), document, QStringLiteral("new"));
    if (!made)
        return Reply::failure(made.errors());
    return madeReply(made.value(), QStringLiteral("Made"), width, height);
}

Reply canvasImport(const Command &command, Context &) {
    const QString file = command.positionals.at(0);
    const Result<mask::PaintDocument> document = mask::readPaintFile(file);
    if (!document)
        return Reply::failure(document.errors());
    CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasInfo> made =
        store.create(command.has("name") ? command.value("name") : QFileInfo(file).completeBaseName(), document.value(), QStringLiteral("import"));
    if (!made)
        return Reply::failure(made.errors());
    return madeReply(made.value(), QStringLiteral("Imported"), document->width(), document->height());
}

Reply canvasExport(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    const CanvasStore store = CanvasStore::forSession();
    const Result<QString> revision = store.resolve(ref.canvas, ref.revision);
    if (!revision)
        return Reply::failure(revision.errors());
    const Result<mask::PaintDocument> document = store.load(ref.canvas, revision.value());
    if (!document)
        return Reply::failure(document.errors());
    const Result<QByteArray> bytes = mask::paintToOra(document.value());
    if (!bytes)
        return Reply::failure(bytes.errors());
    const QString out = command.positionals.at(1);
    const Errors written = createFile(out, bytes.value(), QStringLiteral("the exported canvas"));
    if (hasErrors(written))
        return Reply::failure(written);
    return Reply::success(QJsonObject{{"canvas", ref.canvas}, {"revision", revision.value()}, {"path", QFileInfo(out).absoluteFilePath()}},
                          QStringLiteral("Exported %1 revision %2 to %3.").arg(ref.canvas, revision.value(), out));
}

Reply canvasLog(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    int limit = 20;
    if (command.has("limit")) {
        bool ok = false;
        limit = command.value("limit").toInt(&ok);
        if (!ok || limit < 1)
            return Reply::failure(usageError(QStringLiteral("--limit is a whole number of at least 1."), command.value("limit")));
    }
    const CanvasStore store = CanvasStore::forSession();
    const Result<std::vector<canvas::Revision>> entries = store.log(ref.canvas, ref.revision, limit);
    if (!entries)
        return Reply::failure(entries.errors());
    QJsonArray listed;
    QStringList lines;
    for (const canvas::Revision &entry : entries.value()) {
        listed.append(revisionJson(entry));
        lines << QStringLiteral("%1  %2  %3").arg(entry.id.left(15), entry.time, entry.producer);
    }
    return Reply::success(QJsonObject{{"canvas", ref.canvas}, {"revisions", listed}}, lines.join('\n'));
}

Reply canvasShow(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    const CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasInfo> info = store.info(ref.canvas);
    if (!info)
        return Reply::failure(info.errors());
    const Result<QString> id = store.resolve(ref.canvas, ref.revision);
    if (!id)
        return Reply::failure(id.errors());
    const Result<canvas::Revision> revision = store.revision(ref.canvas, id.value());
    if (!revision)
        return Reply::failure(revision.errors());
    const Result<canvas::Tree> tree = store.tree(revision->tree);
    if (!tree)
        return Reply::failure(tree.errors());
    QJsonArray layers;
    QStringList lines{QStringLiteral("%1 (%2), revision %3%4").arg(info->name, info->id, revision->id, id.value() == info->head ? QStringLiteral(" (head)") : QString()),
                      QStringLiteral("  %1 x %2, tree %3").arg(tree->width).arg(tree->height).arg(revision->tree),
                      QStringLiteral("  parent %1, by %2 at %3").arg(revision->parent.isEmpty() ? QStringLiteral("none") : revision->parent, revision->producer, revision->time)};
    for (const canvas::LayerRecord &layer : tree->layers) {
        layers.append(QJsonObject{{"name", layer.name},
                                  {"kind", kindText(layer.kind)},
                                  {"visible", layer.visible},
                                  {"opacity", layer.opacity},
                                  {"tiles", int(layer.tiles.size())}});
        lines << QStringLiteral("  %1: %2%3, opacity %4, %5 tiles").arg(layer.name, kindText(layer.kind), layer.visible ? QString() : QStringLiteral(", hidden")).arg(layer.opacity).arg(layer.tiles.size());
    }
    QJsonObject data = revisionJson(revision.value());
    data.insert(QStringLiteral("canvas"), info->id);
    data.insert(QStringLiteral("name"), info->name);
    data.insert(QStringLiteral("head"), info->head);
    data.insert(QStringLiteral("is_head"), id.value() == info->head);
    data.insert(QStringLiteral("width"), tree->width);
    data.insert(QStringLiteral("height"), tree->height);
    data.insert(QStringLiteral("layers"), layers);
    return Reply::success(data, lines.join('\n'));
}

namespace {

QJsonObject rectJson(const mask::IRect &rect) {
    return QJsonObject{{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}};
}

QString rectText(const mask::IRect &rect) { return QStringLiteral("%1,%2 %3x%4").arg(rect.x).arg(rect.y).arg(rect.width).arg(rect.height); }

QJsonObject candidateJson(const canvas::Candidate &candidate) {
    QJsonObject data = revisionJson(candidate.revision);
    data.insert(QStringLiteral("on_head"), candidate.onHead);
    data.insert(QStringLiteral("dropped"), candidate.dropped);
    return data;
}

// The revision `accept` and `drop` mean: the one written, or the only open candidate when none was.
Result<QString> candidateRef(const CanvasStore &store, const Ref &ref) {
    if (ref.written)
        return store.resolve(ref.canvas, ref.revision);
    const Result<std::vector<canvas::Candidate>> open = store.candidates(ref.canvas);
    if (!open)
        return Result<QString>(open.errors());
    if (open->size() == 1)
        return open->front().revision.id;
    return Result<QString>(usageError(open->empty() ? QStringLiteral("Canvas %1 has no candidate.").arg(ref.canvas)
                                                    : QStringLiteral("Canvas %1 has %2 candidates; name one as %1@<revision>.").arg(ref.canvas).arg(open->size()),
                                      QStringLiteral("`fugue canvas candidates %1` lists them.").arg(ref.canvas)));
}

} // namespace

Reply canvasList(const Command &, Context &) {
    const CanvasStore store = CanvasStore::forSession();
    QJsonArray listed;
    QStringList lines;
    for (const canvas::CanvasInfo &info : store.list()) {
        listed.append(canvasJson(info));
        lines << QStringLiteral("%1  %2  %3").arg(info.id, info.created, info.name);
    }
    if (lines.isEmpty())
        lines << QStringLiteral("No canvas yet: `fugue canvas new` or `fugue canvas import` makes one.");
    return Reply::success(QJsonObject{{"canvases", listed}}, lines.join('\n'));
}

Reply canvasCandidates(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    if (ref.written)
        return Reply::failure(usageError(QStringLiteral("candidates takes a canvas id, not a revision.")));
    const CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasInfo> info = store.info(ref.canvas);
    if (!info)
        return Reply::failure(info.errors());
    const Result<std::vector<canvas::Candidate>> found = store.candidates(ref.canvas, command.has("all"));
    if (!found)
        return Reply::failure(found.errors());
    QJsonArray listed;
    QStringList lines;
    for (const canvas::Candidate &candidate : found.value()) {
        listed.append(candidateJson(candidate));
        lines << QStringLiteral("%1  %2  %3%4%5").arg(candidate.revision.id.left(15), candidate.revision.time, candidate.revision.producer,
                                                      candidate.onHead ? QString() : QStringLiteral("  (made from an older revision)"),
                                                      candidate.dropped ? QStringLiteral("  (dropped)") : QString());
    }
    if (lines.isEmpty())
        lines << QStringLiteral("Canvas %1 has no candidate.").arg(ref.canvas);
    return Reply::success(QJsonObject{{"canvas", ref.canvas}, {"head", info->head}, {"candidates", listed}}, lines.join('\n'));
}

Reply canvasAccept(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    CanvasStore store = CanvasStore::forSession();
    const Result<QString> id = candidateRef(store, ref);
    if (!id)
        return Reply::failure(id.errors());
    const Result<canvas::Accepted> done = store.accept(ref.canvas, id.value());
    if (!done)
        return Reply::failure(done.errors());
    if (!done->moved)
        return Reply::failure(makeError(QStringLiteral("canvas_head_moved"), QStringLiteral("The head of %1 moved while this ran (it is %2 now); nothing was changed.").arg(ref.canvas, done->current),
                                        QStringLiteral("Run it again.")));
    QJsonObject data = revisionJson(done->head);
    data.insert(QStringLiteral("canvas"), ref.canvas);
    data.insert(QStringLiteral("accepted"), id.value());
    data.insert(QStringLiteral("rebased"), done->rebased);
    return Reply::success(data, done->rebased ? QStringLiteral("Placed the layers of %1 on the head of %2: new head %3.").arg(id.value().left(15), ref.canvas, done->head.id)
                                              : QStringLiteral("Accepted %1: the head of %2 is %3.").arg(id.value().left(15), ref.canvas, done->head.id));
}

Reply canvasDrop(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    CanvasStore store = CanvasStore::forSession();
    const Result<QString> id = candidateRef(store, ref);
    if (!id)
        return Reply::failure(id.errors());
    const Result<canvas::Revision> dropped = store.drop(ref.canvas, id.value());
    if (!dropped)
        return Reply::failure(dropped.errors());
    QJsonObject data = revisionJson(dropped.value());
    data.insert(QStringLiteral("canvas"), ref.canvas);
    return Reply::success(data, QStringLiteral("Dropped %1 from %2 (it is kept, and `fugue canvas accept` can still take it).").arg(id.value().left(15), ref.canvas));
}

Reply canvasDiff(const Command &command, Context &) {
    const Ref first = parseRef(command.positionals.at(0));
    const CanvasStore store = CanvasStore::forSession();
    const Result<QString> aId = store.resolve(first.canvas, first.revision);
    if (!aId)
        return Reply::failure(aId.errors());
    const Result<canvas::Revision> a = store.revision(first.canvas, aId.value());
    if (!a)
        return Reply::failure(a.errors());
    // One argument: against its parent. Two: any two revisions, of one canvas or of two.
    Ref second = first;
    QString bId;
    if (command.positionals.size() > 1) {
        const QString text = command.positionals.at(1);
        // `@rev` or a bare hash name a revision of the first one's canvas.
        second = text.startsWith(QLatin1Char('@')) ? Ref{first.canvas, text.mid(1), true}
                 : text.startsWith(QLatin1String("sha256:")) ? Ref{first.canvas, text, true}
                                                              : parseRef(text);
        const Result<QString> resolved = store.resolve(second.canvas, second.revision);
        if (!resolved)
            return Reply::failure(resolved.errors());
        bId = resolved.value();
    } else {
        if (a->parent.isEmpty())
            return Reply::failure(usageError(QStringLiteral("Revision %1 is the first one and has no parent; name a second revision to compare with.").arg(aId.value().left(19))));
        bId = aId.value();
        second = first;
    }
    // With one argument the older side is the parent; with two, `a` is the base and `b` the new one.
    const QString baseId = command.positionals.size() > 1 ? aId.value() : a->parent;
    const Result<canvas::Revision> base = store.revision(first.canvas, baseId);
    const Result<canvas::Revision> next = store.revision(second.canvas, bId);
    if (!base || !next)
        return Reply::failure(!base ? base.errors() : next.errors());
    const Result<canvas::Tree> before = store.tree(base->tree), after = store.tree(next->tree);
    if (!before || !after)
        return Reply::failure(!before ? before.errors() : after.errors());
    const canvas::TreeDiff diff = canvas::diffTrees(before.value(), after.value());
    QJsonArray layers;
    QStringList lines{QStringLiteral("%1 -> %2").arg(base->id.left(15), next->id.left(15))};
    for (const canvas::LayerDiff &layer : diff.layers) {
        layers.append(QJsonObject{{"name", layer.name},
                                  {"change", canvas::changeName(layer.change)},
                                  {"tiles", layer.tilesChanged},
                                  {"bounds", rectJson(layer.bounds)},
                                  {"properties", QJsonArray::fromStringList(layer.properties)}});
        QString line = QStringLiteral("  %1: %2").arg(layer.name, canvas::changeName(layer.change));
        if (layer.tilesChanged > 0)
            line += QStringLiteral(", %1 tiles in %2").arg(layer.tilesChanged).arg(rectText(layer.bounds));
        if (!layer.properties.isEmpty())
            line += QStringLiteral(", %1").arg(layer.properties.join(QStringLiteral(", ")));
        lines << line;
    }
    if (diff.sizeChanged)
        lines << QStringLiteral("  size: %1 x %2 -> %3 x %4").arg(diff.widthA).arg(diff.heightA).arg(diff.widthB).arg(diff.heightB);
    if (diff.identical())
        lines << QStringLiteral("  no difference");
    return Reply::success(QJsonObject{{"from", base->id},
                                      {"to", next->id},
                                      {"identical", diff.identical()},
                                      {"size_changed", diff.sizeChanged},
                                      {"bounds", rectJson(diff.bounds())},
                                      {"layers", layers}},
                          lines.join('\n'));
}

} // namespace fugue::cli
