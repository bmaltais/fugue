#pragma once

// Where the CLI meets the compiler (engine/compiler.h) and the providers. `fugue
// validate`, `compile`, `run` and `workflow describe` all compile through here and
// nowhere else, so a flag such as --provider means the same thing in each.

#include <QList>
#include <QString>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/providers.h"
#include "engine/registry.h"

namespace fugue::cli {

// The flags that change a compile: `--to <node>` and `--provider <name>`.
struct CompileSettings {
    QString target;
    QString provider;
};

// compile() with the configured provider instances (providers.json, or the default
// "This machine") passed as `providers`, so a pin to an unknown instance is
// unknown_provider. An unreadable providers.json is the only extra error.
Compilation compileDocument(const Document &document, const Registry &registry, const CompileSettings &settings = {});

// What validate reports: the compiler's errors and warnings, and when the graph
// compiles, warnings from asking the providers whether each node could run (8.5).
// This talks to the configured providers when some node asks for a capability.
Errors validateDocument(const Document &document, const Registry &registry);

// The provider side of validate, as a pure function over descriptions already
// fetched: each node whose type lists a capability is run through selectProvider(),
// and every failure becomes a warning on that node (a missing model, a model not
// running, no provider for the capability, an unreachable provider). Validate
// executes nothing, so none of these is an error there; a run that reaches the node
// reports it as one.
Errors providerWarnings(const CompiledGraph &graph, const Registry &registry, const QList<Candidate> &candidates);

} // namespace fugue::cli
