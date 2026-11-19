#include "cli/compile_step.h"

namespace fugue::cli {

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

Errors validateDocument(const Document &document, const Registry &registry, const RunEnvironment &environment) {
    const Compilation compilation = compileDocument(document, registry);
    Errors errors = compilation.errors;
    if (compilation.ok())
        errors += checkGraph(*compilation.graph, environment).warnings;
    return errors;
}

} // namespace fugue::cli
