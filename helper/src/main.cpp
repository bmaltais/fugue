// fugue-helper: stages of image generation on stable-diffusion.cpp in Fugue's fork
// (helper/README.md, spec 8.10).
//
//   fugue-helper <request.json>     one operation, then exit
//   fugue-helper --serve            the provider protocol over stdio (8.8), for HelperProvider
//   fugue-helper --inspect FILE...  what each model file is, read from its header alone: one JSON line per file,
//                                   {"path", "recognised", and if so "version", "id", "is_dit", "video", "inpaint",
//                                   "latent_channels", "vae_kind"} or {"path", "error"}; exit 0. No backend, no GPU,
//                                   no weights (the fork's sd_fugue_inspect_model); the library's log goes to stderr
//   fugue-helper --serve-http --listen ADDR --port N --key-file FILE --models DIR[:DIR...]
//                                   the same operations over HTTP, for a paired host (README.md)
//
// A request names an operation and its files:
//
//   {"op": "encode",     "conditioning": OUT,                  prompts -> conditioning; a video
//                        "image": IN (optional)}               model's "image" is its first frame
//   {"op": "sample",     "conditioning": IN, "latent": OUT,    conditioning -> latent; with
//                        "init_latent": IN (optional),         init_latent, image-to-image from it;
//                        "image": IN (optional),               "image" as in encode (a video's first
//                        "mask": IN (optional)}                frame), or with a mask (below)
//   {"op": "decode",     "latent": IN, "image": OUT,           latent -> PNG; for a video model
//                        "video": OUT}                         "video" is written instead (below)
//   {"op": "vae_encode", "image": IN, "latent": OUT}           PNG -> latent; for a video model
//                        "video": IN (a YUV4MPEG2 clip)        the clip -> its latent (below)
//   {"op": "generate",   "image": OUT, "video": OUT,           all stages in one call; with a
//                        "mask": IN (optional)}                mask, params.init_image is the picture
//   {"op": "upscale",    "image": IN, "output": OUT}           PNG -> PNG through an upscale model;
//                        "context": {"model_path"}, "params": {"tile_size"} (no sd context: the
//                        upscaler stays loaded between calls); "frames": DIR, "frames_output":
//                        DIR instead of image and output: every PNG of a folder, in name order
//   {"op": "restore",    "image": IN, "output": OUT}           PNG -> PNG restored by SeedVR2 at its own
//                        "context": {"model_path", "vae_path"}, size (the caller scales it first): padded
//                        "params": {"tile_size", "overlap",    to a multiple of 16, VAE-encoded in tiles,
//                        "seed"}                               one euler step, decoded in tiles, cropped;
//                        refused before loading: model_unsupported (the files' tensor names), input_invalid (size, tiling)
//
// A context of a video model (Wan) makes the same operations on video: the library's video
// options (video_frames, fps, high_noise_*) and sd_generate_video; the latent is the clip's, and
// decode writes the "video" file, YUV4MPEG2 (4:2:0, which every ffmpeg reads; the helper links no
// codec), not an image. Video to video: vae_encode of a "video" (a YUV4MPEG2 file in the layout
// decode writes: 4:2:0, 4n+1 frames, sides a multiple of 16) makes the clip's latent; its size, length
// and rate are the file's and replace the params'. A sample takes it as "init_latent" with
// params.strength (the part redrawn, as for an image), and a generate takes the clip itself as
// "init_video": the pixels are the same as the staged run.
//
// A "mask" (sample, generate; image models only, else input_invalid) is a PNG read with one channel
// at the request's width and height (white is redrawn), checked before any model loads. The library
// rounds it to 0 or 1. Its start picture depends on the model, by sd_get_model_version_name: an
// inpaint model (INPAINT_VERSIONS below) takes the picture, "image" of a sample, as init_image and
// ignores a start latent (its masked picture is a predictor input the sample's program builds from
// that picture and the mask); any other image model takes the start latent
// ("init_latent", the one vae_encode made) and ignores "image". A mask with neither, an inpaint
// model without the picture, is input_invalid.
//
// plus "context" (the model files and context options) and "params" (the generation options),
// passed to stable-diffusion.cpp by key as they are (docs/guides/c_api.md at the pin). A value
// is a string, number, bool, list of numbers, {"image": PATH} or a list of them, or for
// "loras" a list of {"path", "multiplier", "is_high_noise"} (the last two optional). Every stage of one generation takes the same
// context and params. A stage loads only the components it runs, except that every call
// VAE-encodes the request's images (init, reference, control) and so loads the VAE.
// The one-shot reply is one JSON line on stdout, {"outputs": [...]} or {"error": {"code",
// "message"}}; the library's log goes to stderr.
//
// --serve reads one JSON line per operation on stdin, each with an "id" its reply echoes:
//
//   {"op": "submit", "capability", "params", "inputs", "output_dir", "preview"?}  -> {"job"}
//   {"op": "status", "job"}    -> {"state", "progress", "stage", "error"?}
//   {"op": "cancel", "job"}    -> {"state"}
//   {"op": "result", "job"}    -> {"outputs": {NAME: {"path"} or a list of them}, "used"}
//   {"op": "shutdown"}         -> {}, then it exits; so does it when stdin closes
//
// The capability is the operation: text.encode (encode), latent.sample (sample),
// latent.decode (decode), latent.encode (vae_encode), image.upscale (upscale), image.restore (restore). params.context is the request's
// "context" and the other params its "params"; inputs.conditioning, .latent, .init_latent,
// .image, .mask and .frames ({"path"}) are its input files, and its outputs are written into output_dir
// as conditioning.bin, latent.bin, image.png (an upscale of frames: the folder frames-out/). Jobs
// run one at a time, in order. Unasked, it writes
// {"op": "status", "job", "state", "progress", "stage", "error"?, "preview"?} lines as a job moves.
// A latent.sample submit with "preview" ("proj", "tae" or "vae"; anything else is none) also
// writes about 8 pictures of the sampling to output_dir/preview.png, one after the other (each
// renamed over the last), and a status line with "preview": PATH tells each is there. A key an
// older helper does not know is ignored, and a status line's unknown key is ignored by an older Fugue.
// An error is {"error": {"code", "message"}}; out of memory is "out_of_memory".
//
// Loaded models stay loaded between jobs (one context per set of context options) until their
// memory is needed: before loading, the least recently used contexts are freed while the device
// has less free memory than the new one's files take, and a load or a generation that runs out
// of memory frees the others and is tried once more.
//
// --serve-http serves the same jobs as POST /fugue/v1/{describe,submit,status,cancel,result,
// shutdown} and GET /fugue/v1/result/<job>/<i>, JSON bodies and a bearer key; the contract is in
// README.md. There the host makes each job's work dir, inputs arrive as {"data", "format"}, model
// files are named by handle ({"name", "sha256", "size"}) and found under --models, and the
// outputs are fetched as files.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "ggml-backend.h"
#include "json.hpp"
#include "stable-diffusion.h"
#include "engine_error.h"
#include "restore.h"
#include "strength_args.h"

// stb and json.hpp come from stable-diffusion.cpp's thirdparty/, a system include folder here
// (CMakeLists.txt): their warnings are not ours.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "httplib.h"

#ifndef FUGUE_BACKEND
#define FUGUE_BACKEND "cpu"
#endif

using nlohmann::json;

namespace {

struct Failure {
    std::string code;
    std::string message;
};

[[noreturn]] void fail(const std::string &code, const std::string &message) { throw Failure{code, message}; }

// The last error line of the engine's log (#542): a failed component load says only "cannot load
// component", and the reason is logged. The log runs on the worker's thread, and a failure is read on it.
fugue_helper::EngineReason g_engineReason;

void rememberEngineLog(enum sd_log_level_t level, const char *text) {
    if (level == SD_LOG_ERROR)
        g_engineReason.record(text);
}

std::string lastError() {
    const char *e = sd_get_last_error();
    return e && *e ? g_engineReason.explain(e) : "no detail";
}

// The error code of a failed library call: out of memory is its own code (spec 8.8).
void check(sd_status_t status, const std::string &what) {
    if (status == SD_STATUS_OK)
        return;
    fail(status == SD_STATUS_OUT_OF_MEMORY ? "out_of_memory" : "helper_failed", what + ": " + lastError());
}

struct TensorFree {
    void operator()(sd_tensor_t *t) const { sd_tensor_free(t); }
};
using Tensor = std::unique_ptr<sd_tensor_t, TensorFree>;

Tensor loadImage(const std::string &path) {
    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 3);
    if (!pixels)
        fail("input_invalid", "cannot read the image " + path);
    const int64_t shape[3] = {3, w, h};
    Tensor t(sd_tensor_create(SD_TENSOR_U8, 3, shape, pixels, static_cast<size_t>(w) * h * 3));
    stbi_image_free(pixels);
    if (!t)
        fail("helper_failed", "cannot make a tensor of " + path + ": " + lastError());
    return t;
}

// A mask: one channel (stbi converts a color file to gray), at the request's size. The library
// reads a mask with one channel (a color one would break FLUX.1 Fill's packing of it).
Tensor loadMask(const std::string &path, int width, int height) {
    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 1);
    if (!pixels)
        fail("input_invalid", "cannot read the mask " + path);
    if (w != width || h != height) {
        stbi_image_free(pixels);
        fail("input_invalid", "the mask is " + std::to_string(w) + "x" + std::to_string(h) + ", the request " +
                                  std::to_string(width) + "x" + std::to_string(height));
    }
    const int64_t shape[3] = {1, w, h};
    Tensor t(sd_tensor_create(SD_TENSOR_U8, 3, shape, pixels, static_cast<size_t>(w) * h));
    stbi_image_free(pixels);
    if (!t)
        fail("helper_failed", "cannot make a tensor of " + path + ": " + lastError());
    return t;
}

// The library's version names of the inpaint models (sd_get_model_version_name, at the pin): they
// take the masked picture as part of the conditioning. README.md: re-check them when the pin moves.
bool isInpaintVersion(const char *name) {
    static const char *const names[] = {"SD 1.x Inpaint", "SD 2.x Inpaint", "SDXL Inpaint", "Flux Fill", "Flex.2"};
    for (const char *n : names)
        if (name && std::string(name) == n)
            return true;
    return false;
}

void saveImage(const sd_tensor_t *image, const std::string &path) {
    if (sd_tensor_type(image) != SD_TENSOR_U8 || sd_tensor_rank(image) != 3)
        fail("helper_failed", "the library returned an image of an unexpected shape");
    const int c = static_cast<int>(sd_tensor_dim(image, 0));
    const int w = static_cast<int>(sd_tensor_dim(image, 1));
    const int h = static_cast<int>(sd_tensor_dim(image, 2));
    if (!stbi_write_png(path.c_str(), w, h, c, sd_tensor_data(image), w * c))
        fail("helper_failed", "cannot write " + path);
}

// A video as YUV4MPEG2 (4:2:0, full range, the library's U8 [C,W,H,T] frames): a plain file every
// ffmpeg reads, with the size and rate in its header. The helper links no video codec; Fugue
// encodes it (spec 8.10).
void saveVideo(const sd_video_t &video, const std::string &path) {
    const sd_tensor_t *frames = video.data;
    if (!frames || sd_tensor_type(frames) != SD_TENSOR_U8 || sd_tensor_rank(frames) != 4 || sd_tensor_dim(frames, 0) != 3)
        fail("helper_failed", "the library returned a video of an unexpected shape");
    const int64_t w = sd_tensor_dim(frames, 1), h = sd_tensor_dim(frames, 2), count = sd_tensor_dim(frames, 3);
    if (w <= 0 || h <= 0 || count <= 0)
        fail("helper_failed", "the library returned an empty video");
    const auto *pixels = static_cast<const unsigned char *>(sd_tensor_data(frames));
    FILE *out = std::fopen(path.c_str(), "wb");
    if (!out)
        fail("helper_failed", "cannot write " + path);
    const int fps = video.fps > 0 ? video.fps : 16;
    std::fprintf(out, "YUV4MPEG2 W%lld H%lld F%d:1 Ip A1:1 C420jpeg\n", static_cast<long long>(w), static_cast<long long>(h), fps);
    const int64_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    std::vector<unsigned char> y(static_cast<size_t>(w * h)), u(static_cast<size_t>(cw * ch)), v(static_cast<size_t>(cw * ch));
    bool ok = true;
    for (int64_t f = 0; f < count && ok; ++f) {
        const unsigned char *frame = pixels + static_cast<size_t>(f * w * h * 3);
        for (int64_t j = 0; j < ch; ++j)
            for (int64_t i = 0; i < cw; ++i) {
                int su = 0, sv = 0, n = 0; // the chroma of a 2x2 block, averaged
                for (int64_t dj = 0; dj < 2; ++dj)
                    for (int64_t di = 0; di < 2; ++di) {
                        const int64_t jj = std::min(h - 1, 2 * j + dj), ii = std::min(w - 1, 2 * i + di);
                        const unsigned char *p = frame + (jj * w + ii) * 3;
                        const float r = p[0], g = p[1], b = p[2];
                        y[static_cast<size_t>(jj * w + ii)] = static_cast<unsigned char>(std::lround(0.299f * r + 0.587f * g + 0.114f * b));
                        su += static_cast<int>(std::lround(128 - 0.168736f * r - 0.331264f * g + 0.5f * b));
                        sv += static_cast<int>(std::lround(128 + 0.5f * r - 0.418688f * g - 0.081312f * b));
                        ++n;
                    }
                u[static_cast<size_t>(j * cw + i)] = static_cast<unsigned char>(std::clamp((su + n / 2) / n, 0, 255));
                v[static_cast<size_t>(j * cw + i)] = static_cast<unsigned char>(std::clamp((sv + n / 2) / n, 0, 255));
            }
        ok = std::fputs("FRAME\n", out) >= 0 && std::fwrite(y.data(), 1, y.size(), out) == y.size() &&
             std::fwrite(u.data(), 1, u.size(), out) == u.size() && std::fwrite(v.data(), 1, v.size(), out) == v.size();
    }
    ok = (std::fclose(out) == 0) && ok;
    if (!ok)
        fail("helper_failed", "cannot write " + path);
}

// The size, rate and frame count of a YUV4MPEG2 file (4:2:0), from its header and its length.
struct Y4m {
    int width = 0, height = 0, fps = 0, frames = 0;
};

Y4m readY4mHeader(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("YUV4MPEG2", 0) != 0)
        fail("input_invalid", path + " is not a YUV4MPEG2 video");
    Y4m clip;
    int num = 0, den = 1;
    bool is420 = true;
    std::istringstream fields(line);
    std::string field;
    fields >> field;
    while (fields >> field) {
        if (field[0] == 'W')
            clip.width = std::atoi(field.c_str() + 1);
        else if (field[0] == 'H')
            clip.height = std::atoi(field.c_str() + 1);
        else if (field[0] == 'F')
            std::sscanf(field.c_str() + 1, "%d:%d", &num, &den);
        else if (field[0] == 'C')
            is420 = field.compare(1, 3, "420") == 0;
    }
    if (!is420 || clip.width <= 0 || clip.height <= 0)
        fail("input_invalid", "a video must be YUV4MPEG2 4:2:0 with a size: " + path);
    clip.fps = std::max(1, den > 0 ? static_cast<int>(std::lround(double(num) / den)) : 16);
    std::error_code ignored;
    const auto total = static_cast<long long>(std::filesystem::file_size(path, ignored));
    const long long chroma = static_cast<long long>((clip.width + 1) / 2) * ((clip.height + 1) / 2);
    const long long frame = 6 + static_cast<long long>(clip.width) * clip.height + 2 * chroma; // "FRAME\n" and the planes
    clip.frames = static_cast<int>((total - static_cast<long long>(line.size()) - 1) / frame);
    if (clip.frames <= 0)
        fail("input_invalid", "the video has no frames: " + path);
    return clip;
}

bool isImage(const json &v) { return v.is_object() && v.contains("image") && v["image"].is_string(); }

// Sets one option from its JSON value.
void setOption(sd_opt_t *opt, const std::string &key, const json &v) {
    const char *k = key.c_str();
    const std::string what = "option " + key;
    if (v.is_string()) {
        check(sd_opt_set_string(opt, k, v.get<std::string>().c_str()), what);
    } else if (v.is_boolean()) {
        check(sd_opt_set_bool(opt, k, v.get<bool>()), what);
    } else if (v.is_number_integer()) {
        // A whole number may be meant for a float option ("cfg_scale": 1).
        const sd_status_t status = sd_opt_set_int(opt, k, v.get<int64_t>());
        check(status == SD_STATUS_TYPE_MISMATCH ? sd_opt_set_float(opt, k, v.get<double>()) : status, what);
    } else if (v.is_number()) {
        check(sd_opt_set_float(opt, k, v.get<double>()), what);
    } else if (isImage(v)) {
        check(sd_opt_set_tensor(opt, k, loadImage(v["image"].get<std::string>()).get()), what);
    } else if (v.is_array() && !v.empty() && isImage(v[0])) {
        std::vector<Tensor> owned;
        std::vector<const sd_tensor_t *> tensors;
        for (const json &item : v) {
            if (!isImage(item))
                fail("input_invalid", what + ": every item must be an image");
            owned.push_back(loadImage(item["image"].get<std::string>()));
            tensors.push_back(owned.back().get());
        }
        check(sd_opt_set_tensors(opt, k, tensors.data(), tensors.size()), what);
    } else if (key == "loras" && v.is_array()) {
        std::vector<sd_lora_t> loras;
        for (const json &item : v)
            loras.push_back({item.value("is_high_noise", false), item.value("multiplier", 1.0f),
                             item.at("path").get_ref<const std::string &>().c_str()});
        check(sd_opt_set_loras(opt, k, loras.data(), loras.size()), what);
    } else if (v.is_array()) {
        std::vector<float> numbers;
        for (const json &item : v) {
            if (!item.is_number())
                fail("input_invalid", what + ": a list must hold numbers, images or loras");
            numbers.push_back(item.get<float>());
        }
        check(sd_opt_set_floats(opt, k, numbers.data(), numbers.size()), what);
    } else {
        fail("input_invalid", what + " has a value of an unsupported kind");
    }
}

struct OptFree {
    void operator()(sd_opt_t *o) const { sd_opt_free(o); }
};
using Opt = std::unique_ptr<sd_opt_t, OptFree>;

std::string required(const json &request, const char *field) {
    if (!request.contains(field) || !request[field].is_string() || request[field].get<std::string>().empty())
        fail("input_invalid", std::string("\"") + field + "\" is required for " + request.value("op", ""));
    return request[field].get<std::string>();
}

// The free memory of the device the models load on (the first GPU), in bytes; as good as
// unlimited without one (a CPU build).
size_t freeDeviceMemory() {
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU)
            continue;
        size_t free = 0, total = 0;
        ggml_backend_dev_memory(device, &free, &total);
        return free;
    }
    return std::numeric_limits<size_t>::max();
}

// What loading the model files of `options` takes, about: their sizes (the weights), and a fifth
// more for the buffers.
size_t memoryFor(const json &options) {
    size_t bytes = 0;
    for (const auto &[key, value] : options.items()) {
        const std::string suffix = "_path";
        if (!value.is_string() || key.size() < suffix.size() || key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::error_code ignored;
        const auto size = std::filesystem::file_size(value.get<std::string>(), ignored);
        if (!ignored)
            bytes += size;
    }
    return bytes + bytes / 5;
}

// The context options the helper adds unless the request sets them. On CUDA, SageAttention for the
// diffusion model (#382): about a third less attention time, and the pictures look the same (33-39
// dB PSNR against flash attention on Klein 9B); a request sends "sage_attn": false to keep flash
// attention. The library falls back to flash attention where its kernel does not apply.
json withDefaults(json options) {
    if (std::string(FUGUE_BACKEND) == "cuda" && !options.contains("sage_attn"))
        options["sage_attn"] = true;
    return options;
}

// The loaded contexts, one per set of context options, least recently used first.
class Contexts {
public:
    Contexts() = default;
    Contexts(const Contexts &) = delete;
    Contexts &operator=(const Contexts &) = delete;
    ~Contexts() { clear(); }

    // The one upscaler kept loaded between upscale jobs, by model file and tile size: a video comes
    // as a job per chunk of frames and must not read the model again for each. It is freed before a
    // diffusion context loads, by dropOthers and by clear, like any other model here.
    sd_upscaler_t *upscaler(const std::string &model, const json &tile) {
        const std::string key = model + "|" + tile.dump();
        if (m_upscaler && m_upscalerKey == key)
            return m_upscaler;
        dropUpscaler();
        Opt load(sd_upscaler_opt_create());
        if (!load)
            fail("out_of_memory", "cannot create the options");
        check(sd_opt_set_string(load.get(), "model_path", model.c_str()), "model_path");
        if (!tile.is_null())
            setOption(load.get(), "tile_size", tile);
        const std::string name = std::filesystem::path(model).filename().string();
        const sd_status_t created = sd_upscaler_create(load.get(), &m_upscaler);
        if (created == SD_STATUS_LOAD_FAILED)
            fail("model_unsupported", "cannot load the upscale model " + name +
                                          ": the engine runs ESRGAN (RRDBNet) 4x models and MoSR; a 2x model that unshuffles pixels (Real-ESRGAN x2plus) is not supported");
        check(created, "loading the upscale model " + name);
        m_upscalerKey = key;
        return m_upscaler;
    }

    sd_ctx_t *get(const json &requested) {
        const json options = withDefaults(requested);
        const std::string key = options.dump();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (it->key == key) {
                m_entries.splice(m_entries.end(), m_entries, it);
                return m_entries.back().ctx;
            }
        dropUpscaler();
        const size_t needed = memoryFor(options);
        while (!m_entries.empty() && freeDeviceMemory() < needed)
            dropFirst();
        Opt context(sd_ctx_opt_create());
        if (!context)
            fail("out_of_memory", "cannot create the context options");
        for (const auto &[name, value] : options.items())
            setOption(context.get(), name, value);
        sd_ctx_t *ctx = nullptr;
        sd_status_t status = sd_ctx_create(context.get(), &ctx);
        if (status == SD_STATUS_OUT_OF_MEMORY && !m_entries.empty()) {
            clear();
            status = sd_ctx_create(context.get(), &ctx);
        }
        check(status, "loading the models");
        m_entries.push_back({key, ctx});
        return ctx;
    }

    // Frees every context but `keep`; false when there was none to free.
    bool dropOthers(sd_ctx_t *keep) {
        bool dropped = dropUpscaler();
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->ctx == keep) {
                ++it;
                continue;
            }
            sd_ctx_free(it->ctx);
            it = m_entries.erase(it);
            dropped = true;
        }
        return dropped;
    }

    void clear() {
        while (!m_entries.empty())
            dropFirst();
        dropUpscaler();
    }

private:
    bool dropUpscaler() {
        if (!m_upscaler)
            return false;
        sd_upscaler_free(m_upscaler);
        m_upscaler = nullptr;
        return true;
    }

    void dropFirst() {
        sd_ctx_free(m_entries.front().ctx);
        m_entries.pop_front();
    }

    struct Entry {
        std::string key;
        sd_ctx_t *ctx;
    };
    std::list<Entry> m_entries;
    sd_upscaler_t *m_upscaler = nullptr;
    std::string m_upscalerKey;
};

// What a job tells the server it is running, and asks of it.
struct Hooks {
    // Told the context just before the generation (false from it: cancelled, do not start) and
    // null after it.
    std::function<bool(sd_ctx_t *)> running;
    // The same for a restore, which runs on a DGL context: told it before the run (false: cancelled) and
    // null after; the server cancels it with sd_dgl_ctx_cancel.
    std::function<bool(sd_dgl_ctx_t *)> runningDgl;
    // True once the job is cancelled: polled between the frames of an upscale, which has no
    // cancel inside a frame.
    std::function<bool()> cancelled;
    // The share of an upscale's frames done, 0 to 1 (a job of one image reports its tiles through
    // the library's own callback).
    std::function<void(double)> progress;
    // A preview frame of the sampling was written to this file (the request's "preview_file").
    std::function<void(const std::string &)> preview;
};

// The library's preview callback for the length of one sampling call (op "sample"): the request's
// "preview" is the mode (proj, tae or vae) and "preview_file" where each frame goes, written beside
// it and renamed over it, so a reader never sees half a picture. About 8 frames a run, whatever the
// steps. A video model has none (the frames of a clip are not worth their cost). Without
// "preview", with "off", or in a request no one listens to, it does nothing; the library's
// callback is global, and the worker runs one job at a time.
class PreviewScope {
public:
    PreviewScope(const json &request, const Hooks &hooks, bool video, const std::string &version) : m_hooks(hooks) {
        std::string mode = request.value("preview", "");
        m_file = request.value("preview_file", "");
        if (video || m_file.empty() || !hooks.preview || (mode != "proj" && mode != "tae" && mode != "vae"))
            return;
        // A tiny autoencoder needs its file (context "taesd_path"); without it, the cheap projection.
        if (mode == "tae" && !request.value("context", json::object()).contains("taesd_path"))
            mode = "proj";
        const json params = request.value("params", json::object());
        const json &asked = params.contains("sample.steps") ? params["sample.steps"] : params.value("steps", json());
        int steps = asked.is_number() ? asked.get<int>() : 20;
        // A schedule cut asks for a longer schedule than it runs (strength_args.h).
        if (params.contains("strength") && params["strength"].is_number())
            steps = fugue_helper::sampledSteps(steps, params.value("sample.args", std::string()), version,
                                               params["strength"].get<double>());
        m_active = sd_set_preview_callback(&PreviewScope::frame, mode.c_str(), std::max(1, (steps + 7) / 8), true, false,
                                           this) == SD_STATUS_OK;
    }
    ~PreviewScope() {
        if (m_active)
            sd_set_preview_callback(nullptr, "none", 1, false, false, nullptr);
    }
    PreviewScope(const PreviewScope &) = delete;
    PreviewScope &operator=(const PreviewScope &) = delete;

private:
    static void frame(int, int count, const sd_tensor_t *const *frames, bool, void *data) {
        auto *self = static_cast<PreviewScope *>(data);
        const sd_tensor_t *image = count > 0 && frames ? frames[0] : nullptr;
        if (!image || sd_tensor_type(image) != SD_TENSOR_U8 || sd_tensor_rank(image) != 3)
            return;
        const int c = static_cast<int>(sd_tensor_dim(image, 0));
        const int w = static_cast<int>(sd_tensor_dim(image, 1));
        const int h = static_cast<int>(sd_tensor_dim(image, 2));
        const std::string temporary = self->m_file + ".tmp";
        std::error_code ignored;
        if (!stbi_write_png(temporary.c_str(), w, h, c, sd_tensor_data(image), w * c))
            return;
        std::filesystem::rename(temporary, self->m_file, ignored);
        if (!ignored)
            self->m_hooks.preview(self->m_file);
    }

    const Hooks &m_hooks;
    std::string m_file;
    bool m_active = false;
};

// One image through the upscaler (the library's tiling reports each tile as progress).
void upscaleImage(sd_upscaler_t *upscaler, const std::string &in, const std::string &out) {
    Opt options(sd_upscale_opt_create());
    if (!options)
        fail("out_of_memory", "cannot create the options");
    check(sd_opt_set_tensor(options.get(), "image", loadImage(in).get()), "image");
    sd_result_t *result = nullptr;
    check(sd_upscale(upscaler, options.get(), &result), "upscale");
    std::unique_ptr<sd_result_t, void (*)(sd_result_t *)> resultOwner(result, sd_result_free);
    if (sd_result_get_image_count(result) == 0)
        fail("helper_failed", "no image was made");
    saveImage(sd_result_get_image(result, 0), out);
}

// image.upscale (spec 8.1): an image, or a folder of frames, through an upscale model (ESRGAN,
// MoSR), tiled by the library's own tiling (params.tile_size, overlap a quarter) so a large image
// fits in memory. The upscaler stays loaded between jobs (Contexts::upscaler), so a video's chunks
// do not read the model again. "frames" is a folder of PNGs: each one is upscaled in file-name
// order into "frames_output" under the same name, one at a time (a frame is never held with its
// neighbours), the progress is the frames done, and a cancel is heard between frames, as the
// library has no hook inside one. A single image's cancel is the provider's: the helper is ended
// after the grace period (spec 9.6).
json upscaleOnce(const json &request, Contexts &contexts, const Hooks &hooks) {
    const json context = request.value("context", json::object());
    const json params = request.value("params", json::object());
    const std::string model = context.value("model_path", "");
    if (model.empty())
        fail("input_invalid", "\"model_path\" is required for upscale");
    const json tile = params.contains("tile_size") ? params["tile_size"] : json();
    if (!request.contains("frames")) {
        sd_upscaler_t *upscaler = contexts.upscaler(model, tile);
        upscaleImage(upscaler, required(request, "image"), required(request, "output"));
        return {{"outputs", json::array({json{{"image", request["output"]}}})}, {"scale", sd_upscaler_get_scale(upscaler)}};
    }
    namespace fs = std::filesystem;
    const fs::path in = required(request, "frames"), out = required(request, "frames_output");
    std::vector<fs::path> names;
    std::error_code ignored;
    for (const auto &entry : fs::directory_iterator(in, ignored))
        if (entry.is_regular_file() && entry.path().extension() == ".png")
            names.push_back(entry.path().filename());
    std::sort(names.begin(), names.end());
    if (names.empty())
        fail("input_invalid", "no PNG frames in " + in.string());
    fs::create_directories(out, ignored);
    int scale = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (hooks.cancelled && hooks.cancelled())
            fail("cancelled", "upscale was cancelled");
        // Written beside, then renamed: a frame is either whole or not there.
        const fs::path part = out / (names[i].string() + ".part"), done = out / names[i];
        try {
            sd_upscaler_t *upscaler = contexts.upscaler(model, tile);
            scale = sd_upscaler_get_scale(upscaler);
            upscaleImage(upscaler, (in / names[i]).string(), part.string());
        } catch (const Failure &failure) {
            // The loaded models may hold the memory it needs: free them, and try this frame once more.
            if (failure.code != "out_of_memory" || !contexts.dropOthers(nullptr))
                throw;
            upscaleImage(contexts.upscaler(model, tile), (in / names[i]).string(), part.string());
        }
        fs::rename(part, done);
        if (hooks.progress)
            hooks.progress(double(i + 1) / names.size());
    }
    return {{"outputs", json::array({json{{"frames", out.string()}}})}, {"scale", scale}, {"frames", names.size()}};
}

json runUpscale(const json &request, Contexts &contexts, const Hooks &hooks) {
    try {
        return upscaleOnce(request, contexts, hooks);
    } catch (const Failure &failure) {
        // The loaded models may hold the memory it needs: free them, and try once more.
        if (failure.code != "out_of_memory" || !contexts.dropOthers(nullptr))
            throw;
        return upscaleOnce(request, contexts, hooks);
    }
}


// image.restore: SeedVR2 through the library's DGL (the pin has no vid_upscale call; its docs/models/seedvr2.md
// script is the model). The picture is already the size it should end at (Fugue scales it, so the
// reference of the colour fix is the same picture). The program, the limits and the progress bar are in
// restore.h; this reads the request, checks it before anything is freed, and runs the program on a DGL
// context made for the job.
struct DglFree {
    void operator()(sd_dgl_ctx_t *c) const { sd_dgl_ctx_free(c); }
};
struct DglResultFree {
    void operator()(sd_dgl_result_t *r) const { sd_dgl_result_free(r); }
};

// The tracker the global tile callback feeds while a restore runs (both run on the worker thread).
fugue_helper::RestoreProgress *g_restoreProgress = nullptr;

// A whole number in [low, high] from a request value, or a refusal naming the field.
int64_t wholeNumber(const json &params, const char *field, int64_t fallback) {
    if (!params.contains(field))
        return fallback;
    const json &value = params[field];
    if (value.is_number_integer() || value.is_number_unsigned())
        return value.get<int64_t>();
    if (value.is_number_float() && std::isfinite(value.get<double>()) && value.get<double>() == std::floor(value.get<double>()) &&
        std::fabs(value.get<double>()) < 9e15)
        return static_cast<int64_t>(value.get<double>());
    fail("input_invalid", std::string(field) + " must be a whole number");
}

// The tensor names of a safetensors or GGUF file's header (empty when it is neither or cannot be read).
std::set<std::string> tensorNames(const std::string &path) {
    std::set<std::string> names;
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    if (!in.read(magic, 4))
        return names;
    const auto u64 = [&] {
        uint64_t v = 0;
        in.read(reinterpret_cast<char *>(&v), 8);
        return in ? v : uint64_t(~0ull);
    };
    const auto u32 = [&] {
        uint32_t v = 0;
        in.read(reinterpret_cast<char *>(&v), 4);
        return v;
    };
    const auto text = [&](uint64_t limit) {
        const uint64_t length = u64();
        if (!in || length > limit)
            return std::string("\1");
        std::string value(length, '\0');
        in.read(value.data(), static_cast<std::streamsize>(length));
        return value;
    };
    if (std::memcmp(magic, "GGUF", 4) == 0) {
        u32(); // version
        const uint64_t tensors = u64(), pairs = u64();
        if (!in || tensors > 1'000'000 || pairs > 1'000'000)
            return names;
        // A value: its type, then the bytes; an array is a type and a count, then that many values.
        std::function<bool(uint32_t)> skip = [&](uint32_t type) {
            static const int sizes[] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
            if (type == 8)
                return text(1 << 28) != "\1" && bool(in);
            if (type == 9) {
                const uint32_t inner = u32();
                const uint64_t count = u64();
                if (!in || inner > 12 || count > (1ull << 32))
                    return false;
                if (inner != 8 && inner != 9) {
                    in.seekg(static_cast<std::streamoff>(count * sizes[inner]), std::ios::cur);
                    return bool(in);
                }
                for (uint64_t i = 0; i < count; ++i)
                    if (!skip(inner))
                        return false;
                return true;
            }
            if (type > 12)
                return false;
            in.seekg(sizes[type], std::ios::cur);
            return bool(in);
        };
        for (uint64_t i = 0; i < pairs; ++i) {
            if (text(1 << 20) == "\1" || !skip(u32()))
                return names;
        }
        for (uint64_t i = 0; i < tensors; ++i) {
            const std::string name = text(1 << 20);
            if (name == "\1")
                return {};
            names.insert(name);
            const uint32_t dims = u32();
            if (!in || dims > 8)
                return {};
            in.seekg(static_cast<std::streamoff>(dims) * 8 + 4 + 8, std::ios::cur);
            if (!in)
                return {};
        }
        return names;
    }
    // safetensors: an 8-byte length (its first 4 bytes are already read), then a JSON object.
    in.seekg(0);
    const uint64_t length = u64();
    if (!in || length == 0 || length > (100ull << 20))
        return names;
    std::string header(length, '\0');
    if (!in.read(header.data(), static_cast<std::streamsize>(length)))
        return names;
    const json parsed = json::parse(header, nullptr, false);
    if (!parsed.is_object())
        return names;
    for (const auto &item : parsed.items())
        if (item.key() != "__metadata__")
            names.insert(item.key());
    return names;
}

json runRestore(const json &request, Contexts &contexts, const Hooks &hooks) {
    const json context = request.value("context", json::object());
    const json params = request.value("params", json::object());
    const std::string model = context.value("model_path", ""), vae = context.value("vae_path", "");
    if (model.empty() || vae.empty())
        fail("input_invalid", "\"model_path\" and \"vae_path\" are required for restore");
    if (!fugue_helper::isValidUtf8(model) || !fugue_helper::isValidUtf8(vae))
        fail("input_invalid", "a model path is not valid UTF-8");
    const int64_t tile = wholeNumber(params, "tile_size", 512), overlap = wholeNumber(params, "overlap", 64);
    const int64_t seed = wholeNumber(params, "seed", 0);
    if (const std::string why = fugue_helper::restoreTilingError(tile, overlap); !why.empty())
        fail("input_invalid", why);
    if (seed < 0)
        fail("input_invalid", "seed must be a whole number of 0 or more");
    const std::string input = required(request, "image");
    Tensor image = loadImage(input);
    if (const std::string why = fugue_helper::restoreGeometryError(sd_tensor_dim(image.get(), 1), sd_tensor_dim(image.get(), 2)); !why.empty())
        fail("input_invalid", why);
    // The files are read for their tensor names before anything loaded is freed: the wrong file (a
    // Klein model, a GGUF without its embedding, a Flux VAE) fails here, as the upscaler's does.
    const auto unsupported = [](const std::string &file, const std::string &why) {
        fail("model_unsupported", std::filesystem::path(file).filename().string() + ": " + why);
    };
    for (const auto &[file, vaeFile] : {std::pair{model, false}, std::pair{vae, true}}) {
        const std::set<std::string> names = tensorNames(file);
        if (names.empty())
            unsupported(file, "cannot read its tensor names (a .safetensors or .gguf file is needed)");
        if (const std::string why = vaeFile ? fugue_helper::restoreVaeError(names) : fugue_helper::restoreModelError(names); !why.empty())
            unsupported(file, why);
    }
    if (hooks.cancelled && hooks.cancelled())
        fail("cancelled", "restore was cancelled");
    // Free what is loaded (an upscaler, a diffusion context): the DiT needs the whole card.
    contexts.dropOthers(nullptr);
    std::unique_ptr<sd_dgl_ctx_t, DglFree> dgl(sd_dgl_ctx_create_once());
    if (!dgl)
        fail("out_of_memory", "cannot create the DGL context");
    sd_dgl_ctx_set(dgl.get(), "embed_metadata", "off");
    struct Bar {
        fugue_helper::RestoreProgress tracker;
        const Hooks *hooks;
    } bar{{}, &hooks};
    sd_dgl_ctx_set_progress_callback(
        dgl.get(),
        [](const sd_generation_progress_t *progress, void *data) {
            auto *self = static_cast<Bar *>(data);
            if (!progress || !progress->stage)
                return;
            self->tracker.stage(progress->stage, progress->completed, progress->total);
            if (self->hooks->progress)
                self->hooks->progress(self->tracker.value());
        },
        &bar);
    g_restoreProgress = &bar.tracker;
    // A cancel asked while it loaded is heard here; later ones reach the run through sd_dgl_ctx_cancel.
    if (hooks.runningDgl && !hooks.runningDgl(dgl.get())) {
        g_restoreProgress = nullptr;
        fail("cancelled", "restore was cancelled");
    }
    const std::string source = fugue_helper::restoreScript(json(model).dump(), json(vae).dump(), static_cast<int>(tile),
                                                           std::min(0.5, double(overlap) / double(tile)), seed);
    const sd_dgl_tensor_input_t binding{"image", image.get()};
    sd_dgl_result_t *raw = nullptr;
    const sd_status_t status = sd_dgl_run(dgl.get(), source.c_str(), &binding, 1, &raw);
    std::unique_ptr<sd_dgl_result_t, DglResultFree> result(raw);
    g_restoreProgress = nullptr;
    if (hooks.runningDgl)
        hooks.runningDgl(nullptr);
    if (status == SD_STATUS_CANCELLED)
        fail("cancelled", "restore was cancelled");
    check(status, "restoring " + std::filesystem::path(input).filename().string());
    const sd_tensor_t *restored = sd_dgl_value_tensor(sd_dgl_value_field(sd_dgl_result_root(result.get()), "image"));
    if (!restored)
        fail("helper_failed", "no image was made");
    saveImage(restored, required(request, "output"));
    return {{"outputs", json::array({json{{"image", request["output"]}}})}};
}

json run(const json &request, Contexts &contexts, const Hooks &hooks = {}) {
    g_engineReason.reset(); // every operation starts without the last one's engine error
    const std::string op = request.value("op", "");
    const auto &running = hooks.running;
    if (op == "upscale")
        return runUpscale(request, contexts, hooks);
    if (op == "restore")
        return runRestore(request, contexts, hooks);
    if (op != "encode" && op != "sample" && op != "decode" && op != "vae_encode" && op != "generate")
        fail("input_invalid", "unknown op \"" + op + "\"; use encode, sample, decode, vae_encode, generate, upscale or restore");

    // A mask is read and checked first, so a wrong one is refused before any model loads.
    const json params = request.value("params", json::object());
    Tensor mask;
    if (request.contains("mask")) {
        if (op != "sample" && op != "generate")
            fail("input_invalid", "a mask is for sample or generate");
        mask = loadMask(required(request, "mask"), params.value("width", 512), params.value("height", 512));
    }

    // The context first: whether it makes video (Wan, ...) says which options and which call.
    sd_ctx_t *ctx = contexts.get(request.value("context", json::object()));
    const bool video = sd_ctx_supports_video_generation(ctx);
    if (op == "vae_encode" && video != request.contains("video"))
        fail("input_invalid", video ? "a video model's VAE encode takes a clip: \"video\" (an image model's takes \"image\")"
                                    : "an image model's VAE encode takes \"image\"; \"video\" is for a video model");
    if (request.contains("init_video") && (!video || op != "generate"))
        fail("input_invalid", "\"init_video\" is the clip a video model's generate redraws");
    Opt options(video ? sd_vid_gen_opt_create() : sd_img_gen_opt_create());
    if (!options)
        fail("out_of_memory", "cannot create the options");
    for (const auto &[key, value] : params.items())
        setOption(options.get(), key, value);
    // Strength below 1 is a noise level for a model on a shifted flow schedule (strength_args.h, #244).
    // A video model's strength is read by its own pipeline.
    if (!video && params.contains("strength") && params["strength"].is_number()) {
        const std::string args = params.value("sample.args", std::string());
        const std::string sent = fugue_helper::strengthSampleArgs(args, sd_get_model_version_name(ctx), params["strength"].get<double>());
        if (sent != args)
            check(sd_opt_set_string(options.get(), "sample.args", sent.c_str()), "sample.args");
    }
    // Video to video: the clip's own size, rate and length are the request's.
    const std::string clipFile = op == "vae_encode" && video ? required(request, "video") : request.value("init_video", "");
    if (!clipFile.empty()) {
        const Y4m clip = readY4mHeader(clipFile);
        check(sd_opt_set_int(options.get(), "width", clip.width), "width");
        check(sd_opt_set_int(options.get(), "height", clip.height), "height");
        check(sd_opt_set_int(options.get(), "video_frames", clip.frames), "video_frames");
        check(sd_opt_set_int(options.get(), "fps", clip.fps), "fps");
    }

    // A mask: the start picture, or the start latent, by the model (see the header).
    const bool inpaint = mask && isInpaintVersion(sd_get_model_version_name(ctx));
    if (mask) {
        if (video)
            fail("input_invalid", "a mask is for an image model; a video has none");
        if (op == "generate" ? !params.contains("init_image") : !request.contains(inpaint ? "image" : "init_latent"))
            fail("input_invalid", inpaint ? sd_get_model_version_name(ctx) + std::string(" needs the picture to redraw (image, init_image) with its mask")
                                          : "a mask needs the picture or its latent to redraw");
        check(sd_opt_set_tensor(options.get(), "mask_image", mask.get()), "mask_image");
    }

    // A start picture (image-to-video: Wan 2.2 I2V's first frame, which the encode turns into its
    // conditioning, and Wan 2.2 TI2V's, which the sample reads): "image" of encode and sample.
    if ((op == "encode" || op == "sample") && request.contains("image") && !mask) {
        if (!video)
            fail("input_invalid", "a start image is for a video model; an image model starts from a latent (vae_encode, then sample)");
        check(sd_opt_set_tensor(options.get(), "init_image", loadImage(required(request, "image")).get()), "init_image");
    }

    json outputs = json::array();
    auto setPath = [&](const char *key, const std::string &path) {
        check(sd_opt_set_string(options.get(), key, path.c_str()), key);
    };
    std::string imageOut, videoOut;
    if (op == "encode") {
        const std::string cond = required(request, "conditioning");
        setPath("fugue.cond_out", cond);
        setPath("fugue.stop_after", "cond");
        outputs.push_back({{"conditioning", cond}});
    } else if (op == "sample") {
        const std::string latent = required(request, "latent");
        setPath("fugue.cond_in", required(request, "conditioning"));
        if (request.contains("init_latent")) {
            // An inpaint model starts from the picture (its masked picture is built from it).
            if (!inpaint)
                setPath("fugue.init_latent_in", required(request, "init_latent"));
        }
        if (inpaint)
            check(sd_opt_set_tensor(options.get(), "init_image", loadImage(required(request, "image")).get()), "init_image");
        setPath("fugue.latent_out", latent);
        setPath("fugue.stop_after", "sample");
        outputs.push_back({{"latent", latent}});
    } else if (op == "decode") {
        setPath("fugue.latent_in", required(request, "latent"));
        (video ? videoOut : imageOut) = required(request, video ? "video" : "image");
    } else if (op == "vae_encode") {
        const std::string latent = required(request, "latent");
        if (video)
            setPath("fugue.init_video", clipFile);
        else
            check(sd_opt_set_tensor(options.get(), "init_image", loadImage(required(request, "image")).get()), "init_image");
        setPath("fugue.init_latent_out", latent);
        setPath("fugue.stop_after", "init");
        outputs.push_back({{"latent", latent}});
    } else {
        if (!clipFile.empty())
            setPath("fugue.init_video", clipFile);
        (video ? videoOut : imageOut) = required(request, video ? "video" : "image");
    }

    sd_result_t *result = nullptr;
    PreviewScope previews(request, hooks, video, sd_get_model_version_name(ctx));
    // sd_generate_image clears a cancel asked before it starts, so that one is heard here.
    if (running && !running(ctx))
        fail("cancelled", op + " was cancelled");
    const auto generate = [&] {
        return video ? sd_generate_video(ctx, options.get(), &result) : sd_generate_image(ctx, options.get(), &result);
    };
    sd_status_t status = generate();
    if (status == SD_STATUS_OUT_OF_MEMORY && contexts.dropOthers(ctx))
        status = generate();
    if (running)
        running(nullptr);
    if (status == SD_STATUS_CANCELLED)
        fail("cancelled", op + " was cancelled");
    check(status, op);
    std::unique_ptr<sd_result_t, void (*)(sd_result_t *)> resultOwner(result, sd_result_free);
    if (!videoOut.empty()) {
        const sd_video_t *clip = sd_result_get_video(result);
        if (!clip)
            fail("helper_failed", "no video was made: " + lastError());
        saveVideo(*clip, videoOut);
        outputs.push_back({{"video", videoOut}});
    }
    if (!imageOut.empty()) {
        const size_t count = sd_result_get_image_count(result);
        if (count == 0)
            fail("helper_failed", "no image was made");
        for (size_t i = 0; i < count; ++i) {
            std::filesystem::path path = imageOut;
            if (count > 1) // out.png -> out-0.png, out-1.png, ...
                path.replace_filename(path.stem().string() + "-" + std::to_string(i) + path.extension().string());
            saveImage(sd_result_get_image(result, i), path.string());
            outputs.push_back({{"image", path.string()}});
        }
    }
    return {{"outputs", outputs}};
}

json errorJson(const std::string &code, const std::string &message) {
    return {{"code", code}, {"message", message}};
}

// Runs one operation: the reply, {"outputs": [...]} or {"error": ...}.
json attempt(const std::function<json()> &call) {
    try {
        return call();
    } catch (const Failure &f) {
        return {{"error", errorJson(f.code, f.message)}};
    } catch (const std::bad_alloc &) {
        return {{"error", errorJson("out_of_memory", "out of memory")}};
    } catch (const std::exception &e) {
        return {{"error", errorJson("input_invalid", e.what())}};
    }
}

namespace fs = std::filesystem;

// SHA-256 (FIPS 180-4) of a file, read in 8 MB chunks. stable-diffusion.cpp has no reusable one.
class Sha256 {
public:
    void update(const unsigned char *data, size_t size) {
        m_total += size;
        while (size > 0) {
            const size_t take = std::min(size, sizeof m_block - m_used);
            std::memcpy(m_block + m_used, data, take);
            m_used += take;
            data += take;
            size -= take;
            if (m_used == sizeof m_block) {
                compress(m_block);
                m_used = 0;
            }
        }
    }

    std::string hex() {
        const uint64_t bits = m_total * 8;
        const unsigned char one = 0x80;
        update(&one, 1);
        const unsigned char zero = 0;
        while (m_used != 56)
            update(&zero, 1);
        unsigned char length[8];
        for (int i = 0; i < 8; ++i)
            length[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(length, 8);
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (const uint32_t word : m_state)
            for (int shift = 28; shift >= 0; shift -= 4)
                out += digits[(word >> shift) & 15];
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const unsigned char *block) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[4 * i]) << 24) | (uint32_t(block[4 * i + 1]) << 16) | (uint32_t(block[4 * i + 2]) << 8) | block[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    uint32_t m_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char m_block[64] = {};
    size_t m_used = 0;
    uint64_t m_total = 0;
};

// The hash of a file; empty if `cancelled` says stop or the file cannot be read.
std::string sha256OfFile(const fs::path &path, const std::function<bool()> &cancelled) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::vector<unsigned char> chunk(8u * 1024 * 1024);
    Sha256 hash;
    while (in) {
        in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        hash.update(chunk.data(), static_cast<size_t>(in.gcount()));
        if (cancelled && cancelled())
            return {};
    }
    return in.eof() ? hash.hex() : std::string();
}

// A handle's "sha256" ("sha256:<hex>" or bare hex) as lower-case hex; empty if the handle has none.
std::string wantedSha256(const std::string &key, const json &handle) {
    if (!handle.contains("sha256") || handle["sha256"].is_null())
        return {};
    std::string text = handle["sha256"].is_string() ? handle["sha256"].get<std::string>() : std::string();
    if (text.compare(0, 7, "sha256:") == 0)
        text.erase(0, 7);
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (text.size() != 64 || text.find_first_not_of("0123456789abcdef") != std::string::npos)
        fail("input_invalid", key + ": the model's \"sha256\" is not 64 hex digits");
    return text;
}

// --serve-http: the model files of the host, found by handle under the --models roots (8.9).
class Models {
public:
    // `hashFile`: where the hashes of the model files are kept between runs (empty: not kept).
    explicit Models(const std::vector<fs::path> &roots, fs::path hashFile = {}) : m_hashFile(std::move(hashFile)) {
        for (const fs::path &root : roots) {
            m_roots.push_back(fs::absolute(root).lexically_normal());
            std::error_code ec;
            m_canonRoots.push_back(fs::weakly_canonical(m_roots.back(), ec));
        }
        scan();
        loadHashes();
    }

    const std::vector<fs::path> &roots() const { return m_roots; }

    // A path given as a string is used only inside a root: no other host file is read. Symlinks are
    // resolved first (a link under a root that points out of it is outside); a path that does not exist
    // yet is checked through its closest existing parent.
    bool inside(const fs::path &path) const {
        if (path.empty() || !path.is_absolute())
            return false;
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(path, ec);
        if (ec || canonical.empty())
            return false;
        for (const fs::path &root : m_canonRoots) {
            const fs::path rel = canonical.lexically_relative(root);
            if (!rel.empty() && *rel.begin() != ".." && rel != ".")
                return true;
        }
        return false;
    }

    // The file a handle names, or a model_missing failure. `key` ("vae_path", "loras") picks the
    // type folders tried next to the plain name. With a "sha256" only a file of that content is
    // accepted (8.9): it hashes the candidates, which takes a while the first time; `cancelled`,
    // when given, is polled while it reads.
    struct Wanted {
        std::string name;
        std::optional<uintmax_t> size;
        std::string sha; // lower-case hex, or empty
    };

    // The handle's fields, or input_invalid.
    static Wanted parse(const std::string &key, const json &handle) {
        if (!handle.contains("name") || !handle["name"].is_string())
            fail("input_invalid", key + ": a model handle needs a \"name\"");
        Wanted w;
        w.name = handle["name"].get<std::string>();
        const fs::path path(w.name);
        if (w.name.empty() || path.is_absolute() || w.name.front() == '/' || w.name.find('\\') != std::string::npos)
            fail("input_invalid", key + ": the model name \"" + w.name + "\" is not a relative path");
        for (const fs::path &part : path)
            if (part == "..")
                fail("input_invalid", key + ": the model name \"" + w.name + "\" has \"..\"");
        if (handle.contains("size") && handle["size"].is_number_unsigned())
            w.size = handle["size"].get<uintmax_t>();
        w.sha = wantedSha256(key, handle);
        return w;
    }

    std::string resolve(const std::string &key, const json &handle, const std::function<bool()> &cancelled = {}) {
        const Wanted want = parse(key, handle);
        const std::string &name = want.name;
        const std::optional<uintmax_t> &size = want.size;
        const std::string &sha = want.sha;
        const fs::path wanted(name);

        std::vector<std::string> tried; // the candidates whose content was seen, in order
        std::vector<std::string> candidates;
        for (int pass = 0; pass < 2; ++pass) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (pass == 1)
                    scan(); // a file added since the start
                candidates = look(wanted, key, size);
            }
            for (const std::string &path : candidates) {
                if (std::find(tried.begin(), tried.end(), path) != tried.end())
                    continue;
                if (sha.empty())
                    return path;
                tried.push_back(path);
                if (hashOf(path, cancelled) == sha)
                    return path;
            }
        }
        std::string searched;
        for (const fs::path &root : m_roots)
            searched += (searched.empty() ? "" : ", ") + root.string();
        if (tried.empty())
            fail("model_missing", "the model file \"" + name + "\"" + (size ? " of " + std::to_string(*size) + " bytes" : "") +
                                      " is not under " + searched);
        std::string others;
        for (const std::string &path : tried) {
            std::error_code ec;
            others += (others.empty() ? "" : ", ") + path + " (" + std::to_string(fs::file_size(path, ec)) + " bytes)";
        }
        fail("model_missing", "no file named \"" + name + "\" has the content asked for (sha256 " + sha + "): the sha256 differs for " + others);
    }

private:
    struct Entry {
        std::string rel; // relative to its root, with '/'
        fs::path path;
        uintmax_t size;
    };

    static std::vector<std::string> folders(const std::string &key) {
        static const std::map<std::string, std::vector<std::string>> byKey{
            {"diffusion_model_path", {"diffusion_models", "unet", "checkpoints"}},
            {"model_path", {"checkpoints", "diffusion_models"}},
            {"vae_path", {"vae"}},
            {"taesd_path", {"vae_approx", "vae"}},
            {"llm_path", {"text_encoders", "clip"}},
            {"clip_l_path", {"text_encoders", "clip"}},
            {"clip_g_path", {"text_encoders", "clip"}},
            {"clip_vision_path", {"clip_vision"}},
            {"tokenizer", {"tokenizers"}},
            {"t5xxl_path", {"text_encoders", "clip"}},
            {"controlnet_path", {"controlnet"}},
            {"loras", {"loras"}},
        };
        const auto it = byKey.find(key);
        return it == byKey.end() ? std::vector<std::string>{} : it->second;
    }

    static bool fits(const fs::path &file, std::optional<uintmax_t> size) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec))
            return false;
        return !size || fs::file_size(file, ec) == *size;
    }

    // Every file the handle's name can mean, best match first, without repeats.
    std::vector<std::string> look(const fs::path &name, const std::string &key, std::optional<uintmax_t> size) const {
        std::vector<std::string> found;
        auto add = [this, &found](const fs::path &path) {
            const std::string text = path.string();
            if (!inside(path)) // a link under a root that leads out of it is no model of this host's
                return;
            if (std::find(found.begin(), found.end(), text) == found.end())
                found.push_back(text);
        };
        std::vector<fs::path> prefixes{fs::path()};
        for (const std::string &folder : folders(key))
            prefixes.push_back(folder);
        for (const fs::path &root : m_roots)
            for (const fs::path &prefix : prefixes) {
                const fs::path candidate = root / prefix / name;
                if (fits(candidate, size))
                    add(candidate);
            }
        const std::string tail = name.generic_string();
        for (const Entry &e : m_files) {
            const bool ends = e.rel == tail || (e.rel.size() > tail.size() && e.rel.compare(e.rel.size() - tail.size(), tail.size(), tail) == 0 &&
                                                e.rel[e.rel.size() - tail.size() - 1] == '/');
            if (ends && (!size || e.size == *size))
                add(e.path);
        }
        const std::string base = name.filename().string();
        for (const Entry &e : m_files)
            if (fs::path(e.rel).filename() == base && (!size || e.size == *size))
                add(e.path);
        return found;
    }

    struct Hash {
        uintmax_t size = 0;
        int64_t mtime = 0;
        std::string hex;
    };

    // The SHA-256 of a file, computed once per (path, size, mtime) and kept; empty if cancelled.
    std::string hashOf(const std::string &path, const std::function<bool()> &cancelled) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        const int64_t mtime = static_cast<int64_t>(fs::last_write_time(path, ec).time_since_epoch().count());
        {
            std::lock_guard<std::mutex> lock(m_hashMutex);
            const auto known = m_hashes.find(path);
            if (known != m_hashes.end() && known->second.size == size && known->second.mtime == mtime)
                return known->second.hex;
        }
        std::fprintf(stderr, "fugue-helper: hashing %s (%ju bytes)\n", path.c_str(), static_cast<uintmax_t>(size));
        const std::string hex = sha256OfFile(path, cancelled);
        if (hex.empty()) {
            if (cancelled && cancelled())
                fail("cancelled", "cancelled while hashing " + path);
            return {};
        }
        std::lock_guard<std::mutex> lock(m_hashMutex);
        m_hashes[path] = {size, mtime, hex};
        saveHashes();
        return hex;
    }

    void loadHashes() {
        if (m_hashFile.empty())
            return;
        std::ifstream in(m_hashFile);
        const json doc = json::parse(in, nullptr, false);
        if (doc.is_discarded() || !doc.is_object())
            return;
        for (const auto &[path, entry] : doc.items())
            if (entry.is_object() && entry.contains("size") && entry.contains("mtime") && entry.contains("sha256") && entry["sha256"].is_string() &&
                entry["size"].is_number_unsigned() && entry["mtime"].is_number_integer())
                m_hashes[path] = {entry["size"].get<uintmax_t>(), entry["mtime"].get<int64_t>(), entry["sha256"].get<std::string>()};
    }

    // Under m_hashMutex. Written next to the file and renamed over it, so a crash leaves the old one.
    void saveHashes() const {
        if (m_hashFile.empty())
            return;
        json doc = json::object();
        for (const auto &[path, h] : m_hashes)
            doc[path] = {{"size", h.size}, {"mtime", h.mtime}, {"sha256", h.hex}};
        std::error_code ec;
        fs::create_directories(m_hashFile.parent_path(), ec);
        const fs::path temp = m_hashFile.string() + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << doc.dump(1) << "\n";
            if (!out.flush())
                return;
        }
        fs::rename(temp, m_hashFile, ec);
        if (ec)
            fs::remove(temp, ec);
    }

    // Recursive, symlinks followed (to a depth, against loops), dot-names skipped.
    void walk(const fs::path &dir, const std::string &rel, int depth) {
        if (depth > 12)
            return;
        std::error_code ec;
        std::vector<fs::path> entries;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            entries.push_back(it->path());
        std::sort(entries.begin(), entries.end());
        for (const fs::path &path : entries) {
            const std::string name = path.filename().string();
            if (name.empty() || name[0] == '.')
                continue;
            const std::string childRel = rel.empty() ? name : rel + "/" + name;
            if (fs::is_directory(path, ec))
                walk(path, childRel, depth + 1);
            else if (fs::is_regular_file(path, ec))
                m_files.push_back({childRel, path, fs::file_size(path, ec)});
        }
    }

    void scan() {
        m_files.clear();
        for (const fs::path &root : m_roots)
            walk(root, "", 0);
    }

    std::vector<fs::path> m_roots;
    std::vector<fs::path> m_canonRoots; // m_roots with symlinks resolved
    std::vector<Entry> m_files;
    std::mutex m_mutex;
    fs::path m_hashFile;
    std::map<std::string, Hash> m_hashes;
    std::mutex m_hashMutex;
};

// The bytes of a base64 string (RFC 4648, the URL-safe alphabet and white space tolerated).
std::string base64Decode(const std::string &text) {
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+' || c == '-')
            v = 62;
        else if (c == '/' || c == '_')
            v = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ')
            continue;
        else
            fail("input_invalid", "an input's data is not base64");
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

// Equal strings, in a time that does not tell where they differ.
bool sameBytes(const std::string &a, const std::string &b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    return diff == 0;
}

// MemAvailable of /proc/meminfo, in bytes (the GB10 shares one memory, spec 8.10); the device's
// own free memory where there is no such file.
size_t availableMemory() {
    std::ifstream in("/proc/meminfo");
    std::string label;
    unsigned long long kb = 0;
    while (in >> label) {
        if (label == "MemAvailable:" && in >> kb)
            return static_cast<size_t>(kb) * 1024;
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return freeDeviceMemory();
}

// A model handle with a sha256, resolved in its job (hashing a big file takes a while): where its
// path goes in the job's request (a JSON pointer), and the handle.
struct PendingModel {
    std::string pointer;
    std::string key;
    json handle;
};

// --serve: the provider protocol of spec 8.8 over stdio.
class Server {
public:
    Server() = default;
    // Over HTTP: `models` names the model files, `base` holds the jobs' work dirs.
    Server(Models *models, fs::path base) : m_models(models), m_base(std::move(base)) {}

    int serve() {
        sd_set_progress_callback(&Server::progressed, this);
        std::thread worker([this] { work(); });
        worker.detach(); // ended with the process (_Exit): a running job is not waited for
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty())
                continue;
            const json message = json::parse(line, nullptr, false);
            if (message.is_discarded() || !message.is_object()) {
                say({{"error", errorJson("input_invalid", "a line that is not a JSON object")}});
                continue;
            }
            if (message.value("op", "") == "shutdown") {
                say({{"id", message.value("id", json())}});
                break;
            }
            json reply = handle(message);
            reply["id"] = message.value("id", json());
            say(reply);
        }
        // Without the exit-time teardown, as the one-shot run (main()).
        std::fflush(stdout);
        std::_Exit(0);
    }

    int serveHttp(const std::string &address, int port, const std::string &key) {
        sd_set_progress_callback(&Server::progressed, this);
        std::thread worker([this] { work(); });
        worker.detach();
        httplib::Server http;
        http.set_payload_max_length(512u * 1024 * 1024);
        http.set_pre_routing_handler([key](const httplib::Request &req, httplib::Response &res) {
            const std::string given = req.get_header_value("Authorization");
            const std::string prefix = "Bearer ";
            if (given.compare(0, prefix.size(), prefix) == 0 && sameBytes(given.substr(prefix.size()), key))
                return httplib::Server::HandlerResponse::Unhandled;
            reply(res, {{"error", errorJson("unauthorized", "a valid \"Authorization: Bearer <key>\" is required")}});
            return httplib::Server::HandlerResponse::Handled;
        });
        http.set_error_handler([](const httplib::Request &, httplib::Response &res) {
            if (!res.body.empty())
                return;
            const std::string code = res.status == 404 ? "not_found" : res.status == 413 ? "payload_too_large" : "input_invalid";
            reply(res, {{"error", errorJson(code, "HTTP " + std::to_string(res.status))}}, res.status);
        });
        http.set_exception_handler([](const httplib::Request &, httplib::Response &res, std::exception_ptr) {
            reply(res, {{"error", errorJson("helper_failed", "the helper failed on this request")}});
        });
        auto post = [&](const char *name, std::function<json(const json &)> call) {
            http.Post(std::string("/fugue/v1/") + name, [call](const httplib::Request &req, httplib::Response &res) {
                json body = json::object();
                if (!req.body.empty()) {
                    body = json::parse(req.body, nullptr, false);
                    if (body.is_discarded() || !body.is_object())
                        return reply(res, {{"error", errorJson("input_invalid", "the body is not a JSON object")}});
                }
                reply(res, attempt([&] { return call(body); }));
            });
        };
        post("describe", [this](const json &) {
            return json{{"protocol", 1},
                        {"engine", "sdcpp"},
                        {"shares_filesystem", false},
                        {"backend", FUGUE_BACKEND},
                        {"capabilities", {"text.encode", "latent.sample", "latent.decode", "latent.encode", "image.upscale", "image.restore"}},
                        {"free_memory_bytes", availableMemory()},
                        {"host", hostName()}};
        });
        post("submit", [this](const json &body) { return submitHttp(body); });
        for (const char *op : {"status", "cancel", "result"})
            post(op, [this, op](const json &body) {
                json message = body;
                message["op"] = op;
                return handle(message);
            });
        post("shutdown", [](const json &) { return json::object(); }); // the host runs the server
        http.Get(R"(/fugue/v1/result/([^/]+)/(\d+))", [this](const httplib::Request &req, httplib::Response &res) {
            const std::string file = fileOf(req.matches[1], std::stoul(req.matches[2]));
            std::ifstream in(file, std::ios::binary);
            if (file.empty() || !in)
                return reply(res, {{"error", errorJson("input_invalid", "no output file " + req.matches[2].str() + " of job \"" + req.matches[1].str() + "\"")}}, 404);
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            res.set_content(std::move(bytes), "application/octet-stream");
        });
        if (!http.bind_to_port(address, port)) {
            std::fprintf(stderr, "fugue-helper: cannot listen on %s:%d\n", address.c_str(), port);
            return 1;
        }
        std::fprintf(stderr, "fugue-helper: serving http://%s:%d/fugue/v1/ (models: %zu roots)\n", address.c_str(), port, m_models->roots().size());
        http.listen_after_bind();
        return 0;
    }

private:
    struct Job {
        json request; // the one-shot form
        fs::path dir; // HTTP: its work dir
        std::vector<std::string> files; // HTTP: the output files, as result/<job>/<i> indexes them
        json resolved = json::array(); // HTTP: the model handles it named, and the files they became
        std::vector<PendingModel> pending; // HTTP: handles with a sha256, resolved when the job starts
        uint64_t finishedSeq = 0;
        std::chrono::steady_clock::time_point finishedAt;
        std::string stage;
        std::string state = "queued";
        json outputs = json::object();
        json error;
        bool cancel = false;
    };

    // A reply as JSON; an error's HTTP status follows its code.
    static void reply(httplib::Response &res, const json &body, int status = 0) {
        if (status == 0) {
            status = 200;
            if (body.contains("error")) {
                const std::string code = body["error"].value("code", "");
                const std::string message = body["error"].value("message", "");
                status = code == "unauthorized" ? 401
                         : code == "input_invalid" && message.compare(0, 6, "no job") == 0 ? 404
                         : code == "input_invalid" || code == "model_missing" ? 400
                                                                                : 500;
            }
        }
        res.status = status;
        res.set_content(body.dump(), "application/json");
    }

    static std::string hostName() {
        char name[256] = {};
        return gethostname(name, sizeof name - 1) == 0 ? name : "";
    }

    std::string fileOf(const std::string &id, size_t index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_jobs.find(id);
        return found == m_jobs.end() || index >= found->second.files.size() ? std::string() : found->second.files[index];
    }

    // Under m_mutex: a finished job's files stay for an hour or until 20 newer jobs have finished.
    void purge() {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> done;
        for (const auto &[id, job] : m_jobs)
            if (job.finishedSeq)
                done.push_back(id);
        std::sort(done.begin(), done.end(), [this](const std::string &a, const std::string &b) { return m_jobs[a].finishedSeq > m_jobs[b].finishedSeq; });
        for (size_t i = 0; i < done.size(); ++i) {
            const Job &job = m_jobs[done[i]];
            if (i < 20 && now - job.finishedAt < std::chrono::hours(1))
                continue;
            std::error_code ignored;
            fs::remove_all(job.dir, ignored);
            m_progress.erase(done[i]);
            m_jobs.erase(done[i]);
        }
    }

    // Writes one input's data into the job's dir; the {"path"} the stdio protocol takes.
    json storeInput(const json &input, const fs::path &dir, const std::string &name, size_t index) {
        if (!input.is_object() || !input.contains("data") || !input["data"].is_string())
            fail("input_invalid", "input \"" + name + "\" needs {\"data\": base64, \"format\"}");
        const std::string format = input.value("format", "");
        const std::string ext = format == "png" ? ".png" : ".bin";
        // The name is the caller's: only letters, digits, - and _ reach the file's name, so it stays in `dir`.
        std::string safe;
        for (const char c : name)
            safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
        const fs::path path = dir / (safe.substr(0, 64) + "-" + std::to_string(index) + ext);
        const std::string bytes = base64Decode(input["data"].get<std::string>());
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out.flush())
            fail("helper_failed", "cannot write " + path.string());
        return {{"path", path.string()}};
    }

    // A model value: a path inside the roots as it is, or a handle's file; recorded in `used`.
    json modelValue(const std::string &key, const json &value, json &used, const std::string &pointer, std::vector<PendingModel> &pending) {
        if (value.is_string()) {
            if (!m_models->inside(value.get<std::string>()))
                fail("input_invalid", key + ": \"" + value.get<std::string>() + "\" is not inside the model folders; name the file by a handle");
            return value;
        }
        if (!value.is_object())
            fail("input_invalid", key + ": a model is a path or a handle");
        if (!Models::parse(key, value).sha.empty()) { // hashed in the job, not in this request
            pending.push_back({pointer, key, value});
            return "";
        }
        const std::string path = m_models->resolve(key, value);
        json entry = value;
        entry["key"] = key;
        entry["path"] = path;
        used.push_back(entry);
        return path;
    }

    // The context options a remote client may set (#545): the model files, by handle or by a path inside the
    // roots, and the one switch Fugue sends. Any other key is refused, so a client cannot reach an engine
    // option that opens a host file or a network connection (rpc_servers, ...).
    static bool contextKeyAllowed(const std::string &key) {
        static const std::set<std::string> allowed{
            "model_path",       "diffusion_model_path", "high_noise_diffusion_model_path", "vae_path",
            "taesd_path",       "llm_path",             "clip_l_path",                     "clip_g_path",
            "clip_vision_path", "t5xxl_path",           "controlnet_path",                 "tokenizer",
            "sage_attn",
        };
        return allowed.count(key) != 0;
    }

    // The request over HTTP made ready for submit(): paths only inside the roots, handles
    // resolved, input data written into a work dir of the job.
    json submitHttp(const json &body) {
        const std::string capability = body.value("capability", "");
        if (capability.empty())
            fail("input_invalid", "\"capability\" is required");
        json message = body;
        json used = json::array();
        std::vector<PendingModel> pending;
        json params = body.value("params", json::object());
        if (!params.is_object())
            fail("input_invalid", "\"params\" is not an object");
        for (auto &[key, value] : params.items()) {
            if (key == "context" && !value.is_object())
                fail("input_invalid", "\"context\" is not an object");
            if (key == "context") {
                for (auto &[ckey, cvalue] : value.items()) {
                    if (!contextKeyAllowed(ckey))
                        fail("input_invalid", "context option \"" + ckey + "\" is not one a remote client may set");
                    if ((ckey.size() > 5 && ckey.compare(ckey.size() - 5, 5, "_path") == 0) || ckey == "tokenizer") {
                        cvalue = modelValue(ckey, cvalue, used, "/context/" + ckey, pending);
                        if (ckey == "tokenizer" && cvalue.is_string())
                            if (const std::string why = fugue_helper::tokenizerPathError(cvalue.get<std::string>()); !why.empty())
                                fail("input_invalid", why);
                    }
                }
            } else if (key == "loras" && value.is_array()) {
                size_t index = 0;
                for (json &lora : value) {
                    if (!lora.is_object() || !lora.contains("path"))
                        fail("input_invalid", "a lora needs a \"path\"");
                    lora["path"] = modelValue("loras", lora["path"], used, "/params/loras/" + std::to_string(index++) + "/path", pending);
                }
            } else if (key.size() > 5 && key.compare(key.size() - 5, 5, "_path") == 0 && (value.is_string() || value.is_object())) {
                value = modelValue(key, value, used, "/params/" + key, pending);
            }
        }
        // Nothing else of the host's is read: an image option names no host file.
        auto noHostImage = [](const json &v) {
            if (isImage(v))
                fail("input_invalid", "an {\"image\": PATH} option reads a host file; send the image as an input");
        };
        for (const auto &[key, value] : params.items()) {
            noHostImage(value);
            if (value.is_array())
                for (const json &item : value)
                    noHostImage(item);
        }

        std::error_code ec;
        fs::create_directories(m_base, ec);
        std::string pattern = (m_base / "job-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            fail("helper_failed", "cannot make a work dir under " + m_base.string());
        const fs::path dir = pattern;
        try {
            // An image option (ref_images, ...) arrives as {"data", "format"}; it becomes the {"image": PATH}
            // of a file in the job's dir.
            auto imageOption = [&](const json &v, const std::string &name, size_t index) -> json {
                return {{"image", storeInput(v, dir, name, index)["path"]}};
            };
            auto isData = [](const json &v) { return v.is_object() && v.contains("data"); };
            for (auto &[key, value] : params.items()) {
                if (isData(value)) {
                    value = imageOption(value, "param-" + key, 0);
                } else if (value.is_array()) {
                    for (size_t i = 0; i < value.size(); ++i) {
                        // A list item is {"image": ...} for the engine; the client sends {"image": {data}} or {data}.
                        if (isData(value[i]))
                            value[i] = imageOption(value[i], "param-" + key, i);
                        else if (value[i].is_object() && value[i].contains("image") && isData(value[i]["image"]))
                            value[i] = imageOption(value[i]["image"], "param-" + key, i);
                    }
                }
            }
            message["params"] = params;
            json inputs = body.value("inputs", json::object());
            if (!inputs.is_object())
                fail("input_invalid", "\"inputs\" is not an object");
            for (auto &[name, value] : inputs.items()) {
                if (value.is_array()) {
                    json list = json::array();
                    for (size_t i = 0; i < value.size(); ++i)
                        list.push_back(storeInput(value[i], dir, name, i));
                    value = list;
                } else {
                    value = storeInput(value, dir, name, 0);
                }
            }
            message["inputs"] = inputs;
            message["output_dir"] = dir.string();
            json out = submit(message, dir, used, pending);
            return out;
        } catch (...) {
            fs::remove_all(dir, ec);
            throw;
        }
    }

    void say(const json &message) {
        if (m_models)
            return; // over HTTP, state is asked for
        const std::string text = message.dump() + "\n";
        std::lock_guard<std::mutex> lock(m_out);
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }

    // Under m_mutex.
    json statusOf(const std::string &id, const Job &job) const {
        json status{{"op", "status"}, {"job", id}, {"state", job.state}, {"stage", job.stage},
                    {"progress", job.state == "done" ? 1.0 : m_progress.count(id) ? m_progress.at(id) : 0.0}};
        if (!job.error.is_null())
            status["error"] = job.error;
        return status;
    }

    json handle(const json &message) {
        const std::string op = message.value("op", "");
        if (op == "submit")
            return submit(message);
        const std::string id = message.value("job", "");
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_models)
            purge();
        const auto found = m_jobs.find(id);
        if (op != "status" && op != "cancel" && op != "result")
            return {{"error", errorJson("input_invalid", "unknown op \"" + op + "\"")}};
        if (found == m_jobs.end())
            return {{"error", errorJson("input_invalid", "no job \"" + id + "\"")}};
        Job &job = found->second;
        if (op == "status") {
            json status = statusOf(id, job);
            status.erase("op");
            status.erase("job");
            return status;
        }
        if (op == "cancel") {
            if (job.state == "queued") {
                job.state = "cancelled";
                say(statusOf(id, job));
                if (m_models) {
                    std::error_code ignored;
                    fs::remove_all(job.dir, ignored);
                }
                m_jobs.erase(found); // work() passes over an id it no longer has
                m_progress.erase(id);
                return {{"state", "cancelled"}};
            } else if (job.state == "running") {
                job.cancel = true;
                if (m_running)
                    sd_cancel_generation(m_running, SD_CANCEL_ALL);
                if (m_runningDgl)
                    sd_dgl_ctx_cancel(m_runningDgl);
            }
            return {{"state", job.state}};
        }
        if (job.state != "done")
            return {{"error", errorJson("input_invalid", "job \"" + id + "\" is " + job.state + ", not done")}};
        // The options it ran with, as it was given them (the library reports no others).
        json used = job.request.value("params", json::object());
        used["engine"] = "sdcpp";
        if (m_models) { // the files stay to be fetched, until purge()
            used["models"] = job.resolved;
            return {{"outputs", job.outputs}, {"used", used}};
        }
        const json reply{{"outputs", job.outputs}, {"used", used}};
        // Fugue fetches a result once, at once: the job is done with.
        m_progress.erase(id);
        m_jobs.erase(found);
        return reply;
    }

    json submit(const json &message, const fs::path &workDir = {}, const json &resolved = json::array(), const std::vector<PendingModel> &pending = {}) {
        static const std::map<std::string, std::string> ops{{"text.encode", "encode"},
                                                            {"latent.sample", "sample"},
                                                            {"latent.decode", "decode"},
                                                            {"latent.encode", "vae_encode"},
                                                            {"image.upscale", "upscale"},
                                                            {"image.restore", "restore"}};
        const std::string capability = message.value("capability", "");
        const auto op = ops.find(capability);
        if (op == ops.end())
            return {{"error", errorJson("input_invalid", "no capability \"" + capability + "\"")}};
        json params = message.value("params", json::object());
        json request{{"op", op->second}, {"context", params.value("context", json::object())}};
        params.erase("context");
        request["params"] = params;
        const json inputs = message.value("inputs", json::object());
        for (const char *name : {"conditioning", "latent", "init_latent", "image", "mask", "frames", "video"})
            if (inputs.contains(name) && inputs[name].is_object() && inputs[name].contains("path"))
                request[name] = inputs[name]["path"];
        const std::filesystem::path dir = message.value("output_dir", "");
        if (dir.empty())
            return {{"error", errorJson("input_invalid", "\"output_dir\" is required")}};
        if (op->second == "encode")
            request["conditioning"] = (dir / "conditioning.bin").string();
        else if (op->second == "sample" || op->second == "vae_encode")
            request["latent"] = (dir / "latent.bin").string();
        else if (op->second == "restore")
            request["output"] = (dir / "image.png").string();
        else if (op->second == "upscale") {
            request["output"] = (dir / "image.png").string(); // "image" stays the input
            request["frames_output"] = (dir / "frames-out").string();
        }
        else {
            request["image"] = (dir / "image.png").string(); // a video model writes the video instead
            request["video"] = (dir / "video.y4m").string();
        }

        // A frame of the sampling for Fugue's card (stdio only: over HTTP nothing fetches it). An older
        // helper never reads this key, so a newer Fugue's request still runs there, without frames.
        if (!m_models && op->second == "sample" && message.value("preview", json()).is_string()) {
            request["preview"] = message["preview"];
            request["preview_file"] = (dir / "preview.png").string();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string id = std::to_string(++m_count);
        Job job;
        job.request = request;
        job.stage = op->second;
        job.dir = workDir;
        job.resolved = resolved;
        job.pending = pending;
        m_jobs[id] = job;
        m_queue.push_back(id);
        m_wake.notify_one();
        return {{"job", id}};
    }

    void work() {
        Contexts contexts;
        for (;;) {
            std::string id;
            json request;
            std::vector<PendingModel> pending;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return !m_queue.empty(); });
                id = m_queue.front();
                m_queue.pop_front();
                const auto queued = m_jobs.find(id);
                if (queued == m_jobs.end())
                    continue; // cancelled while it waited
                Job &job = queued->second;
                job.state = "running";
                m_current = id;
                request = job.request;
                pending = job.pending;
                if (!pending.empty())
                    job.stage = "models";
                say(statusOf(id, job));
            }
            const json reply = attempt([&] {
                if (!pending.empty()) { // hashing a big file takes a while: not in the request
                    const auto cancelled = [this, id] {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        const auto found = m_jobs.find(id);
                        return found == m_jobs.end() || found->second.cancel;
                    };
                    for (const PendingModel &model : pending) {
                        const std::string path = m_models->resolve(model.key, model.handle, cancelled);
                        if (model.key == "tokenizer")
                            if (const std::string why = fugue_helper::tokenizerPathError(path); !why.empty())
                                fail("input_invalid", why);
                        request[json::json_pointer(model.pointer)] = path;
                        json entry = model.handle;
                        entry["key"] = model.key;
                        entry["path"] = path;
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_jobs[id].resolved.push_back(entry);
                    }
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_jobs[id].stage = request.value("op", "");
                }
                Hooks hooks;
                hooks.running = [this](sd_ctx_t *ctx) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (ctx && m_jobs[m_current].cancel) // asked while the models loaded
                        return false;
                    m_running = ctx;
                    return true;
                };
                hooks.runningDgl = [this](sd_dgl_ctx_t *ctx) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (ctx && m_jobs[m_current].cancel)
                        return false;
                    m_runningDgl = ctx;
                    return true;
                };
                hooks.cancelled = [this] {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    return m_jobs[m_current].cancel;
                };
                hooks.progress = [this](double done) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_progress[m_current] = done;
                    say(statusOf(m_current, m_jobs[m_current]));
                };
                hooks.preview = [this](const std::string &file) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    json status = statusOf(m_current, m_jobs[m_current]);
                    status["preview"] = file;
                    say(status);
                };
                return run(request, contexts, hooks);
            });
            std::lock_guard<std::mutex> lock(m_mutex);
            Job &job = m_jobs[id];
            m_running = nullptr;
            m_current.clear();
            if (job.cancel) {
                job.state = "cancelled";
            } else if (reply.contains("error")) {
                job.state = "error";
                job.error = reply["error"];
            } else {
                job.state = "done";
                for (const json &output : reply["outputs"])
                    for (const auto &[name, path] : output.items()) {
                        json file{{"path", path}};
                        if (m_models) { // fetched over HTTP: the file's index, kind and size
                            std::error_code ignored;
                            file = {{"file", job.files.size()},
                                    {"format", name == "image" ? "png" : name == "video" ? "y4m" : name},
                                    {"size", fs::file_size(path.get<std::string>(), ignored)}};
                            job.files.push_back(path.get<std::string>());
                        }
                        if (!job.outputs.contains(name))
                            job.outputs[name] = file;
                        else if (job.outputs[name].is_array())
                            job.outputs[name].push_back(file);
                        else
                            job.outputs[name] = json::array({job.outputs[name], file});
                    }
            }
            if (m_models) {
                job.finishedSeq = ++m_finished;
                job.finishedAt = std::chrono::steady_clock::now();
                purge();
            }
            say(statusOf(id, job));
            if (job.state != "done" && !m_models) { // Fugue asks nothing more of it (over HTTP it polls, and purge() ends it)
                m_progress.erase(id);
                m_jobs.erase(id);
            }
        }
    }

    // sd.cpp's progress callback, on the worker thread: one status line per step.
    static void progressed(int step, int steps, float, void *data) {
        auto *self = static_cast<Server *>(data);
        std::lock_guard<std::mutex> lock(self->m_mutex);
        if (self->m_current.empty() || steps <= 0)
            return;
        if (self->m_jobs[self->m_current].request.contains("frames")) // the frames are the progress
            return;
        if (g_restoreProgress) { // a restore's tiles are one span of its own bar, never the whole of it
            g_restoreProgress->tiles(step, steps);
            self->m_progress[self->m_current] = g_restoreProgress->value();
            self->say(self->statusOf(self->m_current, self->m_jobs[self->m_current]));
            return;
        }
        self->m_progress[self->m_current] = double(step) / steps;
        self->say(self->statusOf(self->m_current, self->m_jobs[self->m_current]));
    }

    std::mutex m_out; // stdout, written by both threads
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<std::string, Job> m_jobs;
    std::map<std::string, double> m_progress;
    std::deque<std::string> m_queue;
    std::string m_current;         // the running job
    sd_ctx_t *m_running = nullptr; // its context while it generates, for cancel
    sd_dgl_ctx_t *m_runningDgl = nullptr; // a restore's, the same
    int m_count = 0;
    uint64_t m_finished = 0;
    Models *m_models = nullptr; // set over HTTP
    fs::path m_base;
};

void logToStderr(enum sd_log_level_t level, const char *text, void *) {
    rememberEngineLog(level, text);
    std::fputs(text, stderr);
}

} // namespace

// --serve-http: reads the key and the model roots, then serves until killed.
int serveHttp(int argc, char **argv) {
    std::string address, keyFile, models;
    int port = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) {
            std::fprintf(stderr, "fugue-helper: %s needs a value\n", flag.c_str());
            return 2;
        }
        const std::string value = argv[++i];
        if (flag == "--listen")
            address = value;
        else if (flag == "--port")
            port = std::atoi(value.c_str());
        else if (flag == "--key-file")
            keyFile = value;
        else if (flag == "--models")
            models = value;
        else {
            std::fprintf(stderr, "fugue-helper: unknown option %s\n", flag.c_str());
            return 2;
        }
    }
    if (address.empty() || port <= 0 || port > 65535 || keyFile.empty() || models.empty()) {
        std::fprintf(stderr, "usage: fugue-helper --serve-http --listen ADDR --port N --key-file FILE --models DIR[:DIR...]\n");
        return 2;
    }
    struct stat info {};
    if (stat(keyFile.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        std::fprintf(stderr, "fugue-helper: cannot read the key file %s\n", keyFile.c_str());
        return 1;
    }
    if (info.st_mode & (S_IRWXG | S_IRWXO)) {
        std::fprintf(stderr, "fugue-helper: the key file %s is readable by others; chmod 600 it\n", keyFile.c_str());
        return 1;
    }
    std::string key;
    {
        std::ifstream in(keyFile);
        std::getline(in, key);
    }
    const size_t first = key.find_first_not_of(" \t\r\n");
    key = first == std::string::npos ? "" : key.substr(first, key.find_last_not_of(" \t\r\n") - first + 1);
    if (key.empty()) {
        std::fprintf(stderr, "fugue-helper: the key file %s is empty\n", keyFile.c_str());
        return 1;
    }
    std::vector<fs::path> roots;
    for (size_t start = 0; start <= models.size();) {
        size_t end = models.find(':', start);
        if (end == std::string::npos)
            end = models.size();
        if (end > start)
            roots.emplace_back(models.substr(start, end - start));
        start = end + 1;
    }
    if (roots.empty()) {
        std::fprintf(stderr, "fugue-helper: --models names no folder\n");
        return 2;
    }
    // Work dirs live in one private folder of this process, removed on SIGINT and SIGTERM.
    std::error_code ec;
    std::string base = (fs::temp_directory_path(ec) / "fugue-helper-XXXXXX").string();
    if (!mkdtemp(base.data())) {
        std::fprintf(stderr, "fugue-helper: cannot make a work folder under the temp folder\n");
        return 1;
    }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::thread([signals, base]() mutable {
        int received = 0;
        sigwait(&signals, &received);
        std::error_code ignored;
        fs::remove_all(base, ignored);
        std::_Exit(0);
    }).detach();
    // The hashes of the model files outlive the process (the work folder does not): $XDG_STATE_HOME or ~/.local/state.
    fs::path state;
    if (const char *xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg)
        state = xdg;
    else if (const char *home = std::getenv("HOME"); home && *home)
        state = fs::path(home) / ".local" / "state";
    Models index(roots, state.empty() ? fs::path() : state / "fugue" / "model-hashes.json");
    Server server(&index, base);
    return server.serveHttp(address, port, key);
}

// --inspect FILE...: one JSON line per file, from the header alone (no backend is made).
static int inspectFiles(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        char *text = sd_fugue_inspect_model(argv[i]);
        json line = text ? json::parse(text, nullptr, false) : json();
        sd_fugue_string_free(text);
        if (!line.is_object())
            line = {{"error", "no answer from the engine"}};
        line["path"] = argv[i];
        // A path need not be UTF-8: replaced bytes, not an exception. Each answer is flushed as it comes, so a crash
        // on a later file keeps the earlier ones.
        std::printf("%s\n", line.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        std::fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && std::string(argv[1]) == "--inspect") {
        sd_set_log_callback(logToStderr, nullptr);
        return inspectFiles(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--serve-http") {
        sd_set_log_callback(logToStderr, nullptr);
        return serveHttp(argc, argv);
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: fugue-helper <request.json> | --inspect FILE... | --serve | --serve-http --listen ADDR --port N --key-file FILE --models DIR[:DIR...]\n");
        return 2;
    }
    sd_set_log_callback(logToStderr, nullptr);
    if (std::string(argv[1]) == "--serve")
        return Server().serve();
    json reply;
    {
        Contexts contexts; // freed before the reply, as one run's models always were
        reply = attempt([&] {
            std::ifstream in(argv[1]);
            if (!in)
                fail("input_invalid", std::string("cannot read ") + argv[1]);
            const json request = json::parse(in, nullptr, false);
            if (request.is_discarded() || !request.is_object())
                fail("input_invalid", std::string(argv[1]) + " is not a JSON object");
            return run(request, contexts);
        });
    }
    std::printf("%s\n", reply.dump().c_str());
    std::fflush(stdout);
    // End without the exit-time teardown (the models are freed already): NVIDIA's Vulkan
    // driver can crash in it, a shader-compiler thread still running at exit (seen with SDXL),
    // which would turn a finished run into a failed one.
    std::_Exit(reply.contains("error") ? 1 : 0);
}
