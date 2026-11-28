#include "engine/compile_for_run.h"

#include "engine/group_library.h"

namespace fugue {

RunCompileSetup runCompileSetup(std::function<Errors(Registry &)> registerProviders, const TrustStore &trust,
                                std::function<QString()> shownCanvas) {
    RunCompileSetup setup;
    setup.registerProviders = std::move(registerProviders);
    setup.trusted = [trust](const QString &pack, const QString &version) { return trust.isTrusted(pack, version); };
    setup.resolveCanvas = sessionCanvasResolver();
    setup.activeCanvas = [shownCanvas = std::move(shownCanvas)] { return activeCanvasOrNewest(shownCanvas ? shownCanvas() : QString()); };
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
    options.from = request.from;
    options.provider = request.provider;
    options.entered = request.entered;
    options.overrides = request.overrides;
    if (setup.registerProviders)
        options.providers = registry.providerNames(); // without providers, a pin is not checked
    options.trusted = setup.trusted;
    // A library or pack group named only by its type id runs from its definition (5.3, 6.1).
    Document embedded = document;
    embedGroupTypes(embedded, registry);
    const Compilation compilation = compile(embedded, registry, options);
    if (!compilation)
        return Compilation(providerProblems + compilation.problems());
    // After the compile, so a group's inner nodes (path ids) are pinned too, and only a graph that
    // can run is looked up in the store.
    CompiledGraph graph = compilation.value();
    const Errors canvasProblems = resolveCanvasRevisions(graph, setup.resolveCanvas, setup.activeCanvas);
    const Errors problems = providerProblems + compilation.problems() + canvasProblems;
    return hasErrors(canvasProblems) ? Compilation(problems) : Compilation(graph, problems);
}

} // namespace fugue
