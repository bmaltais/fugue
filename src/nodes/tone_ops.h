#pragma once

// Desaturate, Posterize and Pixelize (#551, spec 14.2): plain functions on float pixel buffers.
// Alpha is left alone by the first two and averaged by Pixelize (it is a channel of the block).

#include "nodes/resample.h"

namespace fugue::nodes {

// Each color moved toward its Rec. 709 luma (0.2126 R + 0.7152 G + 0.0722 B, on the values as they are) by `amount`:
// 0 is the picture, 1 is gray.
Pixels desaturate(const Pixels &image, float amount);

// Each color channel rounded to the nearest of `levels` (2 to 256) evenly spaced values from 0 to 1.
Pixels posterize(const Pixels &image, int levels);

// Every `block` x `block` square (those at the right and bottom edges are smaller) filled with the average of its pixels,
// every channel. The picture keeps its size. A block of 1 is the picture.
Pixels pixelize(const Pixels &image, int block);

} // namespace fugue::nodes
