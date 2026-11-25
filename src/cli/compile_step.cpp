#include "cli/compile_step.h"

#include <QFileInfo>

#include "cli/process.h"
#include "engine/input_use.h"
#include "engine/compile_for_run.h"
#include "engine/remote_consent.h"
#include "engine/trust_store.h"

namespace fugue::cli {

QJsonArray notUsedJson(const QList<UnusedInput> &unused) {
    QJsonArray list;
    for (const UnusedInput &input : unused)
        list.append(QJsonObject{{"input", input.input}, {"why", input.why}});
    return list;
}

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
    // Inputs the node's own values make irrelevant (6.1 shown_when): kept and passed on, but not used.
    const QMap<QString, QList<UnusedInput>> unused = unusedInputs(document, context.registry);
    for (auto it = unused.begin(); it != unused.end(); ++it) {
        QJsonObject node = validation.nodes.value(it.key()).toObject();
        node.insert("not_used", notUsedJson(it.value()));
        validation.nodes.insert(it.key(), node);
    }
    return validation;
}

QList<ProviderInstance> registeredInstances(const Registry &registry) {
    QList<ProviderInstance> instances;
    for (const ProviderEntry &entry : registry.providers())
        instances << entry.instance;
    return instances;
}

Errors remoteConsentProblems(const CompiledGraph &graph, Context &context, const QString &workflow, const QStringList &allowedNow) {
    const QList<ProviderInstance> instances = registeredInstances(context.registry);
    if (!anyOffMachine(instances))
        return {};
    RemoteConsent store;
    store.prune(instances);
    QSet<QString> allowed = store.allowedNames(instances);
    for (const QString &name : allowedNow)
        allowed.insert(name);
    const Process process(context);
    RunOptions options;
    options.workflow = QFileInfo(workflow).absoluteFilePath();
    const GraphCheck check = checkGraph(graph, process.environment, options);
    QSet<QString> runs;
    for (auto it = check.states.cbegin(); it != check.states.cend(); ++it)
        if (it.value() == NodeState::Stale)
            runs.insert(it.key());
    const QList<RemoteUse> uses = notAllowed(remoteUses(check.routes, runs, instances), allowed);
    return uses.isEmpty() ? Errors() : Errors{consentNeededError(uses)};
}

} // namespace fugue::cli
