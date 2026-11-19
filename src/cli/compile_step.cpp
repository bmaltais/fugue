#include "cli/compile_step.h"

#include <QEventLoop>
#include <QTimer>

namespace fugue::cli {

namespace {

// The capability a node type asks for: the first of its `capabilities` (8.1).
QString capabilityOf(const NodeType &type) {
    const QJsonArray list = type.capabilities.toArray();
    return list.isEmpty() ? QString() : list.at(0).toString();
}

bool asksForCapability(const CompiledGraph &graph, const Registry &registry) {
    for (const CompiledNode &node : graph.nodes) {
        const NodeType *type = registry.nodeType(node.type);
        if (type && !capabilityOf(*type).isEmpty())
            return true;
    }
    return false;
}

// describe() on each enabled instance, waiting for every reply (or a time limit).
QList<Candidate> describeInstances(const QList<ProviderInstance> &instances) {
    QList<Candidate> candidates;
    QList<std::shared_ptr<Provider>> keep;
    int pending = 0;
    QEventLoop loop;
    for (const ProviderInstance &instance : instances) {
        Candidate candidate;
        candidate.instance = instance;
        candidates.append(candidate);
    }
    for (int i = 0; i < candidates.size(); ++i) {
        if (!candidates.at(i).instance.enabled)
            continue;
        Result<std::shared_ptr<Provider>> provider = createProvider(candidates.at(i).instance);
        if (!provider) {
            candidates[i].description = Result<Description>(provider.errors());
            continue;
        }
        keep.append(provider.value());
        ++pending;
        provider.value()->describe([&, i](Result<Description> description) {
            candidates[i].description = std::move(description);
            if (--pending == 0)
                loop.quit();
        });
    }
    if (pending > 0) {
        QTimer::singleShot(10000, &loop, [&] { loop.quit(); });
        loop.exec();
    }
    return candidates;
}

} // namespace

Compilation compileDocument(const Document &document, const Registry &registry, const CompileSettings &settings) {
    const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath());
    if (!instances) {
        Compilation failed;
        failed.errors = instances.errors();
        return failed;
    }
    CompileOptions options;
    options.target = settings.target;
    options.provider = settings.provider;
    QStringList names;
    for (const ProviderInstance &instance : instances.value())
        names << instance.name;
    options.providers = names;
    return compile(document, registry, options);
}

Errors validateDocument(const Document &document, const Registry &registry) {
    const Compilation compilation = compileDocument(document, registry);
    Errors errors = compilation.errors;
    if (!compilation.ok() || !asksForCapability(*compilation.graph, registry))
        return errors;
    const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath());
    if (!instances)
        return errors;
    errors += providerWarnings(*compilation.graph, registry, describeInstances(instances.value()));
    return errors;
}

Errors providerWarnings(const CompiledGraph &graph, const Registry &registry, const QList<Candidate> &candidates) {
    Errors warnings;
    for (auto it = graph.nodes.cbegin(); it != graph.nodes.cend(); ++it) {
        const NodeType *type = registry.nodeType(it->type);
        if (!type)
            continue;
        const QString capability = capabilityOf(*type);
        if (capability.isEmpty())
            continue;
        Requirement requirement;
        requirement.capability = capability;
        requirement.nodePin = it->provider; // the compiler already folded the workflow pin in
        const auto model = it->inputs.constFind("model");
        if (model != it->inputs.cend() && model->kind() == InputValue::Kind::Resource)
            requirement.model = model->handle();
        const Result<Selection> selection = selectProvider(candidates, requirement);
        if (selection)
            continue;
        for (Error error : selection.errors()) {
            error.severity = Error::Severity::Warning;
            error.node = it.key();
            warnings.append(error);
        }
    }
    return warnings;
}

} // namespace fugue::cli
