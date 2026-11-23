#include "engine/canvas_psd.h"

#include <QFile>
#include <QFileInfo>
#include <QSize>

#include <functional>

#include "engine/canvas_layer_ops.h"
#include "engine/paint_file.h"

namespace fugue::canvas {

namespace {

using Reader = std::function<Result<mask::MaskPaint>(const QString &, int, int, int)>;

// The pixels and layer mask of one record, read through the document code (the tiles, the mask) with a group's children left out.
Result<bool> fillFromRecord(const Tree &tree, LayerRecord record, const Reader &read, mask::PsdLayer &layer) {
    record.children.clear();
    record.group = false;
    Tree one;
    one.width = tree.width;
    one.height = tree.height;
    one.layers.push_back(std::move(record));
    const Result<mask::PaintDocument> document = documentOf(one, read);
    if (!document)
        return Result<bool>(document.errors());
    const int id = document->layer(0).id;
    if (const mask::MaskPaint *pixels = document->pixels(id))
        layer.pixels = *pixels;
    if (layer.info.hasMask)
        if (const mask::MaskPaint *mask = document->maskPixels(id))
            layer.mask = *mask;
    return true;
}

Result<bool> build(const Tree &tree, const std::vector<LayerRecord> &records, const Reader &read, std::vector<mask::PsdLayer> &out, QStringList &notes) {
    for (const LayerRecord &record : records) {
        if (record.kind == mask::LayerKind::Mask && !record.group) {
            notes << QStringLiteral("The mask layer \"%1\" is a selection, not a picture: it is not in the Photoshop file.").arg(record.name);
            continue;
        }
        mask::PsdLayer layer;
        layer.info.name = record.name;
        layer.info.visible = record.visible;
        layer.info.opacity = record.opacity;
        layer.info.compositeOp = record.compositeOp;
        layer.info.hasMask = record.hasMask;
        layer.info.maskEnabled = record.maskEnabled;
        layer.group = record.group;
        const Result<bool> filled = fillFromRecord(tree, record, read, layer);
        if (!filled)
            return filled;
        if (record.group) {
            const Result<bool> inner = build(tree, record.children, read, layer.children, notes);
            if (!inner)
                return inner;
        }
        out.push_back(std::move(layer));
    }
    return true;
}

// Loads the leaves of `level` into `document` and records, in `edits`, how to group them (inner groups first). `items` is
// the document's top-level list as it will be when the edits run: one entry per layer or group.
void place(const std::vector<mask::PsdLayer> &level, mask::PaintDocument &document, std::vector<LayerEdit> &edits, std::vector<int> &items, Errors &warnings) {
    for (const mask::PsdLayer &layer : level) {
        if (!layer.group) {
            document.loadLayer(layer.info, layer.pixels, layer.mask ? &*layer.mask : nullptr);
            items.push_back(int(items.size()));
            continue;
        }
        const size_t before = items.size();
        place(layer.children, document, edits, items, warnings);
        const int count = int(items.size() - before);
        if (count == 0)
            continue;
        LayerEdit group;
        group.op = LayerOp::Group;
        group.text = layer.info.name;
        for (size_t i = before; i < items.size(); ++i)
            group.layers.push_back(int(i));
        edits.push_back(group);
        items.resize(before);
        items.push_back(int(before));
        const int at = int(before);
        if (!layer.info.visible) {
            LayerEdit hide;
            hide.op = LayerOp::Visible;
            hide.layers = {at};
            hide.flag = false;
            edits.push_back(hide);
        }
        if (layer.info.opacity < 1) {
            LayerEdit fade;
            fade.op = LayerOp::Opacity;
            fade.layers = {at};
            fade.number = layer.info.opacity;
            edits.push_back(fade);
        }
        bool known = true;
        const mask::Blend blend = mask::blendOfOp(layer.info.compositeOp, &known);
        if (!known) {
            Error w = makeError(QStringLiteral("psd_dropped"), QStringLiteral("The group \"%1\" has a blend mode Fugue does not have: it shows as Normal.").arg(layer.info.name));
            w.severity = Error::Severity::Warning;
            warnings << w;
        } else if (blend != mask::Blend::Normal) {
            LayerEdit mode;
            mode.op = LayerOp::Blend;
            mode.layers = {at};
            mode.text = mask::blendId(blend);
            edits.push_back(mode);
        }
    }
}

} // namespace

Result<mask::PsdExported> psdOfTree(const Tree &tree, const Reader &read) {
    mask::PsdImage image;
    image.width = tree.width;
    image.height = tree.height;
    QStringList notes;
    const Result<bool> built = build(tree, tree.layers, read, image.layers, notes);
    if (!built)
        return Result<mask::PsdExported>(built.errors());
    const Result<mask::PaintDocument> drawn = documentOf(tree, read);
    if (!drawn)
        return Result<mask::PsdExported>(drawn.errors());
    image.merged = drawn->flatten();
    Result<mask::PsdExported> written = mask::paintToPsd(image);
    if (!written)
        return written;
    mask::PsdExported out = written.value();
    out.notes = notes + out.notes;
    return out;
}

Result<ExportedPicture> exportRevisionAsPsd(const CanvasStore &store, const QString &canvasId, const QString &revisionId, const PictureExport &options) {
    const Result<Tree> tree = store.loadTree(canvasId, revisionId);
    if (!tree)
        return Result<ExportedPicture>(tree.errors());
    const Result<QSize> size = exportSize(tree->width, tree->height, options);
    if (!size)
        return Result<ExportedPicture>(size.errors());
    const bool resized = size->width() != tree->width || size->height() != tree->height;
    if (!treeHasGroup(tree.value()) || resized) {
        const Result<mask::PaintDocument> document = store.load(canvasId, revisionId);
        if (!document)
            return Result<ExportedPicture>(document.errors());
        Result<ExportedPicture> picture = exportPicture(document.value(), options);
        if (picture && resized && treeHasGroup(tree.value()))
            picture->notes << QStringLiteral("The canvas was resized, so its groups were written as one layer each.");
        return picture;
    }
    const Result<mask::PsdExported> psd = psdOfTree(tree.value(), [&store](const QString &hash, int w, int h, int c) { return store.tilePixels(hash, w, h, c); });
    if (!psd)
        return Result<ExportedPicture>(psd.errors());
    ExportedPicture out;
    out.bytes = psd->bytes;
    out.notes = psd->notes;
    out.width = tree->width;
    out.height = tree->height;
    out.format = QStringLiteral("psd");
    return out;
}

Result<CanvasInfo> importPsd(CanvasStore &store, const QString &name, const QByteArray &bytes, const QString &producer, int *width, int *height) {
    const Result<mask::PsdImported> read = mask::paintFromPsdSettled(bytes);
    if (!read)
        return Result<CanvasInfo>(read.errors());
    Errors warnings = read.warnings();
    mask::PaintDocument document(read->width, read->height);
    std::vector<LayerEdit> edits;
    std::vector<int> items;
    place(read->layers, document, edits, items, warnings);
    if (document.layerCount() == 0 && read->merged)
        document.loadLayer(mask::LayerKind::Paint, QStringLiteral("Background"), true, 1.0, *read->merged);
    document.forgetHistory();
    if (width)
        *width = document.width();
    if (height)
        *height = document.height();
    const Result<CanvasInfo> made = store.createWithEdits(name, document, edits, producer);
    if (!made)
        return made;
    return Result<CanvasInfo>(made.value(), warnings);
}

Result<CanvasInfo> createCanvasFromFile(CanvasStore &store, const QString &name, const QString &path, const QString &producer, int *width, int *height) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) && mask::looksLikePsd(file.peek(4)) && QFileInfo(path).size() <= mask::paintFileMaxBytes)
        return importPsd(store, name, file.read(mask::paintFileMaxBytes + 1), producer, width, height);
    const Result<mask::PaintDocument> document = mask::readCanvasFile(path);
    if (!document)
        return Result<CanvasInfo>(document.errors());
    if (width)
        *width = document->width();
    if (height)
        *height = document->height();
    const Result<CanvasInfo> made = store.create(name, document.value(), producer);
    if (!made)
        return made;
    return Result<CanvasInfo>(made.value(), document.warnings());
}

} // namespace fugue::canvas
