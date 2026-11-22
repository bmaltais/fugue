#include "cli/compile_step.h"

#include <QFileInfo>

#include "cli/process.h"
#include "engine/compile_for_run.h"
#include "engine/trust_store.h"

namespace fugue::cli {

Compilation compileDocument(const Document &document, Registry &registry, const CompileSettings &settings) {
    return compileForRun(document, registry, {settings.target, settings.provider, {}},
                           runCompileSetup(registerProviders, TrustStore()));
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
