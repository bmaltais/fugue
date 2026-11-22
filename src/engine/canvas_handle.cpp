#include "canvas_handle.h"

#include <QDir>

#include "engine/atomic_file.h"
#include "engine/canonical.h"
#include "engine/canvas_store.h"
#include "engine/hash.h"

namespace fugue::canvas {

namespace {

Error invalid(const QString &why) {
    return makeError(QStringLiteral("input_invalid"), QStringLiteral("This is not a canvas handle: %1.").arg(why),
                     QStringLiteral("Wire a canvas output to this input."));
}

} // namespace

QJsonObject canvasHandleMeta(const CanvasHandle &handle) { return QJsonObject{{"canvas", handle.canvas}, {"revision", handle.revision}}; }

Result<CanvasHandle> canvasHandleFrom(const QJsonValue &value) {
    QJsonObject meta = value.toObject();
    if (meta.contains(QStringLiteral("data"))) {
        if (meta.value(QStringLiteral("data")).toString() != QLatin1String(canvasHandleType))
            return invalid(QStringLiteral("its type is not %1").arg(QLatin1String(canvasHandleType)));
        meta = meta.value(QStringLiteral("meta")).toObject();
    }
    if (meta.keys().size() != 2 || !meta.value(QStringLiteral("canvas")).isString() || !meta.value(QStringLiteral("revision")).isString())
        return invalid(QStringLiteral("it must hold a canvas and a revision and nothing else"));
    CanvasHandle handle{meta.value(QStringLiteral("canvas")).toString(), meta.value(QStringLiteral("revision")).toString()};
    if (!isCanvasId(handle.canvas) || !isSha256(handle.revision))
        return invalid(QStringLiteral("the canvas id or the revision hash is malformed"));
    return handle;
}

QByteArray canvasHandleBytes(const CanvasHandle &handle) { return canonicalJson(canvasHandleMeta(handle)).value_or(QByteArray()); }

Result<QJsonObject> writeCanvasHandle(const QString &workDir, const CanvasHandle &handle) {
    if (!canvasHandleFrom(canvasHandleMeta(handle)))
        return invalid(QStringLiteral("the canvas id or the revision hash is malformed"));
    const QString path = QDir(workDir).filePath(QStringLiteral("canvas.json"));
    const Errors written = replaceFile(path, canvasHandleBytes(handle), QStringLiteral("canvas.json"));
    if (hasErrors(written))
        return Result<QJsonObject>(written);
    return QJsonObject{{"data", QLatin1String(canvasHandleType)}, {"path", path}, {"meta", canvasHandleMeta(handle)}};
}

} // namespace fugue::canvas
