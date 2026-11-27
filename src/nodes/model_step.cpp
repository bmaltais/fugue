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
    std::optional<ModelLibrary> own; // none is given outside a check pass or the cache key's
    const ModelLibrary &library = context.library ? *context.library : own.emplace(ModelLibrary::current(context.registry));
    const Result<QString> path = library.resolve(base, *context.hashes);
    if (!path)
        return kDefaultSizeStep;
    return sizeStepFor(ModelShapes::shared().of(path.value()));
}

QString sizeStepKey(const NodeContext &context) {
    if (!context.inputs.contains(QStringLiteral("model")))
        return {};
    const int step = modelSizeStep(context, context.input(QStringLiteral("model")).json().toObject());
    return step == kDefaultSizeStep ? QString() : QStringLiteral("size_step:%1").arg(step);
}

} // namespace fugue::nodes
