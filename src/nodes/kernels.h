#pragma once

// The built-in node kernels, one function per file and per ticket, so tickets
// never edit the same file. Each adds the kernels for the manifests under
// src/nodes/manifests/ that it owns (spec 14.1).

#include "engine/node_kernel.h"

namespace fugue::nodes {

void addBasicKernels(Kernels &kernels);           // basic.cpp
void addImageGenerationKernels(Kernels &kernels); // image_generation.cpp
void addImageSizeKernels(Kernels &kernels);       // image_size.cpp
void addPixelKernels(Kernels &kernels);           // pixel.cpp

} // namespace fugue::nodes
