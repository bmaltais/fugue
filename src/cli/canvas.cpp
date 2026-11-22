// `fugue canvas new|import|export|log|show|list|candidates|accept|drop|diff|layer|undo|redo` (spec 13.1, 14.5, epic #206): the canvas revision store
// from the command line. The work is engine/canvas_store.h's; this reads the arguments and prints.
// A canvas lives in the session's store (a project's own under FUGUE_HOME); `<canvas>` is its id,
// `<canvas>@<revision>` a revision of it (a full hash, the start of one, or `head`).

#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include "cli/commands.h"
#include "engine/atomic_file.h"
#include "engine/canvas_diff.h"
#include "engine/canvas_layer_ops.h"
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
                    {"kind", kindText(layer.kind)},
                    {"visible", layer.visible},
                    {"opacity", layer.opacity},
                    {"blend", blendText(layer.compositeOp)},
                    {"tiles", int(layer.tiles.size())}};
    if (layer.hasMask)
        out.insert(QStringLiteral("mask"), QJsonObject{{"enabled", layer.maskEnabled}, {"tiles", int(layer.maskTiles.size())}});
    if (!layer.producer.isEmpty())
        out.insert(QStringLiteral("producer"), layer.producer);
    if (!layer.created.isEmpty())
        out.insert(QStringLiteral("created"), layer.created);
    return out;
}

QString layerLine(const canvas::LayerRecord &layer, int position) {
    QString line = QStringLiteral("%1 %2: %3%4, opacity %5, %6 tiles").arg(position).arg(layer.name, kindText(layer.kind),
                                                                           layer.visible ? QString() : QStringLiteral(", hidden"))
                       .arg(layer.opacity).arg(layer.tiles.size());
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

Reply canvasImport(const Command &command, Context &) {
    const QString file = command.positionals.at(0);
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

Reply canvasExport(const Command &command, Context &) {
    const Ref ref = parseRef(command.positionals.at(0));
    const QString out = command.positionals.at(1);
    // By the extension: .ora is the layered file, a picture is the flattened canvas.
    const QString suffix = QFileInfo(out).suffix().toLower();
    const QString picture = suffix == QLatin1String("jpeg") ? QStringLiteral("jpg") : suffix;
    if (suffix != QLatin1String("ora") && picture != QLatin1String("png") && picture != QLatin1String("jpg") && picture != QLatin1String("webp"))
        return Reply::failure(usageError(QStringLiteral("The file name ends in .%1; export writes .ora (layers), .png, .jpg or .webp (flattened).").arg(suffix), out));
    const CanvasStore store = CanvasStore::forSession();
    const Result<QString> revision = store.resolve(ref.canvas, ref.revision);
    if (!revision)
        return Reply::failure(revision.errors());
    const Result<mask::PaintDocument> document = store.load(ref.canvas, revision.value());
    if (!document)
        return Reply::failure(document.errors());
    const Result<QByteArray> bytes = suffix == QLatin1String("ora") ? mask::paintToOra(document.value()) : document->flattenToPicture(picture);
    if (!bytes)
        return Reply::failure(bytes.errors());
    const Errors written = createFile(out, bytes.value(), QStringLiteral("the exported canvas"));
    if (hasErrors(written))
        return Reply::failure(written);
    QJsonObject data{{"canvas", ref.canvas}, {"revision", revision.value()}, {"path", QFileInfo(out).absoluteFilePath()}};
    QString text = QStringLiteral("Exported %1 revision %2 to %3.").arg(ref.canvas, revision.value(), out);
    if (suffix != QLatin1String("ora"))
        data.insert(QStringLiteral("format"), picture);
    if (picture == QLatin1String("jpg")) {
        data.insert(QStringLiteral("background"), QStringLiteral("white"));
        text += QStringLiteral(" JPEG has no transparency: the canvas was flattened onto white.");
    }
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
    static const QStringList actions{"list", "set", "rename", "rm", "dup", "move"};
    if (!actions.contains(action))
        return Reply::failure(usageError(QStringLiteral("\"%1\" is not a layer action.").arg(action), QStringLiteral("Use list, set, rename, rm, dup or move.")));
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
    if (layerWords.isEmpty())
        return Reply::failure(usageError(QStringLiteral("Name at least one layer, by its position (0 is the bottom) or its name.")));
    const Result<std::vector<int>> positions = layerPositions(tree.value(), layerWords);
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

Reply canvasUndo(const Command &command, Context &) { return headStep(command, false); }
Reply canvasRedo(const Command &command, Context &) { return headStep(command, true); }

} // namespace fugue::cli
