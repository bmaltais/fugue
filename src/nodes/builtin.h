#pragma once

// The built-in nodes (spec 14.1): their types come from the node.json files
// compiled into the binary (src/nodes/manifests/<name>/node.json), their
// kernels from the functions in kernels.h. main() hands both to cli::Hooks.

#include "engine/node_kernel.h"
#include "engine/registry.h"

namespace fugue::nodes {

// Registers every compiled-in manifest. A manifest that fails is reported on
// stderr and skipped; tests/nodes/test_builtin.cpp makes that a hard failure.
void addBuiltinNodeTypes(Registry &registry);

void addBuiltinKernels(Kernels &kernels);

} // namespace fugue::nodes
