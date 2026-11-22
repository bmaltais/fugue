#pragma once

// Compiling a document the way a run does (spec 9.1): the one place that knows the recipe. The
// providers are registered, their names and the trust check go to the compiler, a library or
// pack group named only by its type id is embedded (5.3, 6.1), and the provider problems come
// first among what the compiler found. `fugue compile`, `validate` and `run`, the window's runs
// and File > Export compiled all call it, so they can never compile the same document into
// different graphs.
//
// Qt Quick-free: what it needs from outside (how providers are made, who is trusted) comes in
// as a RunCompileSetup, so a test passes fakes.

#include <QString>

#include <functional>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "engine/trust_store.h"

namespace fugue {

// What the caller supplies about the machine; the same for every compile of a process.
struct RunCompileSetup {
    // Puts the provider instances into the registry (providers.json in the app). Its warnings
    // (an instance that cannot be made) are kept; an error fails the compile. None: no providers.
    std::function<Errors(Registry &)> registerProviders;
    // Whether the user allowed the pack at this version (6.7). None: nothing is checked.
    std::function<bool(const QString &pack, const QString &version)> trusted;
};

// The setup with `trust` as the check: what every caller means by "the user's trust".
RunCompileSetup runCompileSetup(std::function<Errors(Registry &)> registerProviders, const TrustStore &trust);

// What one compile asks for.
struct RunCompileRequest {
    QString target;   // "run to here": a path id (CompileOptions::target)
    QString provider; // `--provider`: the instance every unpinned node uses
    QString entered;  // the group instance the canvas shows (CompileOptions::entered)
};

// `registry` gets the providers (Registry::setProviders), so pass one a run does not use at
// the same time. The result's problems hold the provider problems, then the compiler's.
Compilation compileForRun(const Document &document, Registry &registry, const RunCompileRequest &request,
                          const RunCompileSetup &setup);

} // namespace fugue
