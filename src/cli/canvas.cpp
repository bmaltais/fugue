// `fugue canvas new|import|export|log|show` (spec 13.1, 14.5, epic #206): the canvas revision store
// from the command line. The work is engine/canvas_store.h's; this reads the arguments and prints.
// A canvas lives in the session's store (a project's own under FUGUE_HOME); `<canvas>` is its id,
// `<canvas>@<revision>` a revision of it (a full hash, the start of one, or `head`).

#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include "cli/commands.h"
#include "engine/atomic_file.h"
#include "engine/canvas_store.h"
#include "engine/paint_file.h"

namespace fugue::cli {

namespace {

using canvas::CanvasStore;
using mask::LayerKind;

struct Ref {
    QString canvas;
    QString revision; // "head" when none was written
};

Ref parseRef(const QString &text) {
    const qsizetype at = text.indexOf(QLatin1Char('@'));
    return at < 0 ? Ref{text, QStringLiteral("head")} : Ref{text.left(at), text.mid(at + 1)};
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

} // namespace fugue::cli
