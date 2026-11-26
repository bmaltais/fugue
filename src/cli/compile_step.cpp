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
    // What a node's own check found is a warning here, as before: validate's exit stays about the compile (8.5).
    for (Error error : check.checkErrors) {
        error.severity = Error::Severity::Warning;
        validation.errors.append(error);
    }
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

Result<QJsonObject> allowRemoteFingerprints(const QStringList &names, const Context &context) {
    const QList<ProviderInstance> instances = registeredInstances(context.registry);
    QJsonObject fingerprints;
    for (const QString &name : names) {
        const auto known = std::find_if(instances.cbegin(), instances.cend(), [&](const ProviderInstance &i) { return i.name == name; });
        if (known == instances.cend()) {
            Error error = makeError("unknown_provider", QStringLiteral("--allow-remote names \"%1\", which is not a provider instance.").arg(name),
                                    "Use the name of an instance in providers.json (Settings > Providers).");
            for (const ProviderInstance &i : instances)
                error.valid.append(i.name);
            return Result<QJsonObject>(error);
        }
        fingerprints.insert(name, consentFingerprint(*known));
    }
    return fingerprints;
}

Errors remoteConsentProblems(const CompiledGraph &graph, Context &context, const QString &workflow, const QJsonObject &allowedNow) {
    const QList<ProviderInstance> instances = registeredInstances(context.registry);
    if (!anyOffMachine(instances))
        return {};
    RemoteConsent store;
    store.prune(instances);
    const QSet<QString> allowed = store.allowedNames(instances) + RemoteConsent::matching(allowedNow, instances);
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
