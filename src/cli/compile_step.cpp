#include "cli/compile_step.h"

#include <QFileInfo>

#include "cli/process.h"

namespace fugue::cli {

Compilation compileDocument(const Document &document, Registry &registry, const CompileSettings &settings) {
    const Errors providerProblems = registerProviders(registry);
    if (hasErrors(providerProblems))
        return Compilation(providerProblems);
    CompileOptions options;
    options.target = settings.target;
    options.provider = settings.provider;
    options.providers = registry.providerNames();
    const Compilation compilation = compile(document, registry, options);
    // The provider warnings come first, with what the compiler found.
    const Errors problems = providerProblems + compilation.problems();
    return compilation ? Compilation(compilation.value(), problems) : Compilation(problems);
}

Validation validateDocument(const Document &document, Context &context, const QString &workflow) {
    const Compilation compilation = compileDocument(document, context.registry);
    Validation validation;
    validation.errors = compilation.problems();
    if (!compilation.ok())
        return validation;
    const Process process(context);
    RunOptions options;
    options.workflow = QFileInfo(workflow).absoluteFilePath();
    const GraphCheck check = checkGraph(*compilation, process.environment, options);
    validation.errors += check.warnings;
    for (auto it = check.states.begin(); it != check.states.end(); ++it)
        validation.nodes.insert(it.key(), QJsonObject{{"state", nodeStateName(it.value())}});
    for (auto it = compilation->skipped.begin(); it != compilation->skipped.end(); ++it)
        validation.nodes.insert(it.key(), QJsonObject{{"state", nodeStateName(NodeState::Skipped)}, {"reason", it.value()}});
    return validation;
}

} // namespace fugue::cli
