#pragma once

// The command line (spec 13). It is the same binary as the window app:
// main() asks wantsCli() (args.h) before it creates a QGuiApplication, and
// calls run() when the answer is yes. Nothing here may touch Gui, Quick, QML or
// open a dialog.

#include <QStringList>

#include <functional>

#include "engine/node_kernel.h"
#include "engine/registry.h"

namespace fugue::cli {

// What the process adds to the CLI. Production passes nothing: the built-in nodes
// register their types and kernels here when they exist (M1). A test binary passes fake
// ones, so the real run path, queue and worker can be driven without shipping test code:
// the shipped binary has no test hook of its own, in the environment or elsewhere.
struct Hooks {
    std::function<void(Registry &)> addNodeTypes;
    std::function<void(Kernels &)> addKernels;
};

// Runs one command to the end and returns the process exit code (0 or 1).
// Creates its own QCoreApplication, so call it from main() only.
int run(int argc, char **argv, const Hooks &hooks = {});

} // namespace fugue::cli
