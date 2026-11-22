#include "nodes/stitch_handle.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace fugue::nodes {

Result<MediaHandle> namedHandle(const MediaHandle &handle, const NodeContext &context, const QString &name) {
    if (handle.hasFile() && !handle.hash().isEmpty())
        return handle;
    if (!context.hashes)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("The %1 has no file to name.").arg(name));
    return handle.saved(context.workDir, name, *context.hashes);
}

Result<Value> writeStitchHandle(const QString &workDir, const StitchDescriptor &d) {
    const QJsonObject meta{
        {"image", d.image.toJson()},
        {"mask", d.mask.toJson()},
        {"box", QJsonObject{{"x", d.box.x}, {"y", d.box.y}, {"width", d.box.width}, {"height", d.box.height}}},
        {"size", QJsonObject{{"width", d.size.width}, {"height", d.size.height}}}};
    const QByteArray bytes = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    const QString path = QDir(workDir).filePath(QStringLiteral("stitch.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
        return makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot write %1.").arg(path));
    return Value(QJsonObject{{"data", kStitchType}, {"path", path}, {"meta", meta}});
}

Result<StitchDescriptor> readStitchHandle(const Value &stitch, FileHashes *hashes) {
    const QJsonObject meta = stitch.json().toObject().value(QStringLiteral("meta")).toObject();
    const auto bad = [] {
        return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("stitch"),
                              QStringLiteral("This is not the stitch handle of a Crop to Mask or a Canvas Region."),
                              QStringLiteral("Wire a stitch output to this input."));
    };
    if (stitch.json().toObject().value(QStringLiteral("data")).toString() != kStitchType)
        return bad();
    StitchDescriptor d;
    const Result<MediaHandle> image = MediaHandle::fromJson(meta.value(QStringLiteral("image")).toObject());
    const Result<MediaHandle> mask = MediaHandle::fromJson(meta.value(QStringLiteral("mask")).toObject());
    if (!image || !mask)
        return bad();
    d.image = image.value();
    d.mask = mask.value();
    const QJsonObject box = meta.value(QStringLiteral("box")).toObject();
    const QJsonObject size = meta.value(QStringLiteral("size")).toObject();
    d.box = {box.value("x").toInt(), box.value("y").toInt(), box.value("width").toInt(), box.value("height").toInt()};
    d.size = {size.value("width").toInt(), size.value("height").toInt()};
    if (d.box.width <= 0 || d.box.height <= 0 || d.size.width <= 0 || d.size.height <= 0 || d.box.x < 0 || d.box.y < 0)
        return bad();
    if (hashes)
        for (const MediaHandle &file : {d.image, d.mask})
            if (hashes->hash(file.path()) != file.hash())
                return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("stitch"),
                                      QStringLiteral("The picture or mask %1 is gone or changed since the stitch was made.").arg(file.path()),
                                      QStringLiteral("Run the node that made the stitch again."));
    return d;
}

} // namespace fugue::nodes
