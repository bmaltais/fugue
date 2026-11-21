#pragma once

// Float pixel buffers and ComfyUI's resampling (spec 14.1): Resize, Resize to Megapixels and
// Upscale Image share this one kernel, as do the pixel nodes that read and write the buffers.

#include <vector>

namespace fugue::nodes {

struct Pixels {
    int width = 0;
    int height = 0;
    int channels = 0; // 3 (RGB) or 4 (RGBA)
    std::vector<float> data;

    Pixels() = default;
    Pixels(int width, int height, int channels, float fill = 0.0f)
        : width(width), height(height), channels(channels), data(size_t(width) * height * channels, fill) {}

    float &at(int x, int y, int channel) { return data[(size_t(y) * width + x) * channels + channel]; }
    float at(int x, int y, int channel) const { return data[(size_t(y) * width + x) * channels + channel]; }
};

enum class ResizeMethod { Nearest, Bilinear, Bicubic, Lanczos, Area };

// `in` resampled to width x height with torch's interpolate (nearest-exact, bilinear, bicubic
// A = -0.75, area) or PIL's lanczos (a = 3, support widened when shrinking). Not clamped.
// RGBA is resampled with premultiplied color for every method but nearest, so what is
// transparent leaks no color into its neighbours (ComfyUI resamples RGB only).
Pixels resample(const Pixels &in, int width, int height, ResizeMethod method);

} // namespace fugue::nodes
