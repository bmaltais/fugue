#include "nodes/canvas_nodes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <optional>

#include "engine/canvas_handle.h"
#include "engine/canvas_layer_ops.h"
#include "engine/canvas_store.h"
#include "engine/canvas_tiles.h"
#include "engine/clock.h"
#include "engine/hash.h"
#include "engine/new_canvas.h"
#include "nodes/canvas_region_ops.h"
#include "nodes/kernels.h"
#include "nodes/pixel_io.h"
#include "nodes/stitch_handle.h"

namespace fugue::nodes {

namespace {

using canvas::CanvasHandle;
using canvas::CanvasStore;
using mask::LayerKind;
using mask::PaintDocument;

constexpr int kMaxSide = 16384; // the store's own limit (14.5)
constexpr int kMaxPadding = 4096;
constexpr int kMaxBlend = 512;
constexpr int kMaxFeather = 512;

Error warning(Error error) {
    error.severity = Error::Severity::Warning;
    return error;
}

Result<int> intInput(const NodeContext &context, const char *name, int lowest, int highest) {
    const QString input = QString::fromLatin1(name);
    const int value = toInt(context.input(input));
    if (value < lowest || value > highest)
        return makeInputError(QStringLiteral("value_out_of_range"), input,
                              QStringLiteral("%1 is outside %2 to %3.").arg(value).arg(lowest).arg(highest));
    return value;
}

// ---- the name of a Write's layer (#226) ----------------------------------------------------------

// The nodes map of the graph a node's path id lives in ("a/b": inside group a), or empty.
QJsonObject graphOf(const QJsonObject &document, const QString &path, QString *own) {
    QJsonObject nodes = document.value("graph").toObject().value("nodes").toObject();
    const QStringList parts = path.split(QLatin1Char('/'));
    for (int i = 0; i + 1 < parts.size(); ++i)
        nodes = nodes.value(parts[i]).toObject().value("graph").toObject().value("nodes").toObject();
    *own = parts.last();
    return nodes;
}

// The nodes a value links from ({"from": "node.port"}, or a list of them).
void linkedNodes(const QJsonValue &value, QStringList *out) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            linkedNodes(item, out);
        return;
    }
    const QString from = value.toObject().value("from").toString();
    if (!from.isEmpty())
        *out << from.section(QLatin1Char('.'), 0, 0);
}

// The prompt nearest upstream of the node: the first `prompt` text met walking its links back
// (the Text Encode that made the picture), simplified to one line. Empty when there is none.
QString upstreamPrompt(const QJsonObject &document, const QString &node) {
    QString own;
    const QJsonObject nodes = graphOf(document, node, &own);
    QStringList queue{own}, seen{own};
    while (!queue.isEmpty()) {
        const QJsonObject inputs = nodes.value(queue.takeFirst()).toObject().value("inputs").toObject();
        const QJsonValue prompt = inputs.value("prompt");
        if (prompt.isString() && !prompt.toString().trimmed().isEmpty())
            return prompt.toString().simplified();
        QStringList next;
        for (auto it = inputs.begin(); it != inputs.end(); ++it)
            linkedNodes(it.value(), &next);
        for (const QString &id : std::as_const(next))
            if (!seen.contains(id)) {
                seen << id;
                queue << id;
            }
    }
    return {};
}

// A prompt cut to a layer name: at most 40 characters, at a word when it can, with an ellipsis.
QString promptName(const QString &prompt) {
    constexpr int most = 40;
    if (prompt.size() <= most)
        return prompt;
    QString cut = prompt.left(most);
    const qsizetype space = cut.lastIndexOf(QLatin1Char(' '));
    if (space >= most / 2)
        cut = cut.left(space);
    while (!cut.isEmpty() && QStringLiteral(" ,.;:").contains(cut.back()))
        cut.chop(1);
    return cut + QChar(0x2026);
}

// "Fill n": one more than the largest n of a layer named "Fill n" ("Fill" alone is 1).
void highestFill(const std::vector<canvas::LayerRecord> &layers, int *highest) {
    static const QRegularExpression pattern(QStringLiteral("^Fill(?: ([0-9]{1,6}))?$"));
    for (const canvas::LayerRecord &layer : layers) {
        const QRegularExpressionMatch match = pattern.match(layer.name);
        if (match.hasMatch())
            *highest = std::max(*highest, match.captured(1).isEmpty() ? 1 : match.captured(1).toInt());
        highestFill(layer.children, highest); // a layer inside a group counts too
    }
}

QString nextFillName(const canvas::Tree &tree) {
    int highest = 0;
    highestFill(tree.layers, &highest);
    return QStringLiteral("Fill %1").arg(highest + 1);
}

// The errors, each naming `input` when it names none.
Errors withInput(Errors errors, const QString &input) {
    for (Error &error : errors)
        if (error.input.isEmpty())
            error.input = input;
    return errors;
}

Result<PaintDocument> openRevision(const CanvasHandle &handle) {
    return CanvasStore::forSession().load(handle.canvas, handle.revision);
}

// The canvas input of Region: the id and a revision that the compile has already named in full.
Result<CanvasHandle> regionSource(const NodeContext &context) {
    CanvasHandle handle{context.input(QStringLiteral("canvas")).text().trimmed(), context.input(QStringLiteral("revision")).text().trimmed()};
    if (handle.canvas.isEmpty())
        return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("canvas"), QStringLiteral("Choose a canvas: this root has none yet, so there is no active canvas to read."),
                              QStringLiteral("Make one (`fugue canvas new` or `fugue canvas import`, or New canvas in the Canvas view), or give this Canvas Region the id it prints, such as canvas-0123456789ab."));
    if (!canvas::isCanvasId(handle.canvas))
        return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("canvas"),
                              QStringLiteral("\"%1\" is not a canvas id.").arg(handle.canvas),
                              QStringLiteral("Use the id `fugue canvas new` prints, such as canvas-0123456789ab."));
    if (!isSha256(handle.revision))
        return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("revision"),
                              QStringLiteral("The revision \"%1\" was not named in full when the workflow was compiled.").arg(handle.revision),
                              QStringLiteral("Run the workflow with `fugue run` or the Run button, which name head once, and wire the canvas and revision as literals."));
    return handle;
}

class CanvasRegionKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<CanvasHandle> source = regionSource(context);
        if (!source)
            return Result<Values>(source.errors());
        const Result<int> padding = intInput(context, "padding", 0, kMaxPadding);
        const Result<int> feather = intInput(context, "feather", 0, kMaxFeather);
        const Result<int> multiple = intInput(context, "multiple", 1, 256);
        const double scale = context.input(QStringLiteral("scale")).number();
        const double cap = context.input(QStringLiteral("max_megapixels")).number();
        // Region 1.3 (#242): an older workflow has no such input, which reads as 0 (off).
        const double target = context.input(QStringLiteral("target_megapixels")).number();
        for (const Result<int> *checked : {&padding, &feather, &multiple})
            if (!checked->ok())
                return Result<Values>(checked->errors());
        if (!(scale >= 0.25 && scale <= 4))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("scale"), QStringLiteral("A scale of %1 is outside 0.25 to 4.").arg(scale));
        if (!(cap >= 0 && cap <= 16))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("max_megapixels"), QStringLiteral("%1 megapixels is outside 0 to 16.").arg(cap));

        if (!(target >= 0 && target <= 16))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("target_megapixels"), QStringLiteral("%1 megapixels is outside 0 to 16.").arg(target));

        Result<PaintDocument> opened = openRevision(source.value());
        if (!opened)
            return Result<Values>(opened.errors());
        // The composite (the visible layers, as the Canvas view shows them), or one layer's own pixels.
        const QString layerName = context.input(QStringLiteral("layer")).text().trimmed();
        Pixels composite;
        if (layerName.isEmpty()) {
            composite = layerToPixels(opened->flatten());
        } else {
            const mask::MaskPaint *found = nullptr;
            QStringList names;
            std::vector<canvas::LayerFace> faces;
            for (int i = 0; i < opened->layerCount(); ++i)
                faces.push_back({opened->layer(i).name, opened->layer(i).kind, false});
            if (const int at = canvas::topLayerNamed(faces, layerName, &names); at >= 0) // the same layer Canvas Write means
                found = opened->pixels(opened->layer(at).id);
            if (!found)
                return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("layer"),
                                      QStringLiteral("The canvas has no layer named \"%1\" at this revision.").arg(layerName.left(60)),
                                      names.isEmpty() ? QStringLiteral("Leave Layer empty to read the composite.")
                                                      : QStringLiteral("Its layers are: %1. Leave Layer empty to read the composite.").arg(names.join(QStringLiteral(", ")).left(300)));
            if (found->channels() != 4) // a mask layer is not a picture
                return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("layer"),
                                      QStringLiteral("\"%1\" is a mask layer, not a picture.").arg(layerName.left(60)));
            composite = layerToPixels(*found);
        }

        Errors warnings;
        std::optional<CropBox> bounds;
        Pixels mask;
        const QString selection = context.input(QStringLiteral("selection")).text().trimmed();
        if (!selection.isEmpty()) {
            // A selection the Canvas view stored (#230): read from the canvas folder by its hash, as a mask would be.
            if (!isSha256(selection))
                return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("selection"),
                                      QStringLiteral("\"%1\" is not a selection's hash.").arg(selection.left(80)),
                                      QStringLiteral("Paint the selection with Quick Mask in the Canvas view and press Use selection, or leave Selection empty."));
            const Result<mask::MaskPaint> stored = CanvasStore::forSession().selection(selection);
            if (!stored)
                return Result<Values>(withInput(stored.errors(), QStringLiteral("selection")));
            mask = layerToPixels(*stored);
            if (mask.width != composite.width || mask.height != composite.height) {
                warnings << warning(makeInputError(QStringLiteral("mask_resized"), QStringLiteral("selection"),
                                                   QStringLiteral("The selection is %1 x %2 and the canvas %3 x %4; the selection was scaled to the canvas.")
                                                       .arg(mask.width).arg(mask.height).arg(composite.width).arg(composite.height)));
                mask = maskResized(mask, composite.width, composite.height);
            }
            bounds = maskBounds(mask);
            if (!bounds)
                return makeInputError(QStringLiteral("mask_empty"), QStringLiteral("selection"), QStringLiteral("Nothing is selected."),
                                      QStringLiteral("Paint the part to change with Quick Mask in the Canvas view."));
        } else if (const Value given = context.input(QStringLiteral("mask")); given.isMedia()) {
            Result<Pixels> read = maskOf(given, QStringLiteral("mask"));
            if (!read)
                return Result<Values>(read.errors());
            mask = std::move(read.value());
            if (mask.width != composite.width || mask.height != composite.height) {
                warnings << warning(makeInputError(QStringLiteral("mask_resized"), QStringLiteral("mask"),
                                                   QStringLiteral("The mask is %1 x %2 and the canvas %3 x %4; the mask was scaled to the canvas.")
                                                       .arg(mask.width).arg(mask.height).arg(composite.width).arg(composite.height)));
                mask = maskResized(mask, composite.width, composite.height);
            }
            bounds = maskBounds(mask);
            if (!bounds)
                return makeInputError(QStringLiteral("mask_empty"), QStringLiteral("mask"), QStringLiteral("Nothing is painted in the mask."),
                                      QStringLiteral("Paint the part to change."));
        } else {
            bounds = rectBox(toInt(context.input(QStringLiteral("x"))), toInt(context.input(QStringLiteral("y"))),
                             toInt(context.input(QStringLiteral("width"))), toInt(context.input(QStringLiteral("height"))),
                             composite.width, composite.height);
            if (!bounds)
                return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("x"),
                                      QStringLiteral("The rectangle is outside the canvas (%1 x %2).").arg(composite.width).arg(composite.height));
            mask = boxMask(*bounds, composite.width, composite.height);
        }

        // Write's blend stops at this box: room for the feather and a default blend, or the seam is a hard edge (#242).
        const int room = paddingFor(padding.value(), feather.value());
        if (room > padding.value())
            warnings << warning(makeInputError(QStringLiteral("padding_raised"), QStringLiteral("padding"),
                                               QStringLiteral("Padding %1 leaves no room for the blend: it was raised to %2 (Feather %3 + %4).")
                                                   .arg(padding.value()).arg(room).arg(feather.value()).arg(kMinBlendRoom),
                                               QStringLiteral("Set Padding to at least Feather + %1 to silence this.").arg(kMinBlendRoom)));
        const CropBox box = paddedBox(*bounds, room, composite.width, composite.height);
        const CropSize size = regionSize(box, scale, cap, multiple.value(), target);
        Cropped crop = cropAt(composite, mask, box, size);
        // The feather is in canvas pixels; the region may have been redrawn at another scale.
        const int radius = int(std::lround(double(feather.value()) * size.width / box.width));
        crop.mask = featherMask(crop.mask, radius, FeatherDirection::Outward);

        // The stitch (Canvas Write's and Stitch's): the canvas as the original, and the region's own mask.
        Result<Values> original = imageOutput(composite);
        Result<Values> whole = namedOutput(mask, "mask");
        if (!original || !whole)
            return !original ? Result<Values>(original.errors()) : Result<Values>(whole.errors());
        const Result<MediaHandle> originalFile = namedHandle(original.value().value(QStringLiteral("image")).media(), context, QStringLiteral("canvas-image"));
        const Result<MediaHandle> maskFile = namedHandle(whole.value().value(QStringLiteral("mask")).media(), context, QStringLiteral("canvas-mask"));
        if (!originalFile || !maskFile)
            return !originalFile ? Result<Values>(originalFile.errors()) : Result<Values>(maskFile.errors());
        const Result<Value> stitch = writeStitchHandle(context.workDir, {originalFile.value(), maskFile.value(), crop.box, crop.size});
        const Result<QJsonObject> handle = canvas::writeCanvasHandle(context.workDir, source.value());
        Result<Values> image = imageOutput(std::move(crop.image));
        Result<Values> regionMask = namedOutput(std::move(crop.mask), "mask");
        for (const Errors &errors : {stitch.errors(), handle.errors(), image.errors(), regionMask.errors()})
            if (hasErrors(errors))
                return Result<Values>(errors);

        Values out;
        out.insert(QStringLiteral("image"), image.value().value(QStringLiteral("image")));
        out.insert(QStringLiteral("mask"), regionMask.value().value(QStringLiteral("mask")));
        out.insert(QStringLiteral("stitch"), stitch.value());
        out.insert(QStringLiteral("canvas"), Value(handle.value()));
        out.insert(QStringLiteral("width"), Value(size.width));
        out.insert(QStringLiteral("height"), Value(size.height));
        return Result<Values>(out, warnings);
    }
};

class CanvasWriteKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<CanvasHandle> base = canvas::canvasHandleFrom(context.input(QStringLiteral("canvas")).json());
        if (!base)
            return Result<Values>(withInput(base.errors(), QStringLiteral("canvas")));
        const Result<int> blend = intInput(context, "blend", 0, kMaxBlend);
        if (!blend)
            return Result<Values>(blend.errors());
        // Onto (1.6.0): the picture is a layer mask, not a layer, so there is no image to read.
        const QString onto = context.input(QStringLiteral("onto")).text().trimmed();
        // Onto the pixels (1.7.0): the picture goes into the named layer's own pixels instead of a new layer.
        const QString ontoPixels = context.input(QStringLiteral("onto_pixels")).text().trimmed();
        Result<Pixels> redrawn = Pixels();
        if (onto.isEmpty()) {
            redrawn = pixelsOf(context.input(QStringLiteral("image")), QStringLiteral("image"));
            if (!redrawn)
                return Result<Values>(redrawn.errors());
        }
        // "Fill" (the default) or nothing: named from the prompt upstream, else "Fill n" (below).
        QString name = context.input(QStringLiteral("name")).text().trimmed();
        const bool autoName = name.isEmpty() || name == QLatin1String("Fill");
        if (autoName)
            name = promptName(upstreamPrompt(context.document, context.node));

        // The layer is written at the tree level (the base tree and one new layer record on top), so a group in the canvas stays a group.
        CanvasStore store = CanvasStore::forSession();
        const Result<canvas::Tree> baseTree = store.loadTree(base->canvas, base->revision);
        if (!baseTree)
            return Result<Values>(baseTree.errors());
        const int width = baseTree->width, height = baseTree->height;

        CropBox box{0, 0, width, height};
        Pixels mask(width, height, 1, 1.0f);
        std::optional<CropSize> shape;
        if (const Value stitch = context.input(QStringLiteral("stitch")); !stitch.isEmpty()) {
            const Result<StitchDescriptor> described = readStitchHandle(stitch, context.hashes);
            if (!described)
                return Result<Values>(described.errors());
            box = described->box;
            shape = described->size;
            if (box.x + box.width > width || box.y + box.height > height)
                return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("stitch"),
                                      QStringLiteral("The stitch's box lies outside the canvas (%1 x %2).").arg(width).arg(height));
            const Value own = context.input(QStringLiteral("mask"));
            Result<Pixels> read = maskOf(own.isMedia() ? own : Value(described->mask), own.isMedia() ? "mask" : "stitch");
            if (!read)
                return Result<Values>(read.errors());
            mask = maskResized(read.value(), width, height);
        } else if (const Value own = context.input(QStringLiteral("mask")); own.isMedia()) {
            const Result<Pixels> read = maskOf(own, QStringLiteral("mask"));
            if (!read)
                return Result<Values>(read.errors());
            mask = maskResized(read.value(), width, height);
        }
        if (!onto.isEmpty() && !ontoPixels.isEmpty())
            return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("onto_pixels"),
                                  QStringLiteral("Onto (a layer mask) and Onto the pixels of layer are both set: a write goes to one of them."),
                                  QStringLiteral("Empty one of the two."));
        if (!onto.isEmpty())
            return writeLayerMask(context, base.value(), baseTree.value(), onto, box, mask);
        if (shape && !shapeFits(*shape, redrawn->width, redrawn->height))
            return makeInputError(QStringLiteral("stitch_mismatch"), QStringLiteral("image"),
                                  QStringLiteral("The picture is %1 x %2 and the region was %3 x %4: its shape differs by more than 2%.")
                                      .arg(redrawn->width).arg(redrawn->height).arg(shape->width).arg(shape->height),
                                  QStringLiteral("Keep the Sampler on the region's latent, so the picture keeps its shape."));

        if (!ontoPixels.isEmpty())
            return writeLayerPixels(context, base.value(), baseTree.value(), ontoPixels, box,
                                    placedLayerWithMask(width, height, box, redrawn.value(), mask, blend.value()));
        if (name.isEmpty())
            name = nextFillName(baseTree.value());
        if (const QString suffix = context.input(QStringLiteral("suffix")).text().trimmed(); !suffix.isEmpty())
            name += QLatin1Char(' ') + suffix;
        canvas::LayerRecord layer;
        layer.name = name;
        // Hidden when asked (variations 2 to N of a Generate); a workflow older than 1.5.0 has no input: shown.
        const QJsonValue visible = context.input(QStringLiteral("visible")).json();
        layer.visible = !visible.isBool() || visible.toBool();
        layer.producer = QStringLiteral("canvas-write:") + context.node;
        layer.created = isoTime(utcNow());
        // The redrawn picture whole, with the blend as its layer mask (#226), or baked into its alpha as before.
        Result<canvas::TileMap> pictureTiles = canvas::TileMap{};
        if (context.input(QStringLiteral("bake")).json().toBool()) {
            pictureTiles = store.storeLayerTiles(pixelsToLayer(placedLayer(width, height, box, redrawn.value(), mask, blend.value())));
        } else {
            const PlacedLayer placed = placedLayerWithMask(width, height, box, redrawn.value(), mask, blend.value());
            std::vector<uint8_t> gray(placed.mask.data.size());
            for (size_t i = 0; i < gray.size(); ++i)
                gray[i] = uint8_t(std::lround(std::clamp(placed.mask.data[i], 0.0f, 1.0f) * 255.0f));
            const Result<canvas::TileMap> maskTiles = store.storeLayerTiles(mask::MaskPaint(width, height, gray, 1));
            if (!maskTiles)
                return Result<Values>(maskTiles.errors());
            layer.hasMask = true;
            layer.maskTiles = maskTiles.value();
            pictureTiles = store.storeLayerTiles(pixelsToLayer(placed.layer));
        }
        if (!pictureTiles)
            return Result<Values>(pictureTiles.errors());
        layer.tiles = std::move(pictureTiles.value());

        // The result lands on the head and nowhere else: a revision made from the one this work started at, then the head
        // by compare-and-swap (the store's rule, 14.5). A head that moved meanwhile takes the layer as it is now (layers
        // only add). A canvas that changed size cannot take it: the run says so and writes nothing (#239).
        const auto cannotPlace = [&](const QString &head) {
            return Result<Values>(makeError(QStringLiteral("canvas_head_moved"),
                                            QStringLiteral("The canvas changed size while this ran (its head is now %1), so the new layer could not be placed on it and nothing was written.").arg(head.left(19)),
                                            QStringLiteral("Run again from the canvas as it is now.")));
        };
        const Result<canvas::CanvasInfo> current = store.info(base->canvas);
        if (!current)
            return Result<Values>(current.errors());
        if (current->head != base->revision) {
            const Result<canvas::Tree> there = store.loadTree(base->canvas, current->head);
            if (!there)
                return Result<Values>(there.errors());
            if (there->width != width || there->height != height)
                return cannotPlace(current->head);
        }
        const Result<canvas::Revision> added =
            store.addLayerRevision(base->canvas, base->revision, layer, QStringLiteral("canvas-write:") + context.node);
        if (!added)
            return Result<Values>(added.errors());
        Result<canvas::HeadMove> moved = store.moveHead(base->canvas, base->revision, added->id);
        if (!moved)
            return Result<Values>(moved.errors());
        // Lost the race: place the layer on the head as it is now, as a new revision (the same placement accept does).
        // A later write racing this one is retried against the newer head.
        QString landed = added->id;
        for (int attempt = 0; attempt < 4 && !moved->moved; ++attempt) {
            const Result<canvas::Accepted> placed = store.accept(base->canvas, added->id);
            if (placed) {
                moved = canvas::HeadMove{placed->moved, placed->moved ? placed->head.id : placed->current};
                if (placed->moved)
                    landed = placed->head.id;
            } else {
                // Not placeable after all (the canvas changed size in the meantime): hide the revision, report, write nothing visible.
                store.drop(base->canvas, added->id);
                if (!placed.errors().isEmpty() && placed.errors().first().code == QLatin1String("canvas_conflict"))
                    return cannotPlace(moved->head);
                return Result<Values>(placed.errors());
            }
        }
        if (!moved->moved) {
            store.drop(base->canvas, added->id);
            return Result<Values>(makeError(QStringLiteral("canvas_head_moved"),
                                            QStringLiteral("The canvas kept changing while this ran, so the new layer could not be placed and nothing was written."),
                                            QStringLiteral("Run again from the canvas as it is now.")));
        }
        const Result<QJsonObject> handle = canvas::writeCanvasHandle(context.workDir, {base->canvas, landed});
        if (!handle)
            return Result<Values>(handle.errors());

        Values out;
        out.insert(QStringLiteral("canvas"), Value(handle.value()));
        out.insert(QStringLiteral("accepted"), Value(true));
        out.insert(QStringLiteral("head"), Value(moved->head));
        return Result<Values>(out);
    }

    // Onto the pixels (1.7.0, #314): the picture goes into the pixels of the layer named `name` (the top one of that name that is a paint
    // layer at the top level), inside the stitch's box and through the blended mask: old*(1-m) + picture*m. ONE revision made from the
    // one Region read (`setLayerPixels`), Undo takes it back. The picture is composed once, over the layer as it was read.
    // A head that moved meanwhile is rebased when the layer is untouched there (same name and the very same tiles, found again by
    // content, so a layer added above or below, a rename elsewhere, a mask edit of another layer do not matter): the revision is made
    // again from the new head with the same pixels, up to four times (a write that races this one again). When the layer itself changed
    // (painted, transformed, locked, deleted, the canvas resized) the picture was drawn from pixels that are gone, so nothing is written and
    // the run says `canvas_head_moved`: a person's edit is never overwritten by a stale result (a new layer, which only adds, is different).
    Result<Values> writeLayerPixels(const NodeContext &context, const CanvasHandle &base, const canvas::Tree &tree, const QString &name, const CropBox &,
                                    const PlacedLayer &placed) {
        const int width = tree.width, height = tree.height;
        QStringList names;
        const std::vector<canvas::LayerFace> faces = canvas::layerFaces(tree);
        const int position = canvas::topLayerNamed(faces, name, &names);
        if (position < 0)
            return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("onto_pixels"),
                                  QStringLiteral("The canvas has no layer named \"%1\" at this revision.").arg(name.left(60)),
                                  QStringLiteral("Its layers are: %1.").arg(names.join(QStringLiteral(", ")).left(300)));
        if (const QString why = canvas::noPixelsReason(faces[size_t(position)]); !why.isEmpty())
            return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("onto_pixels"), why);
        CanvasStore store = CanvasStore::forSession();
        const canvas::LayerRecord &layer = tree.layers[size_t(position)];
        if (layer.locked) // said before reading a pixel; the store says it again if it got locked since
            return Result<Values>(canvas::lockedError(layer, QStringLiteral("its pixels cannot be painted or replaced")));
        const Result<mask::MaskPaint> held = canvas::assembleLayer(width, height, 4, layer.tiles, [&](const canvas::TileKey &key, const QString &hash) {
            const mask::IRect rect = canvas::tileRect(key, width, height);
            return store.tilePixels(hash, rect.width, rect.height, 4);
        });
        if (!held)
            return Result<Values>(held.errors());
        const mask::MaskPaint composed = pixelsToLayer(layerWithPicture(layerToPixels(held.value()), placed));
        const QString producer = QStringLiteral("canvas-write:") + context.node;
        const QString here = layer.name;
        const canvas::TileMap tiles = layer.tiles;
        QString from = base.revision;
        int at = position;
        for (int attempt = 0; attempt < 5; ++attempt) {
            const Result<canvas::Commit> done = store.setLayerPixels(base.canvas, from, at, composed, producer);
            if (!done)
                return Result<Values>(done.errors());
            if (done->move.moved || done->move.head == from) { // landed, or the same pixels as there already (nothing to write)
                const QString landed = done->move.moved ? done->revision.id : from;
                const Result<QJsonObject> handle = canvas::writeCanvasHandle(context.workDir, {base.canvas, landed});
                if (!handle)
                    return Result<Values>(handle.errors());
                Values out;
                out.insert(QStringLiteral("canvas"), Value(handle.value()));
                out.insert(QStringLiteral("accepted"), Value(true));
                out.insert(QStringLiteral("head"), Value(landed));
                return Result<Values>(out);
            }
            // The head moved: rebase when the layer is just as it was read, found again on the new head.
            const Result<canvas::Tree> there = store.loadTree(base.canvas, done->move.head);
            if (!there)
                return Result<Values>(makeError(QStringLiteral("canvas_unreadable"),
                                                QStringLiteral("The canvas changed while this ran and the store could not read its new version, so nothing was written."),
                                                QStringLiteral("Run again; if it keeps failing, check the canvas with `fugue canvas show`.")));
            at = -1;
            if (there->width == width && there->height == height)
                for (int i = int(there->layers.size()) - 1; i >= 0 && at < 0; --i)
                    if (there->layers[size_t(i)].name == here && there->layers[size_t(i)].tiles == tiles && !there->layers[size_t(i)].group)
                        at = i;
            if (at < 0)
                return Result<Values>(makeError(QStringLiteral("canvas_head_moved"),
                                                QStringLiteral("The layer \"%1\" changed while this ran, so the picture was not written onto it.").arg(here.left(60)),
                                                QStringLiteral("Run again from the canvas as it is now.")));
            from = done->move.head;
        }
        return Result<Values>(makeError(QStringLiteral("canvas_head_moved"),
                                        QStringLiteral("The canvas kept changing while this ran, so the picture was not written onto \"%1\".").arg(here.left(60)),
                                        QStringLiteral("Try again.")));
    }

    // Onto (1.6.0): `layer_mask` becomes the layer mask of the layer named `onto`, one revision made from the one Region read.
    // Inside the stitch's box the layer's mask moves toward the new one by `coverage` (the stitch's mask, or `mask`; all 1 without
    // a stitch), so a selection limits the change and the rest of the old mask stays. A locked layer refuses (the store's rule, #226).
    Result<Values> writeLayerMask(const NodeContext &context, const CanvasHandle &base, const canvas::Tree &tree, const QString &onto, const CropBox &box,
                                  const Pixels &coverage) {
        const int width = tree.width, height = tree.height;
        int position = -1;
        QStringList names;
        for (int i = int(tree.layers.size()) - 1; i >= 0 && position < 0; --i) { // the top one of that name
            names << tree.layers[size_t(i)].name;
            if (tree.layers[size_t(i)].name == onto && tree.layers[size_t(i)].kind != LayerKind::Picture)
                position = i;
        }
        if (position < 0)
            return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("onto"),
                                  QStringLiteral("The canvas has no layer named \"%1\" at this revision.").arg(onto.left(60)),
                                  QStringLiteral("Its layers are: %1.").arg(names.join(QStringLiteral(", ")).left(300)));
        const Value given = context.input(QStringLiteral("layer_mask"));
        if (!given.isMedia())
            return makeInputError(QStringLiteral("input_missing"), QStringLiteral("layer_mask"), QStringLiteral("Onto needs a Layer mask to put on the layer."),
                                  QStringLiteral("Connect the mask (Remove Background Mask's output) to Layer mask."));
        const Result<Pixels> read = maskOf(given, QStringLiteral("layer_mask"));
        if (!read)
            return Result<Values>(read.errors());
        // The mask where it belongs: the region's size stretched to its box, nothing outside it.
        Pixels placed(width, height, 1, 0.0f);
        const Pixels fitted = maskResized(read.value(), box.width, box.height);
        for (int y = 0; y < box.height; ++y)
            for (int x = 0; x < box.width; ++x)
                placed.at(box.x + x, box.y + y, 0) = fitted.at(x, y, 0);
        CanvasStore store = CanvasStore::forSession();
        const canvas::LayerRecord &layer = tree.layers[size_t(position)];
        Pixels old(width, height, 1, 1.0f); // no mask: everything shows
        if (layer.hasMask) {
            const Result<mask::MaskPaint> held = canvas::assembleLayer(width, height, 1, layer.maskTiles, [&](const canvas::TileKey &key, const QString &hash) {
                const mask::IRect rect = canvas::tileRect(key, width, height);
                return store.tilePixels(hash, rect.width, rect.height, 1);
            });
            if (!held)
                return Result<Values>(held.errors());
            old = layerToPixels(held.value());
        }
        std::vector<uint8_t> gray(size_t(width) * size_t(height));
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const float cover = std::clamp(coverage.at(x, y, 0), 0.0f, 1.0f);
                const float value = old.at(x, y, 0) * (1 - cover) + placed.at(x, y, 0) * cover;
                gray[size_t(y) * size_t(width) + size_t(x)] = uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            }
        const Result<canvas::Commit> done = store.setLayerMask(base.canvas, base.revision, position, mask::MaskPaint(width, height, gray, 1),
                                                               QStringLiteral("canvas-write:") + context.node);
        if (!done)
            return Result<Values>(done.errors());
        // A head that moved meanwhile: the mask is for the layer as it was read, so nothing is written (a mask never "only adds").
        if (!done->move.moved && done->move.head != base.revision)
            return Result<Values>(makeError(QStringLiteral("canvas_head_moved"),
                                            QStringLiteral("The canvas changed while this ran (its head is now %1), so the layer mask was not written.").arg(done->move.head.left(19)),
                                            QStringLiteral("Run again from the canvas as it is now.")));
        const QString landed = done->move.moved ? done->revision.id : base.revision; // a mask equal to the one there changes nothing
        const Result<QJsonObject> handle = canvas::writeCanvasHandle(context.workDir, {base.canvas, landed});
        if (!handle)
            return Result<Values>(handle.errors());
        Values out;
        out.insert(QStringLiteral("canvas"), Value(handle.value()));
        out.insert(QStringLiteral("accepted"), Value(true));
        out.insert(QStringLiteral("head"), Value(landed));
        return Result<Values>(out);
    }
};

// Canvas New (1.0.0): a picture becomes a canvas of its own, one layer, at the picture's size. The source canvas is not read.
class CanvasNewKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> picture = pixelsOf(context.input(QStringLiteral("image")), QStringLiteral("image"));
        if (!picture)
            return Result<Values>(picture.errors());
        if (const QString why = mask::canvasSizeProblem(picture->width, picture->height); !why.isEmpty())
            return makeInputError(QStringLiteral("canvas_too_big"), QStringLiteral("image"),
                                  QStringLiteral("The picture is %1 x %2, which is too big for a canvas. %3").arg(picture->width).arg(picture->height).arg(why),
                                  QStringLiteral("Choose a smaller scale."));
        const mask::MaskPaint layer = pixelsToLayer(picture.value());
        QString base = context.input(QStringLiteral("name")).text().trimmed().left(120);
        // The one layer is named as the canvas is asked to be (not "Layer 1"), without the number a taken name gets; no name: Layer 1.
        const Result<mask::PaintDocument> document = mask::canvasFromPixels(picture->width, picture->height, layer.pixels(), base.isEmpty() ? QStringLiteral("Layer 1") : base.left(64));
        if (!document)
            return Result<Values>(document.errors());
        CanvasStore store = CanvasStore::forSession();
        if (base.isEmpty())
            base = QStringLiteral("Untitled");
        QSet<QString> taken;
        for (const canvas::CanvasInfo &info : store.list())
            taken.insert(info.name);
        QString name = base;
        for (int n = 2; taken.contains(name); ++n)
            name = QStringLiteral("%1 %2").arg(base).arg(n);
        const Result<canvas::CanvasInfo> made = store.create(name, document.value(), QStringLiteral("canvas-new:") + context.node);
        if (!made)
            return Result<Values>(made.errors());
        const Result<QJsonObject> handle = canvas::writeCanvasHandle(context.workDir, {made->id, made->head});
        if (!handle)
            return Result<Values>(handle.errors());
        Values out;
        out.insert(QStringLiteral("canvas"), Value(handle.value()));
        out.insert(QStringLiteral("id"), Value(made->id));
        return Result<Values>(out);
    }
};

} // namespace

void addCanvasKernels(Kernels &kernels) {
    kernels.add("fugue/canvas-region", std::make_shared<CanvasRegionKernel>());
    kernels.add("fugue/canvas-write", std::make_shared<CanvasWriteKernel>());
    kernels.add("fugue/canvas-new", std::make_shared<CanvasNewKernel>());
}

} // namespace fugue::nodes
