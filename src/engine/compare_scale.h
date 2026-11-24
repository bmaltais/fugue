#pragma once

// What 100% means in a Compare Images viewer (#420). Both pictures are drawn in A's frame, so "one image pixel per
// screen pixel" needs a choice of picture. When B is the larger picture (a SeedVR2 restore against its original) 100%
// is B's real pixels and A is magnified; otherwise it stays A's. A second press toggles the two while B is larger.
// Plain numbers, no Qt (src/canvas/compare_detail_qml.h gives it to QML; canvas_model.cpp's compareStep uses it too).

#include <algorithm>

namespace fugue::compare {

// B is the larger picture, by area (both sizes known).
inline bool bIsLarger(double aw, double ah, double bw, double bh) {
    return aw > 0 && ah > 0 && bw > 0 && bh > 0 && bw * bh > aw * ah;
}

// Screen pixels per pixel of A's frame at 100% of A (1) or of B. B fills the frame cropped to A's shape, so the
// larger of the two ratios is the frame pixels one of its own cover.
inline double actualScale(double aw, double ah, double bw, double bh, bool ofB) {
    return ofB && bIsLarger(aw, ah, bw, bh) ? std::max(bw / aw, bh / ah) : 1.0;
}

struct Actual {
    bool actual = false;
    bool ofB = false;
};

// The view after a press of 100%: from Fit or a zoom it is the larger picture's (B's when B is larger, else A's);
// at 100% already it toggles A and B when B is larger, and stays when it is not.
inline Actual nextActual(double aw, double ah, double bw, double bh, bool actual, bool ofB) {
    if (!bIsLarger(aw, ah, bw, bh))
        return {true, false};
    return {true, actual ? !ofB : true};
}

} // namespace fugue::compare
