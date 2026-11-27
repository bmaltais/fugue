#include "nodes/model_step.h"

#include "engine/model_library.h"

namespace fugue::nodes {

int sizeStepFor(const std::optional<ModelShape> &diffusionModel) {
    return diffusionModel && diffusionModel->unet.value_or(false) ? kUnetSizeStep : kDefaultSizeStep;
}

int modelSizeStep(const NodeContext &context, const QJsonObject &model) {
    if (model.isEmpty() || !context.hashes)
        return kDefaultSizeStep;
    // A derived model (2.5) names its diffusion model as `base`; a plain handle is the file itself.
    const QJsonObject base = model.contains(QStringLiteral("derived"))
                                 ? model.value(QStringLiteral("derived")).toObject().value(QStringLiteral("base")).toObject()
                                 : model.contains(QStringLiteral("base")) ? model.value(QStringLiteral("base")).toObject() : model;
    const ModelLibrary library = ModelLibrary::current(context.registry);
    const Result<QString> path = library.resolve(base, *context.hashes);
    if (!path)
        return kDefaultSizeStep;
    return sizeStepFor(ModelShapes::shared().of(path.value()));
}

} // namespace fugue::nodes
