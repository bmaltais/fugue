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
#include "engine/scheduler.h"

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
// compiles, the scheduler's checkGraph() warnings (a missing model, a frozen result
// that is gone, 8.5 and 10.4). Nothing runs. checkGraph() asks the providers to
// describe themselves only when some node needs one.
Errors validateDocument(const Document &document, const Registry &registry, const RunEnvironment &environment);

} // namespace fugue::cli
