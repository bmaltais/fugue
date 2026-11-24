#include "nodes/upscale_model.h"

#include <QFileInfo>

#include "engine/file_hashes.h"
#include "engine/model_library.h"

namespace fugue::nodes {

Requirement upscaleRequirement() {
    Requirement requirement;
    requirement.capability = QStringLiteral("image.upscale");
    return requirement;
}

Result<QJsonValue> modelFile(const NodeContext &context, const Selection &selection, const QString &input,
                             const QString &missingMessage, const QString &missingHint) {
    const QJsonObject model = context.input(input).json().toObject();
    if (model.isEmpty())
        return makeInputError(QStringLiteral("missing_input"), input, missingMessage, missingHint);
    const ModelLibrary library = ModelLibrary::current(context.registry);
    if (selection.sharesFilesystem) {
        const Result<QString> found = library.resolve(model, *context.hashes);
        if (!found) {
            Error missing = found.error();
            missing.input = input;
            return missing;
        }
        return QJsonValue(found.value());
    }
    // a paired host finds the file in its own folders (8.10)
    QJsonObject sent{{"name", model.value(QStringLiteral("name")).toString()}};
    if (const QString hash = model.value(QStringLiteral("hash")).toString(); !hash.isEmpty())
        sent.insert(QStringLiteral("sha256"), hash);
    if (const Result<QString> found = library.resolve(model, *context.hashes, ModelLibrary::Hashing::RememberedOnly))
        sent.insert(QStringLiteral("size"), double(QFileInfo(found.value()).size()));
    return QJsonValue(sent);
}

Result<QJsonObject> upscaleParams(const NodeContext &context, const Selection &selection) {
    const Result<QJsonValue> modelPath =
        modelFile(context, selection, QStringLiteral("model"), QStringLiteral("No upscale model is picked."),
                  QStringLiteral("Pick one in the node, or wire a Load Upscale Model."));
    if (!modelPath)
        return modelPath.errors();
    return QJsonObject{{"context", QJsonObject{{"model_path", modelPath.value()}}},
                       {"tile_size", int(context.input(QStringLiteral("tile")).number())}};
}

} // namespace fugue::nodes
