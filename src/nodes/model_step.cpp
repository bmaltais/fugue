#include "nodes/model_step.h"

#include "engine/file_hashes.h"
#include "engine/model_library.h"

namespace fugue::nodes {

int sizeStepFor(const std::optional<ModelShape> &diffusionModel) {
    return diffusionModel && diffusionModel->sizeStep > 0 ? diffusionModel->sizeStep : kDefaultSizeStep;
}

namespace {

// The file a `fugue/model` input's diffusion model (or checkpoint) is: a derived model names it as `base`; a plain handle is the file itself.
Result<QString> baseFile(const NodeContext &context, const QJsonObject &model, ModelLibrary::Hashing hashing) {
    const QJsonObject base = model.contains(QStringLiteral("derived"))
                                 ? model.value(QStringLiteral("derived")).toObject().value(QStringLiteral("base")).toObject()
                                 : model.contains(QStringLiteral("base")) ? model.value(QStringLiteral("base")).toObject() : model;
    std::optional<ModelLibrary> own; // none is given outside a check pass or the cache key's
    const ModelLibrary &library = context.library ? *context.library : own.emplace(ModelLibrary::current(context.registry));
    return library.resolve(base, *context.hashes, hashing);
}

} // namespace

int modelSizeStep(const NodeContext &context, const QJsonObject &model, ModelLibrary::Hashing hashing) {
    if (model.isEmpty() || !context.hashes)
        return kDefaultSizeStep;
    const Result<QString> path = baseFile(context, model, hashing);
    if (!path)
        return kDefaultSizeStep;
    return sizeStepFor(ModelShapes::shared().of(path.value()));
}

QString sizeStepKey(const NodeContext &context) {
    if (!context.inputs.contains(QStringLiteral("model")))
        return {};
    const QJsonObject model = context.input(QStringLiteral("model")).json().toObject();
    if (model.isEmpty() || !context.hashes)
        return {};
    // The key reads no hash (RememberedOnly): a file with the handle's name is taken to be it. Until its hash is remembered that
    // may be another file than the run finds, so the file's own path is in the key then; a run's result is never stored under the
    // step of a file it did not use.
    const Result<QString> path = baseFile(context, model, ModelLibrary::Hashing::RememberedOnly);
    if (!path)
        return {};
    const int step = sizeStepFor(ModelShapes::shared().of(path.value()));
    const bool verified = !context.hashes->known(path.value()).isEmpty();
    if (verified)
        return step == kDefaultSizeStep ? QString() : QStringLiteral("size_step:%1").arg(step);
    return QStringLiteral("size_step:%1@%2").arg(step).arg(path.value());
}

} // namespace fugue::nodes
