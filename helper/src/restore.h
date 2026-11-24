// The pure parts of the helper's `restore` op (SeedVR2, image.restore): the DGL program it runs, what it
// refuses before loading anything, and the progress bar it reports. No engine calls here, so the main
// suite tests them (tests/engine/test_helper_restore.cpp) and helper/test/restore_test.cpp feeds the
// program to the engine's own checker.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>

namespace fugue_helper {

// What was measured on a 24 GB RTX 3090 with the 7B INT8 model (helper/README.md): a 4098 x 4098 result
// (16.8 MP) finishes at 22.9 GB of card use, a 3072 x 3072 one at 18.5 GB. The cap sits at the
// measured size; a float tensor of that size (padded to 16: 4112 x 4112 x 3 x 4 bytes, 203 MB) is also
// under DGL's per-tensor limit (256 MiB).
inline constexpr int64_t kMaxRestorePixels = 16'800'000;
inline constexpr int64_t kMaxRestoreSide = 16384;
inline constexpr int64_t kMinRestoreSide = 16; // the VAE's tiles and the model's patches
inline constexpr int64_t kMinTile = 64;
inline constexpr int64_t kMaxTile = 16384; // DGL's range for a tile size

// Why a picture of this size is refused, or empty. `width` x `height` is the picture the model gets.
inline std::string restoreGeometryError(int64_t width, int64_t height) {
    if (width < kMinRestoreSide || height < kMinRestoreSide)
        return "the picture is " + std::to_string(width) + " x " + std::to_string(height) + "; each side must be at least " +
               std::to_string(kMinRestoreSide) + " pixels";
    if (width > kMaxRestoreSide || height > kMaxRestoreSide || width * height > kMaxRestorePixels) {
        std::ostringstream message;
        message.setf(std::ios::fixed);
        message.precision(1);
        message << "the picture is " << width << " x " << height << " (" << double(width) * double(height) / 1e6
                << " MP); a 24 GB card restores at most " << double(kMaxRestorePixels) / 1e6 << " MP (4098 x 4098)";
        return message.str();
    }
    return {};
}

inline std::string restoreTilingError(int64_t tile, int64_t overlap) {
    if (tile < kMinTile || tile > kMaxTile)
        return "tile_size must be a whole number from " + std::to_string(kMinTile) + " to " + std::to_string(kMaxTile);
    if (overlap < 0 || overlap * 2 > tile)
        return "overlap must be a whole number from 0 to half the tile_size";
    return {};
}

// Whether `text` is well-formed UTF-8 (a path with other bytes cannot go into the program).
inline bool isValidUtf8(const std::string &text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size())
            return false;
        for (size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                return false;
        if ((length == 2 && c < 0xC2) || (length == 4 && c > 0xF4))
            return false;
        i += length;
    }
    return true;
}

// What the engine needs of the files, from their tensor names (it finds SeedVR2 by them and a file
// without the embedding "cannot identify component family"): said before anything is freed or loaded.
inline bool hasName(const std::set<std::string> &names, const std::string &part) {
    return std::any_of(names.begin(), names.end(), [&](const std::string &name) {
        return name.size() >= part.size() && name.compare(name.size() - part.size(), part.size(), part) == 0;
    });
}

inline std::string restoreModelError(const std::set<std::string> &names) {
    if (!hasName(names, "vid_in.proj.weight"))
        return "this is not a SeedVR2 diffusion model";
    if (!hasName(names, "positive_conditioning"))
        return "this SeedVR2 file does not carry its stored embeddings (positive_conditioning); use the Comfy-Org INT8 ConvRot "
               "checkpoint (seedvr2_7b_int8_convrot or seedvr2_3b_int8_convrot)";
    return {};
}

inline std::string restoreVaeError(const std::set<std::string> &names) {
    if (!hasName(names, "encoder.conv_out.bias") || !hasName(names, "decoder.up_blocks.0.upsamplers.0.upscale_conv.weight"))
        return "this is not the SeedVR2 VAE (ema_vae_fp16)";
    return {};
}

// The DGL program of one restore: it is the upstream SeedVR2 image script (docs/models/seedvr2.md at the pin)
// for one frame, with tiled VAE encode and decode and no scaling (the caller resizes first). The picture
// is its one tensor input; the paths and numbers go in as literals. `quoted*` are JSON strings.
inline std::string restoreScript(const std::string &quotedModel, const std::string &quotedVae, int tile, double overlap, int64_t seed) {
    std::ostringstream out;
    out.precision(12);
    out << "# version: 0.0.0\n"
        << "var {\n"
        << "    dit_path = " << quotedModel << "\n"
        << "    vae_path = " << quotedVae << "\n"
        << "    tile = " << tile << "\n"
        << "    overlap = " << overlap << "\n"
        << "    seed = " << seed << "\n"
        << "}\n"
        << R"(fn main(image) {
    dit = load_dit(dit_path)
    vae = load_vae(vae_path)
    shape = tensor::shape(image)
    width = shape[1]
    height = shape[2]
    aligned_width = math::ceil_div(width, 16) * 16
    aligned_height = math::ceil_div(height, 16) * 16
    planar = tensor::permute(tensor::cast(tensor::unsqueeze(image, axis = 3), "f32"), axes = [1, 2, 3, 0])
    padded = tensor::pad(tensor::clamp(tensor::div(planar, 255.0), min = 0, max = 1), [0, aligned_width - width, 0, aligned_height - height])
    pixels = tensor::reshape(padded, [aligned_width, aligned_height, 1, 3, 1])
    tiles = tiling(tile_size_w = tile, tile_size_h = tile, target_overlap = overlap)
    rng = make_rng("cuda", seed = seed)
    encoded = vae_encode(vae, pixels, rng = rng, tiling = tiles, posterior = "mode")
    condition_latent = vae_to_dit_latents(vae, encoded)
    encoded_shape = tensor::shape(encoded)
    mask = tensor::full([encoded_shape[0], encoded_shape[1], encoded_shape[2], 1, encoded_shape[4]], 1)
    condition = { crossattn = seedvr2::positive_embedding(dit), concat = tensor::concat([condition_latent, mask], axis = 3) }
    noise = tensor::randn_like(condition_latent, rng = rng)
    latent = sample(noise, tensor::vector([1.0, 0.0]), predict = predictor(dit, condition), method = "euler", rng = rng)
    decoded = vae_decode(vae, dit_to_vae_latents(vae, latent), tiling = tiles, dtype = "f32")
    cropped = tensor::crop(decoded, axes = [0, 1, 2], start = [0, 0, 0], size = [width, height, 1])
    unit = tensor::clamp(tensor::reshape(cropped, [width, height, 3]), min = 0, max = 1)
    packed = tensor::cast(tensor::add(tensor::mul(tensor::permute(unit, axes = [2, 0, 1]), 255.0), 0.5), "u8")
    return { image = packed }
}
)";
    return out.str();
}

// One bar for the four stages the engine reports (loading the weights, the tiled encode, the step, the tiled
// decode), as spans of a bar that never goes back. The engine reports the loading and the step through the
// run's callback by name and the tiles of the encode and decode through its global progress callback; the
// stage the first names says which the second belongs to.
class RestoreProgress {
public:
    // The run's callback: a stage and, when it has a count, how far.
    void stage(const std::string &name, int64_t completed, int64_t total) {
        // The engine loads a component when it first needs it (the DiT after the encode, the VAE again
        // before the decode): a "loading" after the first real stage is not the bar's first span. Other
        // stages it reports (preparing, executing) are not on the bar.
        if (name == "loading" && !m_stage.empty() && m_stage != "loading")
            return;
        if (name == "loading" || name == "vae_encode" || name == "sampling" || name == "vae_decode")
            m_stage = name;
        else
            return;
        // A stage without a count has just begun; the bar sits at its start.
        place(total > 0 ? double(completed) / double(total) : 0.0);
    }
    // The global callback: a tile of the current stage.
    void tiles(int step, int steps) {
        if (steps > 0 && (m_stage == "vae_encode" || m_stage == "vae_decode"))
            place(double(step) / steps);
    }
    double value() const { return m_value; }

private:
    void place(double within) {
        struct Span {
            const char *name;
            double from, to;
        };
        static const Span spans[] = {{"loading", 0.0, 0.2}, {"vae_encode", 0.2, 0.4}, {"sampling", 0.4, 0.55}, {"vae_decode", 0.55, 1.0}};
        for (const Span &span : spans)
            if (m_stage == span.name)
                m_value = std::max(m_value, span.from + (span.to - span.from) * std::clamp(within, 0.0, 1.0));
    }

    std::string m_stage;
    double m_value = 0;
};

} // namespace fugue_helper
