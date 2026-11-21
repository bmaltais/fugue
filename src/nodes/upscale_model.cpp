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

Result<QJsonObject> upscaleParams(const NodeContext &context, const Selection &selection) {
    const QJsonObject model = context.input(QStringLiteral("model")).json().toObject();
    if (model.isEmpty())
        return makeInputError(QStringLiteral("missing_input"), QStringLiteral("model"),
                              QStringLiteral("No upscale model is picked."),
                              QStringLiteral("Pick one in the node, or wire a Load Upscale Model."));
    QJsonValue modelPath;
    const ModelLibrary library = ModelLibrary::current(context.registry);
    if (selection.sharesFilesystem) {
        const Result<QString> found = library.resolve(model, *context.hashes);
        if (!found) {
            Error missing = found.error();
            missing.input = QStringLiteral("model");
            return missing;
        }
        modelPath = found.value();
    } else { // a paired host finds the file in its own folders (8.10)
        QJsonObject sent{{"name", model.value(QStringLiteral("name")).toString()}};
        if (const QString hash = model.value(QStringLiteral("hash")).toString(); !hash.isEmpty())
            sent.insert(QStringLiteral("sha256"), hash);
        if (const Result<QString> found = library.resolve(model, *context.hashes, ModelLibrary::Hashing::RememberedOnly))
            sent.insert(QStringLiteral("size"), double(QFileInfo(found.value()).size()));
        modelPath = sent;
    }
    return QJsonObject{{"context", QJsonObject{{"model_path", modelPath}}},
                       {"tile_size", int(context.input(QStringLiteral("tile")).number())}};
}

} // namespace fugue::nodes
