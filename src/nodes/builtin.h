#pragma once

// The built-in nodes (spec 14.1): their types come from the node.json files
// compiled into the binary (src/nodes/manifests/<name>/node.json, listed in manifests/pack.json), their
// kernels from the functions in kernels.h. main() hands both to cli::Hooks.

#include "engine/node_kernel.h"
#include "engine/registry.h"

namespace fugue::nodes {

// Loads the `fugue` pack from the compiled-in resources through the same loader as packs on
// disk (engine/pack_loader.h): its declared port type (fugue/image-model, 2.2, 8.4) and every
// node it lists. A problem is reported on stderr and the node skipped; tests/nodes/test_builtin.cpp
// makes any problem a hard failure.
void addBuiltinNodeTypes(Registry &registry);

void addBuiltinKernels(Kernels &kernels);

} // namespace fugue::nodes
