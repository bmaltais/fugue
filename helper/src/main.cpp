// fugue-helper: one stage of image generation per run, on stable-diffusion.cpp with Fugue's
// stage patch (helper/sdcpp.patch, spec 8.10).
//
//   fugue-helper <request.json>
//
// The request names an operation and its files:
//
//   {"op": "encode",     "conditioning": OUT}                  prompts -> conditioning
//   {"op": "sample",     "conditioning": IN, "latent": OUT,    conditioning -> latent; with
//                        "init_latent": IN (optional)}         init_latent, image-to-image from it
//   {"op": "decode",     "latent": IN, "image": OUT}           latent -> PNG
//   {"op": "vae_encode", "image": IN, "latent": OUT}           PNG -> latent
//   {"op": "generate",   "image": OUT}                         all stages in one call
//
// plus "context" (the model files and context options) and "params" (the generation options),
// passed to stable-diffusion.cpp by key as they are (docs/guides/c_api.md at the pin). A value
// is a string, number, bool, list of numbers, {"image": PATH} or a list of them, or for
// "loras" a list of {"path", "multiplier"}. Every stage of one generation takes the same
// context and params: what a stage does not use it does not load.
//
// The reply is one JSON line on stdout, {"outputs": [...]} or {"error": {"code", "message"}};
// the library's log goes to stderr. The persistent helper behind the provider protocol (8.8)
// is #78; this runs one operation and exits.

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"
#include "stable-diffusion.h"

// stb is third-party code: its own warnings are not ours to fix.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Warray-bounds"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

using nlohmann::json;

namespace {

struct Failure {
    std::string code;
    std::string message;
};

[[noreturn]] void fail(const std::string &code, const std::string &message) { throw Failure{code, message}; }

std::string lastError() {
    const char *e = sd_get_last_error();
    return e && *e ? e : "no detail";
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

void saveImage(const sd_tensor_t *image, const std::string &path) {
    if (sd_tensor_type(image) != SD_TENSOR_U8 || sd_tensor_rank(image) != 3)
        fail("helper_failed", "the library returned an image of an unexpected shape");
    const int c = static_cast<int>(sd_tensor_dim(image, 0));
    const int w = static_cast<int>(sd_tensor_dim(image, 1));
    const int h = static_cast<int>(sd_tensor_dim(image, 2));
    if (!stbi_write_png(path.c_str(), w, h, c, sd_tensor_data(image), w * c))
        fail("helper_failed", "cannot write " + path);
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

json run(const json &request) {
    const std::string op = request.value("op", "");
    if (op != "encode" && op != "sample" && op != "decode" && op != "vae_encode" && op != "generate")
        fail("input_invalid", "unknown op \"" + op + "\"");

    Opt image(sd_img_gen_opt_create());
    if (!image)
        fail("out_of_memory", "cannot create the options");
    const json params = request.value("params", json::object());
    for (const auto &[key, value] : params.items())
        setOption(image.get(), key, value);

    json outputs = json::array();
    auto stage = [&](const char *key, const std::string &path) { check(sd_opt_set_string(image.get(), key, path.c_str()), key); };
    std::string imageOut;
    if (op == "encode") {
        const std::string cond = required(request, "conditioning");
        stage("fugue.cond_out", cond);
        stage("fugue.stop_after", "cond");
        outputs.push_back({{"conditioning", cond}});
    } else if (op == "sample") {
        const std::string latent = required(request, "latent");
        stage("fugue.cond_in", required(request, "conditioning"));
        if (request.contains("init_latent"))
            stage("fugue.init_latent_in", required(request, "init_latent"));
        stage("fugue.latent_out", latent);
        stage("fugue.stop_after", "sample");
        outputs.push_back({{"latent", latent}});
    } else if (op == "decode") {
        stage("fugue.latent_in", required(request, "latent"));
        imageOut = required(request, "image");
    } else if (op == "vae_encode") {
        const std::string latent = required(request, "latent");
        check(sd_opt_set_tensor(image.get(), "init_image", loadImage(required(request, "image")).get()), "init_image");
        stage("fugue.init_latent_out", latent);
        stage("fugue.stop_after", "init");
        outputs.push_back({{"latent", latent}});
    } else {
        imageOut = required(request, "image");
    }

    Opt context(sd_ctx_opt_create());
    if (!context)
        fail("out_of_memory", "cannot create the context options");
    const json contextOptions = request.value("context", json::object());
    for (const auto &[key, value] : contextOptions.items())
        setOption(context.get(), key, value);
    sd_ctx_t *ctx = nullptr;
    check(sd_ctx_create(context.get(), &ctx), "loading the models");
    std::unique_ptr<sd_ctx_t, void (*)(sd_ctx_t *)> ctxOwner(ctx, sd_ctx_free);

    sd_result_t *result = nullptr;
    check(sd_generate_image(ctx, image.get(), &result), op);
    std::unique_ptr<sd_result_t, void (*)(sd_result_t *)> resultOwner(result, sd_result_free);
    if (!imageOut.empty()) {
        const size_t count = sd_result_get_image_count(result);
        if (count == 0)
            fail("helper_failed", "no image was made");
        for (size_t i = 0; i < count; ++i) {
            std::string path = imageOut;
            if (count > 1) {
                const size_t dot = path.rfind('.');
                path.insert(dot == std::string::npos ? path.size() : dot, "-" + std::to_string(i));
            }
            saveImage(sd_result_get_image(result, i), path);
            outputs.push_back({{"image", path}});
        }
    }
    return {{"outputs", outputs}};
}

void logToStderr(enum sd_log_level_t, const char *text, void *) { std::fputs(text, stderr); }

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: fugue-helper <request.json>\n");
        return 2;
    }
    sd_set_log_callback(logToStderr, nullptr);
    json reply;
    try {
        std::ifstream in(argv[1]);
        if (!in)
            fail("input_invalid", std::string("cannot read ") + argv[1]);
        const json request = json::parse(in, nullptr, false);
        if (request.is_discarded() || !request.is_object())
            fail("input_invalid", std::string(argv[1]) + " is not a JSON object");
        reply = run(request);
    } catch (const Failure &f) {
        reply = {{"error", {{"code", f.code}, {"message", f.message}}}};
    } catch (const std::exception &e) {
        reply = {{"error", {{"code", "input_invalid"}, {"message", e.what()}}}};
    }
    std::printf("%s\n", reply.dump().c_str());
    return reply.contains("error") ? 1 : 0;
}
