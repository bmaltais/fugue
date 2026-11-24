#pragma once

// The built-in node kernels, one function per file and per ticket, so tickets
// never edit the same file. Each adds the kernels for the manifests under
// src/nodes/manifests/ that it owns (spec 14.1).

#include "engine/node_kernel.h"

namespace fugue::nodes {

void addBasicKernels(Kernels &kernels);           // basic.cpp
void addCanvasKernels(Kernels &kernels);          // canvas_nodes.cpp
void addColorMatchKernels(Kernels &kernels);      // color_match.cpp
void addComponentKernels(Kernels &kernels);       // components.cpp
void addCropStitchKernels(Kernels &kernels);      // crop_stitch.cpp
void addImageGenerationKernels(Kernels &kernels); // image_generation.cpp
void addImageSizeKernels(Kernels &kernels);       // image_size.cpp
void addMaskKernels(Kernels &kernels);            // mask.cpp
void addMediaGenerationKernels(Kernels &kernels); // media_generation.cpp
void addPixelKernels(Kernels &kernels);           // pixel.cpp
void addSaveMediaKernels(Kernels &kernels);       // save_media.cpp
void addTextTemplateKernels(Kernels &kernels);   // text_template.cpp
void addTextGenerationKernels(Kernels &kernels);  // text_generation.cpp
void addUpscaleKernels(Kernels &kernels);         // upscale.cpp
void addRestoreKernels(Kernels &kernels);         // restore.cpp
void addUpscaleVideoModelKernels(Kernels &kernels); // upscale_video_model.cpp

} // namespace fugue::nodes
