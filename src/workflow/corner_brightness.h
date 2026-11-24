#pragma once

// How bright a template's preview is under the Templates window's info button (spec 11.10), at a
// seam with no window: the mean relative luminance (0 black, 1 white, linear light, Rec. 709
// weights) of the bottom-right square of the preview as the card shows it (cropped to 16:10 around
// its centre). The window picks a dark icon on a light disc above `lightCornerThreshold`, else a
// light icon on a dark disc. Cached per file, size and time, so the list costs one read of each picture.

#include <QString>

namespace fugue {

constexpr double lightCornerThreshold = 0.6;

// A picture's file path, or a file URL; -1 when it cannot be read (the card then uses a dark corner).
double cornerLuminance(const QString &pathOrUrl);

}
