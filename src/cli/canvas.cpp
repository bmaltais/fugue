// `fugue canvas new|import|export|log|show|list|candidates|accept|drop|diff|layer|undo|redo` (spec 13.1, 14.5, epic #206): the canvas revision store
// from the command line. The work is engine/canvas_store.h's; this reads the arguments and prints.
// A canvas lives in the session's store (a project's own under FUGUE_HOME); `<canvas>` is its id,
// `<canvas>@<revision>` a revision of it (a full hash, the start of one, or `head`).

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include "cli/commands.h"
#include "engine/atomic_file.h"
#include "engine/canvas_diff.h"
#include "engine/canvas_export.h"
#include "engine/canvas_layer_ops.h"
#include "engine/canvas_package.h"
#include "engine/canvas_resize.h"
#include "engine/canvas_store.h"
#include "engine/new_canvas.h"
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

QString blendText(const QString &compositeOp) {
    bool known = true;
    const mask::Blend blend = mask::blendOfOp(compositeOp, &known);
    return known ? mask::blendId(blend) : compositeOp;
}

// One layer as `show` and `layer list` give it (position 0 is the bottom).
QJsonObject layerJson(const canvas::LayerRecord &layer, int position) {
    QJsonObject out{{"position", position},
                    {"name", layer.name},
                    {"kind", layer.group ? QStringLiteral("group") : kindText(layer.kind)},
                    {"visible", layer.visible},
                    {"opacity", layer.opacity},
                    {"blend", blendText(layer.compositeOp)},
                    {"tiles", layer.tileCount()}};
    if (layer.group) {
        QJsonArray children;
        for (const canvas::LayerRecord &child : layer.children)
            children.append(child.name);
        out.insert(QStringLiteral("children"), children);
    }
    if (layer.hasMask)
        out.insert(QStringLiteral("mask"), QJsonObject{{"enabled", layer.maskEnabled}, {"tiles", int(layer.maskTiles.size())}});
    if (!layer.producer.isEmpty())
        out.insert(QStringLiteral("producer"), layer.producer);
    if (!layer.created.isEmpty())
        out.insert(QStringLiteral("created"), layer.created);
    return out;
}

QString layerLine(const canvas::LayerRecord &layer, int position) {
    QString line = QStringLiteral("%1 %2: %3%4, opacity %5, %6 tiles").arg(position).arg(layer.name, layer.group ? QStringLiteral("group of %1").arg(layer.children.size()) : kindText(layer.kind),
                                                                           layer.visible ? QString() : QStringLiteral(", hidden"))
                       .arg(layer.opacity).arg(layer.tileCount());
    if (!layer.compositeOp.isEmpty())
        line += QStringLiteral(", blend %1").arg(blendText(layer.compositeOp));
    if (layer.hasMask)
        line += layer.maskEnabled ? QStringLiteral(", layer mask") : QStringLiteral(", layer mask off");
    if (!layer.producer.isEmpty())
        line += QStringLiteral(", from %1%2").arg(layer.producer, layer.created.isEmpty() ? QString() : QStringLiteral(" at ") + layer.created);
    return line;
}

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
        if (const QString why = mask::canvasSizeProblem(width, height); !why.isEmpty())
            return Reply::failure(makeError(QStringLiteral("input_invalid"), why));
    }
    const Result<mask::CanvasBackground> background = mask::parseBackground(command.has("background") ? command.value("background") : QString());
    if (!background)
        return Reply::failure(background.errors());
    const Result<mask::PaintDocument> document = mask::blankCanvas(width, height, background.value());
    if (!document)
        return Reply::failure(document.errors());
    CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasInfo> made = store.create(command.has("name") ? command.value("name") : QStringLiteral("Untitled"), document.value(), QStringLiteral("new"));
    if (!made)
        return Reply::failure(made.errors());
    return madeReply(made.value(), QStringLiteral("Made"), width, height);
}

// `canvas import <file.fuguepack>`: the canvas of a package into the store, never over a canvas there.
Reply importPackage(const Command &command, const QString &file) {
    CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasImported> got = canvas::importCanvasPackage(file, store, command.has("name") ? command.value("name") : QString());
    if (!got)
        return Reply::failure(got.errors());
    QJsonObject data = canvasJson(got->info);
    data.insert(QStringLiteral("part"), got->part == canvas::SharedPart::History ? QStringLiteral("history") : QStringLiteral("head"));
    data.insert(QStringLiteral("revisions"), got->revisions);
    data.insert(QStringLiteral("renamed"), got->renamedId);
    QString text = QStringLiteral("Imported canvas %1 as %2 (%3).").arg(got->info.name, got->info.id,
                                                                     got->part == canvas::SharedPart::History ? QStringLiteral("with its history, %1 revisions").arg(got->revisions) : QStringLiteral("the head only"));
    if (got->renamedId) {
        data.insert(QStringLiteral("packagedId"), got->packagedId);
        text += QStringLiteral(" The package's id %1 is taken here, so it has a new one; the canvas that was here is untouched.").arg(got->packagedId);
    }
    return Reply::success(data, text);
}

Reply canvasImport(const Command &command, Context &) {
    const QString file = command.positionals.at(0);
    if (canvas::isCanvasPackage(file))
        return importPackage(command, file);
    const Result<mask::PaintDocument> document = mask::readCanvasFile(file);
    if (!document)
        return Reply::failure(document.errors());
    CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasInfo> made =
        store.create(command.has("name") ? command.value("name") : QFileInfo(file).completeBaseName(), document.value(), QStringLiteral("import"));
    if (!made)
        return Reply::failure(made.errors());
    return madeReply(made.value(), QStringLiteral("Imported"), document->width(), document->height());
}

// A whole number in a range, or a usage error naming the option.
Result<int> wholeOption(const Command &command, const QString &name, int least, int most) {
    bool ok = false;
    const int value = command.value(name).toInt(&ok);
    if (!ok || value < least || value > most)
        return usageError(QStringLiteral("--%1 is a whole number from %2 to %3.").arg(name).arg(least).arg(most), command.value(name));
    return value;
}

// The picture options of `canvas export` (engine/canvas_export.h) from the arguments; `layers` is the one-file-per-layer form.
Result<canvas::PictureExport> pictureOptions(const Command &command, const QString &out) {
    canvas::PictureExport options;
    const QString suffix = QFileInfo(out).suffix().toLower();
    const QString fromName = canvas::normalFormat(suffix, true);
    if (command.has("format")) {
        options.format = canvas::normalFormat(command.value("format"), true);
        if (options.format.isEmpty())
            return usageError(QStringLiteral("--format is png, jpg, webp or ora."), command.value("format"));
        if (!fromName.isEmpty() && fromName != options.format)
            return usageError(QStringLiteral("The file name ends in .%1 but --format is %2.").arg(suffix, options.format), out);
    } else if (!fromName.isEmpty()) {
        options.format = fromName;
    } else {
        return usageError(QStringLiteral("The file name ends in .%1; export writes .ora (layers), .png, .jpg or .webp (flattened).").arg(suffix), out);
    }
    if (command.has("quality")) {
        if (options.format == QLatin1String("png") || options.format == QLatin1String("ora"))
            return usageError(QStringLiteral("--quality applies to jpg and webp."), options.format);
        const Result<int> quality = wholeOption(command, QStringLiteral("quality"), 1, 100);
        if (!quality)
            return quality.errors();
        options.quality = quality.value();
        options.lossless = options.format == QLatin1String("webp") && command.has("lossless"); // a quality on a webp asks for a lossy one
    }
    if (command.has("lossless") && options.format != QLatin1String("webp"))
        return usageError(QStringLiteral("--lossless applies to webp."), options.format);
    if (int(command.has("scale")) + int(command.has("size")) + int(command.has("width") || command.has("height")) > 1)
        return usageError(QStringLiteral("Give one way to set the size: --scale, --size, or --width and/or --height."));
    if (command.has("scale")) {
        QString text = command.value("scale").trimmed();
        const bool percent = text.endsWith(QLatin1Char('%'));
        if (percent)
            text.chop(1);
        bool ok = false;
        const double value = text.toDouble(&ok);
        if (!ok || !(value > 0))
            return usageError(QStringLiteral("--scale is a percentage such as 50% or a factor such as 0.5."), command.value("scale"));
        options.scale = percent ? value / 100 : value;
    }
    if (command.has("size")) {
        static const QRegularExpression pattern(QStringLiteral("^([0-9]{1,5})x([0-9]{1,5})$"));
        const QRegularExpressionMatch match = pattern.match(command.value("size"));
        if (!match.hasMatch())
            return usageError(QStringLiteral("--size is <width>x<height>, such as 800x600."), command.value("size"));
        options.width = match.captured(1).toInt();
        options.height = match.captured(2).toInt();
        if (options.width < 1 || options.height < 1)
            return usageError(QStringLiteral("--size is at least 1x1."), command.value("size"));
    }
    for (const QString &name : QStringList{QStringLiteral("width"), QStringLiteral("height")}) {
        if (!command.has(name))
            continue;
        const Result<int> side = wholeOption(command, name, 1, mask::paintFileMaxSide);
        if (!side)
            return side.errors();
        (name == QLatin1String("width") ? options.width : options.height) = side.value();
    }
    if (command.has("background"))
        options.background = command.value("background");
    return options;
}

Reply layersExport(const Command &command, const Ref &ref, const QString &folder, const mask::PaintDocument &document, const QString &revision) {
    for (const char *name : {"scale", "size", "width", "height", "background", "estimate"})
        if (command.has(QString::fromLatin1(name)))
            return Reply::failure(usageError(QStringLiteral("--%1 is for one picture; --layers writes each layer as it is.").arg(QString::fromLatin1(name))));
    canvas::LayersExport options;
    options.format = command.has("format") ? command.value("format") : QStringLiteral("png");
    options.visibleOnly = command.has("only-visible");
    options.applyMasks = !command.has("no-masks");
    options.prefix = command.has("prefix") ? command.value("prefix") : QString();
    if (command.has("quality")) {
        const Result<int> quality = wholeOption(command, QStringLiteral("quality"), 1, 100);
        if (!quality)
            return Reply::failure(quality.errors());
        options.quality = quality.value();
        options.lossless = command.has("lossless");
    }
    const Result<canvas::LayerFiles> made = canvas::exportLayers(document, options);
    if (!made)
        return Reply::failure(made.errors());
    const Errors written = canvas::writeLayerFiles(folder, made.value());
    if (hasErrors(written))
        return Reply::failure(written);
    QJsonArray files;
    qint64 total = 0;
    for (const canvas::LayerFile &file : made->files) {
        files.append(QJsonObject{{"position", file.position}, {"layer", file.name}, {"file", QDir(folder).absoluteFilePath(file.fileName)}, {"bytes", file.bytes.size()}});
        total += file.bytes.size();
    }
    QJsonObject data{{"canvas", ref.canvas}, {"revision", revision}, {"folder", QFileInfo(folder).absoluteFilePath()}, {"format", canvas::normalFormat(options.format, false)},
                     {"files", files}, {"bytes", total}};
    QString text = QStringLiteral("Exported %1 layers of %2 revision %3 to %4.").arg(made->files.size()).arg(ref.canvas, revision, folder);
    if (!made->skipped.isEmpty()) {
        QJsonArray skipped;
        for (const QString &name : made->skipped)
            skipped.append(name);
        data.insert(QStringLiteral("skipped"), skipped);
        text += QStringLiteral(" Left out (mask layers are selections): %1.").arg(made->skipped.join(QStringLiteral(", ")));
    }
    return Reply::success(data, text);
}

Reply canvasExport(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    const bool layers = command.has("layers");
    const bool estimate = command.has("estimate");
    if (command.positionals.size() < 2 && !estimate)
        return Reply::failure(usageError(QStringLiteral("Name the file to write (or the folder, with --layers), or add --estimate."), QStringLiteral("fugue canvas export <id[@rev]> <file> [options]")));
    const QString out = command.positionals.size() > 1 ? command.positionals.at(1) : QString();
    // A .fuguepack is a package to share (#249); every other target goes to the picture exporter below.
    const bool package = QFileInfo(out).suffix().compare(QLatin1String("fuguepack"), Qt::CaseInsensitive) == 0;
    if (command.has("history") && !package)
        return Reply::failure(usageError(QStringLiteral("--history is for a package: name a file ending in .fuguepack."), QStringLiteral("fugue canvas export <id> <file>.fuguepack --history")));
    if (package) {
        for (const char *name : {"layers", "estimate", "lossless", "only-visible", "no-masks", "format", "quality", "scale", "size", "width", "height", "background", "prefix"})
            if (command.has(QString::fromLatin1(name)))
                return Reply::failure(usageError(QStringLiteral("--%1 is for a picture or a folder of layers; a .fuguepack is a package of the canvas as it is (add --history for every revision).").arg(QString::fromLatin1(name)),
                                                 QStringLiteral("fugue canvas export <id> <file>.fuguepack [--history]")));
        if (ref.written && ref.revision != QLatin1String("head"))
            return Reply::failure(usageError(QStringLiteral("A package carries the head of a canvas, with or without its history; name the canvas without @%1.").arg(ref.revision), command.positionals.at(0)));
        const bool history = command.has("history");
        const CanvasStore shared = CanvasStore::forSession();
        const Errors written = canvas::writeCanvasPackage(shared, ref.canvas, history ? canvas::SharedPart::History : canvas::SharedPart::Head, QFileInfo(out).absoluteFilePath());
        if (hasErrors(written))
            return Reply::failure(written);
        return Reply::success(QJsonObject{{"canvas", ref.canvas}, {"part", history ? "history" : "head"}, {"path", QFileInfo(out).absoluteFilePath()}},
                              QStringLiteral("Wrote %1 (%2 of %3).").arg(out, history ? QStringLiteral("the head and its history") : QStringLiteral("the head only"), ref.canvas));
    }
    if (!layers)
        for (const char *name : {"only-visible", "no-masks", "prefix"})
            if (command.has(QString::fromLatin1(name)))
                return Reply::failure(usageError(QStringLiteral("--%1 is for --layers.").arg(QString::fromLatin1(name))));
    Result<canvas::PictureExport> options = canvas::PictureExport{};
    if (!layers) {
        if (out.isEmpty() && !command.has("format"))
            return Reply::failure(usageError(QStringLiteral("--estimate with no file needs --format png, jpg, webp or ora.")));
        options = pictureOptions(command, out.isEmpty() ? QStringLiteral(".") + command.value("format") : out);
        if (!options)
            return Reply::failure(options.errors());
    }
    const CanvasStore store = CanvasStore::forSession();
    const Result<QString> revision = store.resolve(ref.canvas, ref.revision);
    if (!revision)
        return Reply::failure(revision.errors());
    const Result<mask::PaintDocument> document = store.load(ref.canvas, revision.value());
    if (!document)
        return Reply::failure(document.errors());
    if (layers)
        return layersExport(command, ref, out, document.value(), revision.value());
    const Result<canvas::ExportedPicture> picture = canvas::exportPicture(document.value(), options.value());
    if (!picture)
        return Reply::failure(picture.errors());
    QJsonObject data{{"canvas", ref.canvas}, {"revision", revision.value()}, {"width", picture->width}, {"height", picture->height}, {"bytes", picture->bytes.size()}};
    if (picture->format != QLatin1String("ora"))
        data.insert(QStringLiteral("format"), picture->format);
    if (!picture->background.isEmpty() && picture->background != QLatin1String("transparent"))
        data.insert(QStringLiteral("background"), picture->background);
    QString text;
    if (estimate) {
        text = QStringLiteral("%1 revision %2 as %3, %4 x %5: %6 bytes.").arg(ref.canvas, revision.value(), picture->format).arg(picture->width).arg(picture->height).arg(picture->bytes.size());
    } else {
        const Errors written = createFile(out, picture->bytes, QStringLiteral("the exported canvas"));
        if (hasErrors(written))
            return Reply::failure(written);
        data.insert(QStringLiteral("path"), QFileInfo(out).absoluteFilePath());
        text = QStringLiteral("Exported %1 revision %2 to %3.").arg(ref.canvas, revision.value(), out);
    }
    if (picture->format == QLatin1String("jpg") && picture->background == QLatin1String("white") && !command.has("background"))
        text += QStringLiteral(" JPEG has no transparency: the canvas was flattened onto white.");
    return Reply::success(data, text);
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
        layers.append(layerJson(layer, int(&layer - tree->layers.data())));
        lines << QStringLiteral("  %1").arg(layerLine(layer, int(&layer - tree->layers.data())));
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
    canvas::TreeDiff diff = canvas::diffTrees(before.value(), after.value());
    canvas::tightenBounds(&diff, before.value(), after.value(), [&](const QString &h, int w, int ht, int c) { return store.tilePixels(h, w, ht, c); });
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

namespace {

// The layers named on the command line: positions (0 the bottom) or names (one layer each).
Result<std::vector<int>> layerPositions(const canvas::Tree &tree, const QStringList &words) {
    std::vector<int> out;
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,4}$"));
    for (const QString &word : words) {
        if (digits.match(word).hasMatch()) {
            out.push_back(word.toInt());
            continue;
        }
        std::vector<int> found;
        for (int i = 0; i < int(tree.layers.size()); ++i)
            if (tree.layers[size_t(i)].name == word)
                found.push_back(i);
        if (found.size() != 1)
            return Result<std::vector<int>>(usageError(found.empty() ? QStringLiteral("No layer is named \"%1\".").arg(word)
                                                                     : QStringLiteral("%1 layers are named \"%2\"; give the one you mean by its position.").arg(found.size()).arg(word),
                                                       QStringLiteral("`fugue canvas layer list <canvas>` lists the layers with their positions, 0 the bottom.")));
        out.push_back(found.front());
    }
    return out;
}

Result<bool> onOff(const QString &option, const QString &text, const QString &on, const QString &off) {
    if (text == on)
        return true;
    if (text == off)
        return false;
    return Result<bool>(usageError(QStringLiteral("--%1 is %2 or %3, not %4.").arg(option, on, off, text)));
}

Reply layerList(const CanvasStore &store, const Ref &ref) {
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
    QStringList lines;
    // Top first, as a layers panel lists them; each with its position.
    for (int i = int(tree->layers.size()) - 1; i >= 0; --i) {
        layers.append(layerJson(tree->layers[size_t(i)], i));
        lines << layerLine(tree->layers[size_t(i)], i);
    }
    if (lines.isEmpty())
        lines << QStringLiteral("Revision %1 has no layer.").arg(id.value().left(15));
    return Reply::success(QJsonObject{{"canvas", ref.canvas}, {"revision", id.value()}, {"layers", layers}}, lines.join('\n'));
}

} // namespace

// `fugue canvas layer <action> <canvas> ...`: list, or an edit of the head as one revision each (spec 14.5).
Reply canvasLayer(const Command &command, Context &) {
    const QString action = command.positionals.at(0);
    const Ref ref = parseRef(command.positionals.at(1));
    const QStringList rest = command.positionals.mid(2);
    CanvasStore store = CanvasStore::forSession();
    if (action == QLatin1String("list")) {
        if (!rest.isEmpty())
            return Reply::failure(usageError(QStringLiteral("layer list takes a canvas (and a revision) only.")));
        return layerList(store, ref);
    }
    static const QStringList actions{"list", "set", "rename", "rm", "dup", "move", "new", "merge", "flatten", "group", "ungroup"};
    if (!actions.contains(action))
        return Reply::failure(usageError(QStringLiteral("\"%1\" is not a layer action.").arg(action), QStringLiteral("Use list, set, rename, rm, dup, move, new, merge, flatten, group or ungroup.")));
    if (ref.written && ref.revision != QLatin1String("head"))
        return Reply::failure(usageError(QStringLiteral("Layers are edited on the head only: write the canvas id without @revision."),
                                         QStringLiteral("To edit an older revision, accept it or undo back to it first.")));
    const Result<canvas::CanvasInfo> info = store.info(ref.canvas);
    if (!info)
        return Reply::failure(info.errors());
    if (info->head.isEmpty())
        return Reply::failure(makeError(QStringLiteral("canvas_not_found"), QStringLiteral("Canvas %1 has no revision yet.").arg(ref.canvas)));
    const Result<canvas::Revision> head = store.revision(ref.canvas, info->head);
    if (!head)
        return Reply::failure(head.errors());
    const Result<canvas::Tree> tree = store.tree(head->tree);
    if (!tree)
        return Reply::failure(tree.errors());

    // The edits the action asks for, in order (set may ask for several: each is its own revision).
    std::vector<canvas::LayerEdit> edits;
    QStringList layerWords = rest;
    if (action == QLatin1String("rename")) {
        if (rest.size() != 2)
            return Reply::failure(usageError(QStringLiteral("layer rename takes a canvas, a layer and the new name.")));
        layerWords = rest.mid(0, 1);
    }
    const bool wantsNoLayer = action == QLatin1String("flatten") || action == QLatin1String("new");
    if (layerWords.isEmpty() && !wantsNoLayer)
        return Reply::failure(usageError(QStringLiteral("Name at least one layer, by its position (0 is the bottom) or its name.")));
    if (command.has("name") && action != QLatin1String("new") && action != QLatin1String("group"))
        return Reply::failure(usageError(QStringLiteral("--name names the layer `layer new` or `layer group` makes; `layer rename` renames one.")));
    const Result<std::vector<int>> positions = layerPositions(tree.value(), layerWords); // empty for flatten and an on-top new layer
    if (!positions)
        return Reply::failure(positions.errors());
    const auto add = [&](canvas::LayerOp op) -> canvas::LayerEdit & {
        canvas::LayerEdit e;
        e.op = op;
        e.layers = positions.value();
        edits.push_back(e);
        return edits.back();
    };
    if (action == QLatin1String("set")) {
        if (command.has("visible")) {
            const Result<bool> v = onOff("visible", command.value("visible"), QStringLiteral("true"), QStringLiteral("false"));
            if (!v)
                return Reply::failure(v.errors());
            add(canvas::LayerOp::Visible).flag = v.value();
        }
        if (command.has("opacity")) {
            bool ok = false;
            const double value = command.value("opacity").toDouble(&ok);
            if (!ok)
                return Reply::failure(usageError(QStringLiteral("--opacity is a number from 0 to 1."), command.value("opacity")));
            add(canvas::LayerOp::Opacity).number = value;
        }
        if (command.has("blend"))
            add(canvas::LayerOp::Blend).text = command.value("blend");
        if (command.has("mask")) {
            const QString mask = command.value("mask");
            if (mask == QLatin1String("delete"))
                add(canvas::LayerOp::RemoveMask);
            else {
                const Result<bool> v = onOff("mask", mask, QStringLiteral("on"), QStringLiteral("off"));
                if (!v)
                    return Reply::failure(usageError(QStringLiteral("--mask is on, off or delete, not %1.").arg(mask)));
                add(canvas::LayerOp::MaskEnabled).flag = v.value();
            }
        }
        if (edits.empty())
            return Reply::failure(usageError(QStringLiteral("layer set needs --visible, --opacity, --blend or --mask.")));
    } else if (action == QLatin1String("rename")) {
        add(canvas::LayerOp::Rename).text = rest.at(1);
    } else if (action == QLatin1String("new")) {
        if (layerWords.size() > 1)
            return Reply::failure(usageError(QStringLiteral("layer new takes the one layer the new one goes above, or none (on top).")));
        add(canvas::LayerOp::New).text = command.value("name");
    } else if (action == QLatin1String("merge")) {
        if (layerWords.size() != 1)
            return Reply::failure(usageError(QStringLiteral("layer merge takes the one layer to merge down into the layer under it.")));
        add(canvas::LayerOp::MergeDown);
    } else if (action == QLatin1String("flatten")) {
        if (!layerWords.isEmpty())
            return Reply::failure(usageError(QStringLiteral("layer flatten takes the canvas only: it flattens every shown layer.")));
        add(canvas::LayerOp::Flatten);
    } else if (action == QLatin1String("group")) {
        add(canvas::LayerOp::Group).text = command.value("name");
    } else if (action == QLatin1String("ungroup")) {
        add(canvas::LayerOp::Ungroup);
    } else if (action == QLatin1String("rm")) {
        add(canvas::LayerOp::Remove);
    } else if (action == QLatin1String("dup")) {
        add(canvas::LayerOp::Duplicate);
    } else if (action == QLatin1String("move")) {
        bool ok = false;
        const int to = command.value("to").toInt(&ok);
        if (!command.has("to") || !ok || to < 0)
            return Reply::failure(usageError(QStringLiteral("layer move needs --to <position>, 0 for the bottom.")));
        add(canvas::LayerOp::Move).to = to;
    }

    QString at = info->head;
    QJsonArray made;
    QStringList lines;
    for (const canvas::LayerEdit &edit : edits) {
        const Result<canvas::Commit> done = store.editLayers(ref.canvas, at, edit, QStringLiteral("canvas-cli:") + canvas::layerOpName(edit.op));
        if (!done)
            return Reply::failure(done.errors());
        if (!done->move.moved && done->revision.id == at && done->move.head == at) {
            lines << QStringLiteral("%1: nothing to change.").arg(canvas::layerOpName(edit.op));
            continue;
        }
        if (!done->move.moved)
            return Reply::failure(makeError(QStringLiteral("canvas_head_moved"), QStringLiteral("The head of %1 moved while this ran (it is %2 now); nothing was changed.").arg(ref.canvas, done->move.head),
                                            QStringLiteral("Run it again.")));
        at = done->revision.id;
        made.append(revisionJson(done->revision));
        lines << QStringLiteral("%1: new head %2.").arg(canvas::layerOpName(edit.op), at);
    }
    return Reply::success(QJsonObject{{"canvas", ref.canvas}, {"head", at}, {"revisions", made}}, lines.join('\n'));
}

namespace {

Reply headStep(const Command &command, bool forward) {
    const Ref ref = parseRef(command.positionals.at(0));
    if (ref.written)
        return Reply::failure(usageError(QStringLiteral("%1 takes a canvas id, not a revision.").arg(forward ? QStringLiteral("redo") : QStringLiteral("undo"))));
    CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasInfo> info = store.info(ref.canvas);
    if (!info)
        return Reply::failure(info.errors());
    const Result<canvas::HeadMove> moved = forward ? store.redo(ref.canvas, info->head) : store.undo(ref.canvas, info->head);
    if (!moved)
        return Reply::failure(moved.errors());
    if (!moved->moved)
        return Reply::failure(makeError(QStringLiteral("canvas_head_moved"), QStringLiteral("The head of %1 moved while this ran (it is %2 now); nothing was changed.").arg(ref.canvas, moved->head),
                                        QStringLiteral("Run it again.")));
    return Reply::success(QJsonObject{{"canvas", ref.canvas}, {"head", moved->head}, {"from", info->head}},
                          QStringLiteral("%1 %2: the head is %3.").arg(forward ? QStringLiteral("Redid") : QStringLiteral("Undid"), ref.canvas, moved->head));
}

} // namespace

// `fugue canvas resize <id> --to x,y,w,h [--fill ...] [--blur n]`: ONE revision, the canvas of the rectangle (#240).
Reply canvasResize(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    if (ref.written && ref.revision != QLatin1String("head"))
        return Reply::failure(usageError(QStringLiteral("A canvas is resized at the head: write the canvas id without @revision.")));
    static const QRegularExpression pattern(QStringLiteral("^(-?[0-9]{1,6}),(-?[0-9]{1,6}),([0-9]{1,6}),([0-9]{1,6})$"));
    const QRegularExpressionMatch match = pattern.match(command.value("to"));
    if (!command.has("to") || !match.hasMatch())
        return Reply::failure(usageError(QStringLiteral("canvas resize needs --to <x>,<y>,<width>,<height>, the new canvas as a rectangle of the old one (x and y may be negative to grow left or up)."),
                                         command.value("to")));
    const mask::IRect to{match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt(), match.captured(4).toInt()};
    edgefill::Mode fill = edgefill::Mode::EdgeBlur;
    if (command.has("fill") && !edgefill::modeFromName(command.value("fill"), &fill))
        return Reply::failure(usageError(QStringLiteral("--fill is edge-blur, edge, gray or transparent, not %1.").arg(command.value("fill"))));
    int blur = edgefill::defaultBlur;
    if (command.has("blur")) {
        bool ok = false;
        blur = command.value("blur").toInt(&ok);
        if (!ok || blur < 0 || blur > 512)
            return Reply::failure(usageError(QStringLiteral("--blur is a radius in pixels, 0 to 512."), command.value("blur")));
    }
    CanvasStore store = CanvasStore::forSession();
    const Result<canvas::CanvasInfo> info = store.info(ref.canvas);
    if (!info)
        return Reply::failure(info.errors());
    if (info->head.isEmpty())
        return Reply::failure(makeError(QStringLiteral("canvas_not_found"), QStringLiteral("Canvas %1 has no revision yet.").arg(ref.canvas)));
    const Result<canvas::Commit> done = store.resize(ref.canvas, info->head, to, fill, blur, QStringLiteral("canvas-cli:resize"));
    if (!done)
        return Reply::failure(done.errors());
    if (!done->move.moved)
        return Reply::failure(makeError(QStringLiteral("canvas_head_moved"), QStringLiteral("The head of %1 moved while this ran (it is %2 now); nothing was changed.").arg(ref.canvas, done->move.head),
                                        QStringLiteral("Run it again.")));
    QJsonObject data = revisionJson(done->revision);
    data.insert(QStringLiteral("canvas"), ref.canvas);
    data.insert(QStringLiteral("head"), done->revision.id);
    data.insert(QStringLiteral("width"), to.width);
    data.insert(QStringLiteral("height"), to.height);
    data.insert(QStringLiteral("fill"), QString::fromLatin1(edgefill::modeName(fill)));
    return Reply::success(data, QStringLiteral("Resized %1 to %2 x %3: new head %4.").arg(ref.canvas).arg(to.width).arg(to.height).arg(done->revision.id));
}

Reply canvasUndo(const Command &command, Context &) { return headStep(command, false); }
Reply canvasRedo(const Command &command, Context &) { return headStep(command, true); }

} // namespace fugue::cli
