#pragma once

// What the nodes that run an upscale model on the helper (`image.upscale`, spec 8.1) share: the
// capability they ask for and the model they name in the job (Upscale with Model, #134; Upscale
// Video with Model, #138).

#include <QJsonObject>
#include <QJsonValue>

#include "engine/node_kernel.h"

namespace fugue::nodes {

// The capability, for any model file: the helper offers it with no model of its own.
Requirement upscaleRequirement();

// The job's params: `context.model_path` and `tile_size`, from the node's `model` and `tile`
// inputs. On this machine the model is the file's path, found in the model folders (model_missing
// on `model` when it is not there, and a picked model is required); for a paired host it is the
// handle {name, sha256, size} that host resolves in its own folders (8.10).
Result<QJsonObject> upscaleParams(const NodeContext &context, const Selection &selection);

} // namespace fugue::nodes
