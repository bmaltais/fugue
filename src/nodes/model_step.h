#pragma once

// The step a picture's sides take for a model (#711): the engine aligns the size of every picture it makes to
// the VAE's scale times the diffusion model's down factor (stable-diffusion.cpp src/api/generation_program.cpp,
// `sd::align_image_dimension(size, scale * diffusion_down_factor)`, nearest multiple), which is 64 for an SD 1.x,
// SD 2.x or SDXL UNet (a down factor of 8 on a VAE of 8) and 16 or less for the transformer families. A crop that
// steps by 16 on a UNet is redrawn at another size than Fugue said, so Stitch finds another shape (`stitch_mismatch`).

#include <QJsonObject>

#include <optional>

#include "engine/model_shape.h"
#include "engine/node_kernel.h"

namespace fugue::nodes {

// The step Fugue sizes pictures by when the model is not known: the one every transformer family divides.
inline constexpr int kDefaultSizeStep = 16;
// The step of a UNet (SD 1.x, SD 2.x, SDXL and their inpaint and turbo kin).
inline constexpr int kUnetSizeStep = 64;

// The step for a model of this shape: 64 for a UNet, else 16. No shape (unknown) is 16.
int sizeStepFor(const std::optional<ModelShape> &diffusionModel);

// The step for a `fugue/model` input's value: its diffusion model (or checkpoint) is found by its hash and read by its
// header (ModelShapes). 16 when the input is empty, the file is not on this machine (a paired host's) or is unreadable.
int modelSizeStep(const NodeContext &context, const QJsonObject &model);

} // namespace fugue::nodes
