#include "engine/compile_for_run.h"

#include "engine/group_library.h"

namespace fugue {

RunCompileSetup runCompileSetup(std::function<Errors(Registry &)> registerProviders, const TrustStore &trust) {
    RunCompileSetup setup;
    setup.registerProviders = std::move(registerProviders);
    setup.trusted = [trust](const QString &pack, const QString &version) { return trust.isTrusted(pack, version); };
    return setup;
}

Compilation compileForRun(const Document &document, Registry &registry, const RunCompileRequest &request,
                          const RunCompileSetup &setup) {
    Errors providerProblems;
    if (setup.registerProviders) {
        providerProblems = setup.registerProviders(registry);
        if (hasErrors(providerProblems))
            return Compilation(providerProblems);
    }
    CompileOptions options;
    options.target = request.target;
    options.provider = request.provider;
    options.entered = request.entered;
    if (setup.registerProviders)
        options.providers = registry.providerNames(); // without providers, a pin is not checked
    options.trusted = setup.trusted;
    // A library or pack group named only by its type id runs from its definition (5.3, 6.1).
    Document embedded = document;
    embedGroupTypes(embedded, registry);
    const Compilation compilation = compile(embedded, registry, options);
    const Errors problems = providerProblems + compilation.problems();
    return compilation ? Compilation(compilation.value(), problems) : Compilation(problems);
}

} // namespace fugue
