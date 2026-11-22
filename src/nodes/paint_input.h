#pragma once

// The Paint node (#181, spec 14.4): the layers a `paint` input names (an .ora file in the inputs
// folder, engine/paint_file.h) put over the node's picture, as a picture and a mask.

#include "engine/node_kernel.h"

namespace fugue::nodes {

// Registers fugue/paint.
void addPaintInputKernels(Kernels &kernels);

} // namespace fugue::nodes
