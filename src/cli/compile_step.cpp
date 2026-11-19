#include "cli/compile_step.h"

#include "cli/process.h"

namespace fugue::cli {

Compilation compileDocument(const Document &document, Registry &registry, const CompileSettings &settings) {
    const Errors providerProblems = registerProviders(registry);
    if (hasErrors(providerProblems)) {
        Compilation failed;
        failed.errors = providerProblems;
        return failed;
    }
    CompileOptions options;
    options.target = settings.target;
    options.provider = settings.provider;
    options.providers = registry.providerNames();
    Compilation compilation = compile(document, registry, options);
    compilation.errors = providerProblems + compilation.errors; // warnings only
    return compilation;
}

Validation validateDocument(const Document &document, Context &context) {
    const Compilation compilation = compileDocument(document, context.registry);
    Validation validation;
    validation.errors = compilation.errors;
    if (!compilation.ok())
        return validation;
    const Process process(context);
    const GraphCheck check = checkGraph(*compilation.graph, process.environment);
    validation.errors += check.warnings;
    for (auto it = check.states.begin(); it != check.states.end(); ++it)
        validation.nodes.insert(it.key(), QJsonObject{{"state", nodeStateName(it.value())}});
    for (auto it = compilation.graph->skipped.begin(); it != compilation.graph->skipped.end(); ++it)
        validation.nodes.insert(it.key(), QJsonObject{{"state", nodeStateName(NodeState::Skipped)}, {"reason", it.value()}});
    return validation;
}

} // namespace fugue::cli
