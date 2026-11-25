#pragma once

// Where the CLI meets the compiler (engine/compiler.h) and the providers. `fugue
// validate`, `compile`, `run` and `workflow describe` all compile through here and
// nowhere else, so a flag such as --provider means the same thing in each.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "cli/commands.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/input_use.h"

namespace fugue::cli {

// The flags that change a compile: `--to <node>` and `--provider <name>`.
struct CompileSettings {
    QString target;
    QString provider;
};

// compileForRun (engine/compile_for_run.h) with the configured providers (registerProviders) and
// the user's trust, so a pin to an unknown instance is unknown_provider. A providers.json that cannot be used fails the compile; a provider
// instance that cannot be made is a warning among the errors.
Compilation compileDocument(const Document &document, Registry &registry, const CompileSettings &settings = {});

// [{"input", "why"}] for a node's "not_used" (validate and workflow describe).
QJsonArray notUsedJson(const QList<UnusedInput> &unused);

// What validate reports.
struct Validation {
    // The compiler's errors and warnings and, when the graph compiles, the scheduler's
    // checkGraph() warnings (a missing model, a frozen result that is gone: 8.5, 10.4).
    Errors errors;
    // Per node (10.3), by path id: {"state": "cached" | "stale" | "frozen" | "skipped",
    // "reason"?} with the compiler's reason for a node it left out. Empty when the
    // graph does not compile.
    QJsonObject nodes;
};

// Nothing runs. checkGraph() asks the providers to describe themselves only when some
// node needs one. `workflow` is the file the document came from: a relative `file` input is
// that file's folder's (file_roots.h inputFilePath).
Validation validateDocument(const Document &document, Context &context, const QString &workflow);

// The off-machine provider instances the registry holds (remote_consent.h).
QList<ProviderInstance> registeredInstances(const Registry &registry);

// What a run of `graph` would send off this machine without the person having allowed it (8.2, #404): one consent_needed error
// naming each service and what leaves, or none. The person's kept answers (remote-consent.json, pruned of instances that are
// gone) and `allowedNow` (--allow-remote) count. Looks at the graph as a run would (checkGraph) only when some instance is
// off-machine. `workflow` is as for validateDocument.
Errors remoteConsentProblems(const CompiledGraph &graph, Context &context, const QString &workflow, const QStringList &allowedNow);

} // namespace fugue::cli
