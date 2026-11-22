#pragma once

// The `fugue/stitch` handle (spec 14.4): what Crop to Mask and Canvas Region (#209) hand to Stitch and
// Canvas Write. A data value whose file holds its canonical `meta` and nothing else, so its hash is
// that of the meta. The meta names the original picture and the mask as media handles (so it changes
// with either), the box the crop came from, and the crop's size.

#include <QString>

#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/node_kernel.h"
#include "nodes/crop_stitch_ops.h"

namespace fugue::nodes {

inline const QString kStitchType = QStringLiteral("fugue/stitch");

// What a stitch handle names: the picture and mask, each of which must still be the file the
// producer read; where the crop came from and its size.
struct StitchDescriptor {
    MediaHandle image;
    MediaHandle mask;
    CropBox box;
    CropSize size;
};

// The data value of a descriptor, its stitch.json written into `workDir`.
Result<Value> writeStitchHandle(const QString &workDir, const StitchDescriptor &descriptor);

// The descriptor of a stitch input. With `hashes`, the picture and mask files must still be what
// the handle says; without, they are not read.
Result<StitchDescriptor> readStitchHandle(const Value &stitch, FileHashes *hashes);

// A handle that has a file and a hash, for naming in a descriptor: an input from the scheduler has
// both already; one in memory is saved to `<context.workDir>/<name>.png`.
Result<MediaHandle> namedHandle(const MediaHandle &handle, const NodeContext &context, const QString &name);

} // namespace fugue::nodes
