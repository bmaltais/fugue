// fugue-helper: stages of image generation on stable-diffusion.cpp with Fugue's stage patch
// (helper/sdcpp.patch, spec 8.10).
//
//   fugue-helper <request.json>     one operation, then exit
//   fugue-helper --serve            the provider protocol over stdio (8.8), for HelperProvider
//
// A request names an operation and its files:
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
// "loras" a list of {"path", "multiplier", "is_high_noise"} (the last two optional). Every stage of one generation takes the same
// context and params. A stage loads only the components it runs, except that every call
// VAE-encodes the request's images (init, reference, control) and so loads the VAE.
// The one-shot reply is one JSON line on stdout, {"outputs": [...]} or {"error": {"code",
// "message"}}; the library's log goes to stderr.
//
// --serve reads one JSON line per operation on stdin, each with an "id" its reply echoes:
//
//   {"op": "submit", "capability", "params", "inputs", "output_dir"}  -> {"job"}
//   {"op": "status", "job"}    -> {"state", "progress", "stage", "error"?}
//   {"op": "cancel", "job"}    -> {"state"}
//   {"op": "result", "job"}    -> {"outputs": {NAME: {"path"} or a list of them}, "used"}
//   {"op": "shutdown"}         -> {}, then it exits; so does it when stdin closes
//
// The capability is the operation: text.encode (encode), latent.sample (sample),
// latent.decode (decode), latent.encode (vae_encode). params.context is the request's
// "context" and the other params its "params"; inputs.conditioning, .latent, .init_latent and
// .image ({"path"}) are its input files, and its outputs are written into output_dir as
// conditioning.bin, latent.bin, image.png. Jobs run one at a time, in order. Unasked, it writes
// {"op": "status", "job", "state", "progress", "stage", "error"?} lines as a job moves.
// An error is {"error": {"code", "message"}}; out of memory is "out_of_memory".
//
// Loaded models stay loaded between jobs (one context per set of context options) until their
// memory is needed: before loading, the least recently used contexts are freed while the device
// has less free memory than the new one's files take, and a load or a generation that runs out
// of memory frees the others and is tried once more.

#include <condition_variable>
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
#include <string>
#include <thread>
#include <vector>

#include "ggml-backend.h"
#include "json.hpp"
#include "stable-diffusion.h"

// stb and json.hpp come from stable-diffusion.cpp's thirdparty/, a system include folder here
// (CMakeLists.txt): their warnings are not ours.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

// The loaded contexts, one per set of context options, least recently used first.
class Contexts {
public:
    Contexts() = default;
    Contexts(const Contexts &) = delete;
    Contexts &operator=(const Contexts &) = delete;
    ~Contexts() { clear(); }

    sd_ctx_t *get(const json &options) {
        const std::string key = options.dump();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (it->key == key) {
                m_entries.splice(m_entries.end(), m_entries, it);
                return m_entries.back().ctx;
            }
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
        bool dropped = false;
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
    }

private:
    void dropFirst() {
        sd_ctx_free(m_entries.front().ctx);
        m_entries.pop_front();
    }

    struct Entry {
        std::string key;
        sd_ctx_t *ctx;
    };
    std::list<Entry> m_entries;
};

// `running` is told the context just before the generation (false from it: cancelled, do not
// start) and null after it.
json run(const json &request, Contexts &contexts, const std::function<bool(sd_ctx_t *)> &running = {}) {
    const std::string op = request.value("op", "");
    if (op != "encode" && op != "sample" && op != "decode" && op != "vae_encode" && op != "generate")
        fail("input_invalid", "unknown op \"" + op + "\"; use encode, sample, decode, vae_encode or generate");

    Opt options(sd_img_gen_opt_create());
    if (!options)
        fail("out_of_memory", "cannot create the options");
    const json params = request.value("params", json::object());
    for (const auto &[key, value] : params.items())
        setOption(options.get(), key, value);

    json outputs = json::array();
    auto setPath = [&](const char *key, const std::string &path) {
        check(sd_opt_set_string(options.get(), key, path.c_str()), key);
    };
    std::string imageOut;
    if (op == "encode") {
        const std::string cond = required(request, "conditioning");
        setPath("fugue.cond_out", cond);
        setPath("fugue.stop_after", "cond");
        outputs.push_back({{"conditioning", cond}});
    } else if (op == "sample") {
        const std::string latent = required(request, "latent");
        setPath("fugue.cond_in", required(request, "conditioning"));
        if (request.contains("init_latent"))
            setPath("fugue.init_latent_in", required(request, "init_latent"));
        setPath("fugue.latent_out", latent);
        setPath("fugue.stop_after", "sample");
        outputs.push_back({{"latent", latent}});
    } else if (op == "decode") {
        setPath("fugue.latent_in", required(request, "latent"));
        imageOut = required(request, "image");
    } else if (op == "vae_encode") {
        const std::string latent = required(request, "latent");
        check(sd_opt_set_tensor(options.get(), "init_image", loadImage(required(request, "image")).get()), "init_image");
        setPath("fugue.init_latent_out", latent);
        setPath("fugue.stop_after", "init");
        outputs.push_back({{"latent", latent}});
    } else {
        imageOut = required(request, "image");
    }

    sd_ctx_t *ctx = contexts.get(request.value("context", json::object()));

    sd_result_t *result = nullptr;
    // sd_generate_image clears a cancel asked before it starts, so that one is heard here.
    if (running && !running(ctx))
        fail("cancelled", op + " was cancelled");
    sd_status_t status = sd_generate_image(ctx, options.get(), &result);
    if (status == SD_STATUS_OUT_OF_MEMORY && contexts.dropOthers(ctx))
        status = sd_generate_image(ctx, options.get(), &result);
    if (running)
        running(nullptr);
    if (status == SD_STATUS_CANCELLED)
        fail("cancelled", op + " was cancelled");
    check(status, op);
    std::unique_ptr<sd_result_t, void (*)(sd_result_t *)> resultOwner(result, sd_result_free);
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

// --serve: the provider protocol of spec 8.8 over stdio.
class Server {
public:
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

private:
    struct Job {
        json request; // the one-shot form
        std::string stage;
        std::string state = "queued";
        json outputs = json::object();
        json error;
        bool cancel = false;
    };

    void say(const json &message) {
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
                m_jobs.erase(found); // work() passes over an id it no longer has
                m_progress.erase(id);
                return {{"state", "cancelled"}};
            } else if (job.state == "running") {
                job.cancel = true;
                if (m_running)
                    sd_cancel_generation(m_running, SD_CANCEL_ALL);
            }
            return {{"state", job.state}};
        }
        if (job.state != "done")
            return {{"error", errorJson("input_invalid", "job \"" + id + "\" is " + job.state + ", not done")}};
        // The options it ran with, as it was given them (the library reports no others).
        json used = job.request.value("params", json::object());
        used["engine"] = "sdcpp";
        const json reply{{"outputs", job.outputs}, {"used", used}};
        // Fugue fetches a result once, at once: the job is done with.
        m_progress.erase(id);
        m_jobs.erase(found);
        return reply;
    }

    json submit(const json &message) {
        static const std::map<std::string, std::string> ops{{"text.encode", "encode"},
                                                            {"latent.sample", "sample"},
                                                            {"latent.decode", "decode"},
                                                            {"latent.encode", "vae_encode"}};
        const std::string capability = message.value("capability", "");
        const auto op = ops.find(capability);
        if (op == ops.end())
            return {{"error", errorJson("input_invalid", "no capability \"" + capability + "\"")}};
        json params = message.value("params", json::object());
        json request{{"op", op->second}, {"context", params.value("context", json::object())}};
        params.erase("context");
        request["params"] = params;
        const json inputs = message.value("inputs", json::object());
        for (const char *name : {"conditioning", "latent", "init_latent", "image"})
            if (inputs.contains(name) && inputs[name].is_object() && inputs[name].contains("path"))
                request[name] = inputs[name]["path"];
        const std::filesystem::path dir = message.value("output_dir", "");
        if (dir.empty())
            return {{"error", errorJson("input_invalid", "\"output_dir\" is required")}};
        if (op->second == "encode")
            request["conditioning"] = (dir / "conditioning.bin").string();
        else if (op->second == "sample" || op->second == "vae_encode")
            request["latent"] = (dir / "latent.bin").string();
        else
            request["image"] = (dir / "image.png").string();

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string id = std::to_string(++m_count);
        Job job;
        job.request = request;
        job.stage = op->second;
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
                say(statusOf(id, job));
            }
            const json reply = attempt([&] {
                return run(request, contexts, [this](sd_ctx_t *ctx) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (ctx && m_jobs[m_current].cancel) // asked while the models loaded
                        return false;
                    m_running = ctx;
                    return true;
                });
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
                        const json file{{"path", path}};
                        if (!job.outputs.contains(name))
                            job.outputs[name] = file;
                        else if (job.outputs[name].is_array())
                            job.outputs[name].push_back(file);
                        else
                            job.outputs[name] = json::array({job.outputs[name], file});
                    }
            }
            say(statusOf(id, job));
            if (job.state != "done") { // Fugue asks nothing more of it
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
    int m_count = 0;
};

void logToStderr(enum sd_log_level_t, const char *text, void *) { std::fputs(text, stderr); }

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: fugue-helper <request.json> | --serve\n");
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
