#pragma once

// Canvas Region and Canvas Write (#209, epic #206, spec 14.5): the nodes that read a region from a
// canvas (engine/canvas_store.h) and put a result back as a new layer.

#include "engine/node_kernel.h"

namespace fugue::nodes {

// Registers fugue/canvas-region and fugue/canvas-write.
void addCanvasKernels(Kernels &kernels);

} // namespace fugue::nodes
