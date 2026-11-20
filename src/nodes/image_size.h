#pragma once

// Image sizes from a shape and an area (#72): what Image Size outputs and how Generate Image sizes
// a start image that does not go to the model as it is.

#include <QSize>

namespace fugue::nodes {

// The size of `megapixels` (of 1024 × 1024 pixels each) in the shape `aspect` (width / height),
// each side a multiple of `multiple` and at least one step: the width is the nearest multiple to
// the exact one, the height the nearest to the area left.
QSize sizeForMegapixels(double aspect, double megapixels, int multiple);

} // namespace fugue::nodes
