// The component nodes (spec 14.3, 2.5, 2.6, 8.10): Load Models, Apply LoRA, Text Encode,
// Empty Latent, Sampler, VAE Decode and VAE Encode, run by the helper's stage capabilities.
//
// Seams: the kernels each node type registers, driven by hand as the scheduler drives them
// (a NodeKernel's run, a ProviderKernel's request and outputs); and at the bottom the shipped
// `fugue` binary running a graph of them through the fake helper (tests/helper_support).

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <vips/vips8>

#include "engine/ffmpeg.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/media.h"
#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/remote_helper_provider.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"
#include "../engine/fake_http.h"
#include "../engine/provider_support.h"

using namespace fugue;

namespace {

const QString kLoadModels = QStringLiteral("fugue/load-models");
const QString kApplyLora = QStringLiteral("fugue/apply-lora");
const QString kTextEncode = QStringLiteral("fugue/text-encode");
const QString kEmptyLatent = QStringLiteral("fugue/empty-latent");
const QString kSampler = QStringLiteral("fugue/sampler");
const QString kVaeDecode = QStringLiteral("fugue/vae-decode");
const QString kEmptyVideoLatent = QStringLiteral("fugue/empty-video-latent");
const QString kVaeDecodeVideo = QStringLiteral("fugue/vae-decode-video");
const QString kVaeEncode = QStringLiteral("fugue/vae-encode");
const QString kVaeEncodeVideo = QStringLiteral("fugue/vae-encode-video");

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

// A safetensors file's header with these tensor shapes, and no data: what model_shape.h reads.
QByteArray safetensors(const QJsonObject &shapes) {
    QJsonObject header;
    for (auto it = shapes.begin(); it != shapes.end(); ++it)
        header.insert(it.key(), QJsonObject{{"dtype", "F16"}, {"shape", it.value()}, {"data_offsets", QJsonArray{0, 0}}});
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out;
    for (int i = 0; i < 8; ++i) // the header's length, little-endian
        out.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
    return out + json;
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// What the helper's stage capabilities offer (engine/helper_provider.h): one model with an empty
// handle, the model files coming in the job.
Selection helperSelection() {
    Selection chosen;
    chosen.instance = QStringLiteral("Fugue helper");
    chosen.engine = QStringLiteral("sdcpp");
    chosen.model.status = ModelStatus::Ready;
    return chosen;
}

// The same on a paired host (8.10): it reads none of this machine's files.
Selection remoteSelection() {
    Selection chosen = helperSelection();
    chosen.instance = QStringLiteral("gx10");
    chosen.sharesFilesystem = false;
    return chosen;
}

class Bench {
public:
    Bench() : hashes(dir.filePath("hashes.json")) {
        nodes::addBuiltinNodeTypes(registry);
        nodes::addBuiltinKernels(kernels);
        QDir().mkpath(workDir());
    }

    // A model file in a folder of Fugue's models folder (the test's own data folder), and its
    // handle.
    QJsonObject model(const QString &type, const QString &file, const QByteArray &bytes = {}) {
        const QByteArray content = bytes.isEmpty() ? file.toUtf8() + " weights" : bytes;
        writeFile(modelPath(file), content);
        return QJsonObject{{"resource", type}, {"hash", sha256(content)}, {"name", QFileInfo(file).fileName()}};
    }
    static QString modelPath(const QString &file) { return dataDir() + "/models/" + file; }

    // Z-Image Turbo as the owner's ComfyUI folder has it.
    QJsonObject zImage() {
        return QJsonObject{{"diffusion_model", model("fugue/diffusion-model", "diffusion_models/z-image-turbo.gguf")},
                           {"text_encoder", model("fugue/text-encoder", "clip/Qwen3-4B.gguf")},
                           {"vae", model("fugue/vae", "vae/ae.safetensors")}};
    }
    // Wan 2.2 14B as the owner's ComfyUI folder has it: a low- and a high-noise diffusion model, UMT5
    // (a T5, which the header says: the file's name does not) and Wan's VAE.
    QJsonObject wan() {
        const QByteArray umt5 = safetensors({{"shared.weight", QJsonArray{256384, 4096}},
                                             {"encoder.block.0.layer.0.SelfAttention.q.weight", QJsonArray{4096, 4096}}});
        return QJsonObject{{"diffusion_model", model("fugue/diffusion-model", "unet/Wan2.2-T2V-A14B-LowNoise-Q4_K_S.gguf")},
                           {"high_noise_diffusion_model", model("fugue/diffusion-model", "unet/Wan2.2-T2V-A14B-HighNoise-Q4_K_S.gguf")},
                           {"text_encoder", model("fugue/text-encoder", "text_encoders/umt5_xxl_fp8_e4m3fn_scaled.safetensors", umt5)},
                           {"vae", model("fugue/vae", "vae/wan_2.1_vae.safetensors")}};
    }
    // The model Load Models makes of them.
    QJsonObject loaded(const QJsonObject &parts) {
        Values inputs;
        for (auto it = parts.begin(); it != parts.end(); ++it)
            inputs.insert(it.key(), Value(it.value()));
        const Result<Values> out = run(kLoadModels, inputs);
        if (!out)
            qFatal("Load Models failed: %s", qPrintable(out.error().message));
        return out->value("model").json().toObject();
    }

    QString workDir() const { return dir.filePath("work"); }

    const NodeType &type(const QString &id) const { return *registry.nodeType(id); }

    // The scheduler fills defaults in before a kernel sees the inputs (node_kernel.h).
    NodeContext context(const QString &id, Values inputs) {
        for (const InputSpec &spec : type(id).inputs)
            if (!inputs.contains(spec.name) && !spec.defaultValue.isUndefined())
                inputs.insert(spec.name, Value(spec.defaultValue));
        NodeContext context;
        context.node = QStringLiteral("n");
        context.run = QStringLiteral("20261120-120000-000-test");
        context.type = &type(id);
        context.inputs = inputs;
        context.workDir = workDir();
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        context.hashes = &hashes;
        context.registry = &registry;
        return context;
    }

    Result<Values> run(const QString &id, const Values &inputs) { return kernels.node(id)->run(context(id, inputs)); }

    Result<JobRequest> request(const QString &id, const Values &inputs) {
        return kernels.provider(id)->request(context(id, inputs), helperSelection());
    }

    Result<JobRequest> remoteRequest(const QString &id, const Values &inputs) {
        return kernels.provider(id)->request(context(id, inputs), remoteSelection());
    }

    Result<Values> outputs(const QString &id, const Values &inputs, const JobResult &result) {
        return kernels.provider(id)->outputs(context(id, inputs), helperSelection(), result);
    }

    QTemporaryDir dir;
    FileHashes hashes;
    Registry registry;
    Kernels kernels;
};

// A picture of this size and format in the test's folder, probed as the scheduler hands it over.
MediaHandle picture(Bench &bench, const QString &name, int width, int height, int bands = 3) {
    initVips();
    const QString path = bench.dir.filePath(name);
    (vips::VImage::black(width, height, vips::VImage::option()->set("bands", bands)) + 90)
        .cast(VIPS_FORMAT_UCHAR)
        .write_to_file(path.toUtf8().constData());
    const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, path, bench.hashes);
    if (!image)
        qFatal("cannot probe %s", qPrintable(path));
    return image.value();
}

// A test video of this size, length and rate (ffmpeg's test pattern, H.264 in Matroska), probed as a
// Load Video does.
MediaHandle testVideo(Bench &bench, const QString &name, int width, int height, int frames, int fps) {
    const QString path = bench.dir.filePath(name);
    const Result<QByteArray> made = ffmpeg::run(
        QStringLiteral("ffmpeg"),
        {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
         QStringLiteral("testsrc2=size=%1x%2:rate=%3:duration=%4").arg(width).arg(height).arg(fps).arg(double(frames) / fps, 0, 'f', 4),
         "-frames:v", QString::number(frames), "-c:v", "libx264", "-pix_fmt", "yuv420p", "-f", "matroska", path},
        QStringLiteral("node_failed"));
    if (!made)
        qFatal("ffmpeg: %s", qPrintable(made.error().message));
    const Result<MediaHandle> video = MediaHandle::probe(MediaType::Video, path, bench.hashes);
    if (!video)
        qFatal("cannot probe %s", qPrintable(path));
    return video.value();
}

// A one-channel mask of this size with the rectangle `painted` white (255), probed as a picture.
MediaHandle maskPicture(Bench &bench, const QString &name, int width, int height, const QRect &painted, double gray = 255.0) {
    initVips();
    const QString path = bench.dir.filePath(name);
    vips::VImage mask = vips::VImage::black(width, height).copy_memory();
    if (!painted.isEmpty())
        mask.draw_rect(std::vector<double>{gray}, painted.left(), painted.top(), painted.width(), painted.height(),
                       vips::VImage::option()->set("fill", true));
    mask.write_to_file(path.toUtf8().constData());
    const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, path, bench.hashes);
    if (!image)
        qFatal("cannot probe %s", qPrintable(path));
    return image.value();
}

} // namespace

class TestComponentNodes : public QObject {
    Q_OBJECT

private slots:
    // --- the types (2.2, 2.6, 8.9) ---

    void theModelTypesAreFoundInComfyUIsFolders() {
        Bench bench;
        const ModelLibrary library({QStringLiteral("/models")}, &bench.registry);
        const auto folders = [&](const char *type) { return library.folders(*PortType::parse(QString::fromLatin1(type))); };
        QCOMPARE(folders("fugue/diffusion-model"),
                 (QStringList{"/models/diffusion-model", "/models/diffusion_models", "/models/unet"}));
        QCOMPARE(folders("fugue/text-encoder"),
                 (QStringList{"/models/text-encoder", "/models/text_encoders", "/models/clip"}));
        QCOMPARE(folders("fugue/vae"), QStringList{"/models/vae"});
        QCOMPARE(folders("fugue/lora"), (QStringList{"/models/lora", "/models/loras"}));
        QCOMPARE(folders("fugue/model"), QStringList{"/models/checkpoints"});
        QCOMPARE(bench.registry.portKind(*PortType::parse("fugue/latent")), PortKind::Data);
        QCOMPARE(bench.registry.portKind(*PortType::parse("fugue/conditioning")), PortKind::Data);
    }

    // --- Load Models and Apply LoRA: handles only, nothing is loaded (2.5) ---

    void loadModelsMakesOneModelOfTheThreeFiles() {
        Bench bench;
        const QJsonObject parts = bench.zImage();
        const QJsonObject model = bench.loaded(parts);
        QCOMPARE(model.value("resource").toString(), QStringLiteral("fugue/model"));
        const QJsonObject derived = model.value("derived").toObject();
        QCOMPARE(derived.value("base").toObject(), parts.value("diffusion_model").toObject());
        QCOMPARE(derived.value("text_encoder").toObject(), parts.value("text_encoder").toObject());
        QCOMPARE(derived.value("vae").toObject(), parts.value("vae").toObject());
        QVERIFY(!derived.contains("apply"));
    }

    void applyLoraStacksStepsOnTheModel() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const QJsonObject detail = bench.model("fugue/lora", "loras/detail.safetensors");
        const QJsonObject ink = bench.model("fugue/lora", "loras/style/ink.safetensors");
        const Result<Values> once =
            bench.run(kApplyLora, {{"model", Value(model)}, {"lora", Value(detail)}, {"strength", Value(0.8)}});
        QVERIFY(once);
        const Result<Values> twice =
            bench.run(kApplyLora, {{"model", once->value("model")}, {"lora", Value(ink)}});
        QVERIFY(twice);
        const QJsonObject derived = twice->value("model").json().toObject().value("derived").toObject();
        QCOMPARE(derived.value("base"), model.value("derived").toObject().value("base"));
        QCOMPARE(derived.value("vae"), model.value("derived").toObject().value("vae"));
        const QJsonArray apply = derived.value("apply").toArray();
        QCOMPARE(apply.size(), 2);
        QCOMPARE(apply.at(0).toObject(), (QJsonObject{{"lora", detail}, {"strength", 0.8}}));
        QCOMPARE(apply.at(1).toObject(), (QJsonObject{{"lora", ink}, {"strength", 1}})); // the default
    }

    // --- stages on a paired host (8.10): handles, not paths; the host checks ---

    void aHostThatDoesNotShareTheFilesystemGetsHandlesNotPaths() {
        Bench bench;
        const QJsonObject parts = bench.zImage();
        const QJsonObject lora = bench.model("fugue/lora", "loras/detail.safetensors");
        const Result<Values> withLora = bench.run(
            kApplyLora, {{"model", Value(bench.loaded(parts))}, {"lora", Value(lora)}, {"strength", Value(0.5)}});
        QVERIFY(withLora);
        const Result<JobRequest> request =
            bench.remoteRequest(kTextEncode, {{"model", withLora->value("model")}, {"prompt", Value("a fox")}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        const auto handle = [](const QJsonObject &part, const QString &file) {
            return QJsonObject{{"name", part.value("name")}, {"sha256", part.value("hash")},
                               {"size", double(QFileInfo(Bench::modelPath(file)).size())}};
        };
        QCOMPARE(request->params.value("context").toObject(),
                 (QJsonObject{{"diffusion_model_path", handle(parts.value("diffusion_model").toObject(), "diffusion_models/z-image-turbo.gguf")},
                              {"llm_path", handle(parts.value("text_encoder").toObject(), "clip/Qwen3-4B.gguf")},
                              {"vae_path", handle(parts.value("vae").toObject(), "vae/ae.safetensors")}}));
        QCOMPARE(request->params.value("loras").toArray(),
                 (QJsonArray{QJsonObject{{"path", handle(lora, "loras/detail.safetensors")}, {"multiplier", 0.5}}}));
    }

    void aModelThatIsNotOnThisMachineIsForTheHostToFind() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        QVERIFY(QFile::remove(Bench::modelPath("vae/ae.safetensors")));
        const Result<JobRequest> remote = bench.remoteRequest(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox")}});
        QVERIFY2(remote, remote ? "" : qPrintable(remote.error().message));
        const QJsonObject vae = remote->params.value("context").toObject().value("vae_path").toObject();
        QCOMPARE(vae.value("name").toString(), QStringLiteral("ae.safetensors"));
        QVERIFY(vae.value("sha256").toString().startsWith("sha256:"));
        QVERIFY(!vae.contains("size")); // not here, so not known
        // On this machine it is still model_missing.
        QCOMPARE(bench.request(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox")}}).error().code,
                 QStringLiteral("model_missing"));
    }

    void thePartsAreNotCheckedHereForAHost() {
        Bench bench;
        // A text encoder that does not fit, as in aTextEncoderOrVaeThatDoesNotFitIsModelMismatch.
        const auto file = [&](const QString &type, const QString &path, const QJsonObject &shapes) {
            return bench.model(type, path, safetensors(shapes));
        };
        const QJsonObject parts{
            {"diffusion_model", file("fugue/diffusion-model", "diffusion_models/z.safetensors",
                                     {{"cap_embedder.1.weight", QJsonArray{3840, 2560}}, {"x_embedder.weight", QJsonArray{3840, 64}}})},
            {"text_encoder", file("fugue/text-encoder", "text_encoders/t5.safetensors",
                                  {{"encoder.block.0.layer.0.SelfAttention.q.weight", QJsonArray{4096, 4096}}})},
            {"vae", file("fugue/vae", "vae/flux2-vae.safetensors", {{"decoder.conv_in.weight", QJsonArray{512, 32, 3, 3}}})}};
        const QJsonObject model = bench.loaded(parts);
        const Result<JobRequest> here = bench.request(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox")}});
        QCOMPARE(here.error().code, QStringLiteral("model_mismatch"));
        QVERIFY(bench.remoteRequest(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox")}}));
    }

    void stagesOnTwoMachinesPassTheirFilesThroughTheRunFolder() {
        // Encode on this machine (its conditioning is a file in the local run folder), sample on
        // the host: the Sampler's job names that file, and the provider uploads it.
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const QString conditioning = bench.workDir() + "/conditioning.bin";
        writeFile(conditioning, "CONDITIONING-BYTES");
        const Result<Values> encoded = bench.outputs(
            kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox")}},
            JobResult{QJsonObject{{"conditioning", QJsonObject{{"path", conditioning}, {"format", "conditioning"}}}}, {}});
        QVERIFY(encoded);
        const Result<Values> size = bench.run(kEmptyLatent, {{"width", Value(512)}, {"height", Value(512)}});
        QVERIFY(size);
        const Result<JobRequest> request = bench.remoteRequest(
            kSampler, {{"model", Value(model)}, {"conditioning", encoded->value("conditioning")}, {"latent", size->value("latent")}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));

        fugue::test::FakeHttpServer host;
        QVERIFY(host.listen());
        host.on("POST", "/fugue/v1/submit", [](const fugue::test::HttpRequest &) {
            return fugue::test::HttpResponse{200, R"({"job":"j1"})"};
        });
        ProviderInstance instance;
        instance.name = QStringLiteral("gx10");
        instance.type = QStringLiteral("fugue-helper");
        instance.address = QString::fromLatin1(host.origin());
        instance.key = QStringLiteral("k");
        RemoteHelperProvider provider(instance);
        JobRequest sent = *request;
        sent.outputDir = bench.workDir();
        const Result<QString> job = fugue::test::await<QString>([&](Reply<QString> reply) { provider.submit(sent, reply); });
        QVERIFY(job);
        const QJsonObject body = QJsonDocument::fromJson(host.requestsTo("POST", "/fugue/v1/submit").first().body).object();
        const QJsonObject uploaded = body.value("inputs").toObject().value("conditioning").toObject();
        QCOMPARE(QByteArray::fromBase64(uploaded.value("data").toString().toLatin1()), QByteArray("CONDITIONING-BYTES"));
        QCOMPARE(uploaded.value("format").toString(), QStringLiteral("conditioning"));
        QVERIFY(body.value("params").toObject().value("context").toObject().value("vae_path").toObject().contains("sha256"));
    }

    // --- Text Encode ---

    void textEncodeSendsThePromptsAndEveryFileOfTheModel() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a red fox")},
                                        {"negative_prompt", Value("blurry")}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->capability, QStringLiteral("text.encode"));
        QVERIFY(request->model.isEmpty()); // any ready model: the files go in the job
        QCOMPARE(request->params.value("prompt").toString(), QStringLiteral("a red fox"));
        QCOMPARE(request->params.value("negative_prompt").toString(), QStringLiteral("blurry"));
        // Every component, as stable-diffusion.cpp loads a model (and checks) as one context.
        QCOMPARE(request->params.value("context").toObject(),
                 (QJsonObject{{"diffusion_model_path", Bench::modelPath("diffusion_models/z-image-turbo.gguf")},
                              {"llm_path", Bench::modelPath("clip/Qwen3-4B.gguf")},
                              {"vae_path", Bench::modelPath("vae/ae.safetensors")}}));
        // The negative set is always made: the Sampler's guidance decides whether it is used.
        QVERIFY(request->params.value("sample.guidance.txt_cfg").toDouble() != 1);
        QVERIFY(!request->params.contains("loras"));
    }

    void textEncodeAppliesTheModelsLoras() {
        Bench bench;
        const Result<Values> withLora = bench.run(
            kApplyLora, {{"model", Value(bench.loaded(bench.zImage()))},
                         {"lora", Value(bench.model("fugue/lora", "loras/detail.safetensors"))}, {"strength", Value(0.5)}});
        QVERIFY(withLora);
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", withLora->value("model")}, {"prompt", Value("a fox")}});
        QVERIFY(request);
        QCOMPARE(request->params.value("loras").toArray(),
                 (QJsonArray{QJsonObject{{"path", Bench::modelPath("loras/detail.safetensors")}, {"multiplier", 0.5}}}));
    }

    void aModelFileThatIsNotHereIsModelMissingOnTheModelInput() {
        Bench bench;
        const QJsonObject parts = bench.zImage();
        const QJsonObject model = bench.loaded(parts);
        QVERIFY(QFile::remove(Bench::modelPath("vae/ae.safetensors")));
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox")}});
        QVERIFY(!request);
        QCOMPARE(request.error().code, QStringLiteral("model_missing"));
        QCOMPARE(request.error().input, QStringLiteral("model"));
    }

    // #85: parts whose headers say they do not fit fail before the helper is asked.
    void aTextEncoderOrVaeThatDoesNotFitIsModelMismatch() {
        Bench bench;
        const QJsonObject zImage = bench.model("fugue/diffusion-model", "diffusion_models/z.safetensors",
                                               safetensors({{"cap_embedder.1.weight", QJsonArray{3840, 2560}},
                                                            {"x_embedder.weight", QJsonArray{3840, 64}}}));
        const QJsonObject qwen4b = bench.model("fugue/text-encoder", "text_encoders/qwen_3_4b.safetensors",
                                               safetensors({{"model.embed_tokens.weight", QJsonArray{151936, 2560}}}));
        const QJsonObject qwen8b = bench.model("fugue/text-encoder", "text_encoders/qwen_3_8b.safetensors",
                                               safetensors({{"model.embed_tokens.weight", QJsonArray{151936, 4096}}}));
        const QJsonObject ae = bench.model("fugue/vae", "vae/ae.safetensors",
                                           safetensors({{"decoder.conv_in.weight", QJsonArray{512, 16, 3, 3}}}));
        const QJsonObject flux2vae = bench.model("fugue/vae", "vae/flux2-vae.safetensors",
                                                 safetensors({{"decoder.conv_in.weight", QJsonArray{512, 32, 3, 3}}}));
        const auto encode = [&](const QJsonObject &encoder, const QJsonObject &vae) {
            const QJsonObject model = bench.loaded({{"diffusion_model", zImage}, {"text_encoder", encoder}, {"vae", vae}});
            return bench.request(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox")}});
        };
        QVERIFY(encode(qwen4b, ae));
        const Result<JobRequest> wide = encode(qwen8b, ae);
        QVERIFY(!wide);
        QCOMPARE(wide.error().code, QStringLiteral("model_mismatch"));
        QCOMPARE(wide.error().input, QStringLiteral("model"));
        QCOMPARE(wide.error().message, QStringLiteral("The text encoder qwen_3_8b.safetensors does not fit z.safetensors: "
                                                      "it is 4096 wide, and the model reads 2560."));
        const Result<JobRequest> channels = encode(qwen4b, flux2vae);
        QVERIFY(!channels);
        QCOMPARE(channels.error().message, QStringLiteral("The VAE flux2-vae.safetensors does not fit z.safetensors: "
                                                          "it takes 32-channel latents, and the model makes 16."));
    }

    // #86: a LoRA whose layers are another width than the model's (a Klein 4B LoRA on Klein 9B).
    void aLoraThatDoesNotFitIsModelMismatch() {
        Bench bench;
        const QJsonObject klein9b = bench.model("fugue/diffusion-model", "diffusion_models/klein9b.safetensors",
                                                safetensors({{"txt_in.weight", QJsonArray{4096, 12288}},
                                                             {"img_in.weight", QJsonArray{4096, 128}},
                                                             {"double_blocks.0.img_attn.proj.weight", QJsonArray{4096, 4096}}}));
        const QJsonObject parts{{"diffusion_model", klein9b},
                                {"text_encoder", bench.model("fugue/text-encoder", "text_encoders/qwen_3_8b.safetensors",
                                                             safetensors({{"model.embed_tokens.weight", QJsonArray{151936, 4096}}}))},
                                {"vae", bench.model("fugue/vae", "vae/flux2-vae.safetensors",
                                                    safetensors({{"decoder.conv_in.weight", QJsonArray{512, 32, 3, 3}}}))}};
        const auto lora = [&](const QString &file, int width) {
            return bench.model("fugue/lora", "loras/" + file,
                               safetensors({{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight", QJsonArray{16, width}},
                                            {"diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight", QJsonArray{width, 16}}}));
        };
        const auto encode = [&](const QJsonObject &with) {
            const Result<Values> applied =
                bench.run(kApplyLora, {{"model", Value(bench.loaded(parts))}, {"lora", Value(with)}});
            return bench.request(kTextEncode, {{"model", applied->value("model")}, {"prompt", Value("a fox")}});
        };
        QVERIFY(encode(lora("klein9b-turbo.safetensors", 4096)));
        const Result<JobRequest> wrong = encode(lora("klein4b-rhc.safetensors", 3072));
        QVERIFY(!wrong);
        QCOMPARE(wrong.error().code, QStringLiteral("model_mismatch"));
        QCOMPARE(wrong.error().message, QStringLiteral("The LoRA klein4b-rhc.safetensors does not fit klein9b.safetensors: "
                                                       "it changes a layer 3072 to 3072 wide that is 4096 to 4096 in the model."));
    }

    // ... and the canvas check says so before a run, on the part that does not fit (8.5).
    void fugueValidateWarnsOfPartsThatDoNotFit() {
        Bench bench;
        const auto file = [&](const QString &type, const QString &path, const QJsonObject &shapes) {
            return bench.model(type, path, safetensors(shapes));
        };
        const QJsonObject parts{
            {"diffusion_model", file("fugue/diffusion-model", "diffusion_models/z.safetensors",
                                     {{"cap_embedder.1.weight", QJsonArray{3840, 2560}}, {"x_embedder.weight", QJsonArray{3840, 64}}})},
            {"text_encoder", file("fugue/text-encoder", "text_encoders/qwen_3_4b.safetensors",
                                  {{"model.embed_tokens.weight", QJsonArray{151936, 2560}}})},
            {"vae", file("fugue/vae", "vae/flux2-vae.safetensors", {{"decoder.conv_in.weight", QJsonArray{512, 32, 3, 3}}})}};
        const QString flow = bench.dir.filePath("mismatch.fugue");
        writeFile(flow, QJsonDocument(QJsonObject{{"fugue", 1},
                                                  {"graph", QJsonObject{{"nodes", QJsonObject{
                                                      {"models", QJsonObject{{"type", kLoadModels}, {"version", "1.0.0"}, {"inputs", parts}}},
                                                      {"show", QJsonObject{{"type", "fugue/preview"}, {"version", "1.0.0"},
                                                                           {"inputs", QJsonObject{{"value", QJsonObject{{"from", "models.model"}}}}}}}}}}}})
                            .toJson(QJsonDocument::Compact));
        const QList<QJsonObject> out = fugue(bench, bench.dir.filePath("helper.log"), {"validate", flow, "--json"});
        QVERIFY(!out.isEmpty());
        const QJsonArray warnings = out.first().value("warnings").toArray();
        QVERIFY2(warnings.size() == 1, QJsonDocument(out.first()).toJson().constData());
        const QJsonObject warning = warnings.at(0).toObject();
        QCOMPARE(warning.value("code").toString(), QStringLiteral("model_mismatch"));
        QCOMPARE(warning.value("node").toString(), QStringLiteral("models"));
        QCOMPARE(warning.value("input").toString(), QStringLiteral("vae"));
    }

    void aCheckpointIsOneFile() {
        Bench bench;
        const QJsonObject checkpoint = bench.model("fugue/model", "checkpoints/sd15.safetensors");
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(checkpoint)}, {"prompt", Value("a fox")}});
        QVERIFY(request);
        QCOMPARE(request->params.value("context").toObject(),
                 (QJsonObject{{"model_path", Bench::modelPath("checkpoints/sd15.safetensors")}}));
    }

    void textEncodeGivesTheHelpersConditioningFile() {
        Bench bench;
        const QString file = bench.workDir() + "/conditioning.bin";
        const Result<Values> out = bench.outputs(
            kTextEncode, {{"model", Value(bench.loaded(bench.zImage()))}, {"prompt", Value("a fox")}},
            JobResult{QJsonObject{{"conditioning", QJsonObject{{"path", file}}}}, {}});
        QVERIFY(out);
        QCOMPARE(out->value("conditioning").json().toObject(),
                 (QJsonObject{{"data", "fugue/conditioning"}, {"path", file}}));
        const Result<Values> none = bench.outputs(
            kTextEncode, {{"model", Value(bench.loaded(bench.zImage()))}, {"prompt", Value("a fox")}}, JobResult{});
        QVERIFY(!none);
        QCOMPARE(none.error().code, QStringLiteral("node_failed"));
    }

    // --- Empty Latent and Sampler ---

    void anEmptyLatentIsASizeInAFile() {
        Bench bench;
        const Result<Values> out = bench.run(kEmptyLatent, {{"width", Value(832)}});
        QVERIFY(out);
        const QJsonObject latent = out->value("latent").json().toObject();
        QCOMPARE(latent.value("data").toString(), QStringLiteral("fugue/latent"));
        QCOMPARE(latent.value("meta").toObject(), (QJsonObject{{"width", 832}, {"height", 1024}, {"empty", true}}));
        QVERIFY(QFileInfo(latent.value("path").toString()).absolutePath() == bench.workDir());
        QVERIFY(!readFile(latent.value("path").toString()).isEmpty()); // so the scheduler can hash it
    }

    void theSamplerStartsFromNoiseOfAnEmptyLatentsSize() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const Result<Values> empty = bench.run(kEmptyLatent, {{"width", Value(832)}, {"height", Value(1216)}});
        QVERIFY(empty);
        const QJsonObject conditioning{{"data", "fugue/conditioning"}, {"path", "/runs/r/nodes/encode/conditioning.bin"}};
        const Result<JobRequest> request =
            bench.request(kSampler, {{"model", Value(model)}, {"conditioning", Value(conditioning)},
                                     {"latent", empty->value("latent")}, {"seed", Value(42)}, {"steps", Value(8)},
                                     {"sampler", Value("euler")}, {"scheduler", Value("simple")}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->capability, QStringLiteral("latent.sample"));
        QJsonObject params = request->params;
        QCOMPARE(params.take("context").toObject().value("diffusion_model_path").toString(),
                 Bench::modelPath("diffusion_models/z-image-turbo.gguf"));
        // The options stable-diffusion.cpp names (docs/guides/c_api.md at the pin).
        QCOMPARE(params, (QJsonObject{{"width", 832},
                                      {"height", 1216},
                                      {"seed", 42},
                                      {"sample.steps", 8},
                                      {"sample.guidance.txt_cfg", 1},
                                      {"sample.method", "euler"},
                                      {"sample.scheduler", "simple"}}));
        QCOMPARE(request->inputs, (QJsonObject{{"conditioning", QJsonObject{{"path", conditioning.value("path")}}}}));
    }

    void theSamplerRedrawsFromAGivenLatentAsFarAsDenoiseSays() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const QJsonObject latent{{"data", "fugue/latent"},
                                 {"path", "/runs/r/nodes/encode/latent.bin"},
                                 {"meta", QJsonObject{{"width", 640}, {"height", 480}, {"model", model}}}};
        const Result<JobRequest> request =
            bench.request(kSampler, {{"model", Value(model)},
                                     {"conditioning", Value(QJsonObject{{"data", "fugue/conditioning"}, {"path", "/c.bin"}})},
                                     {"latent", Value(latent)}, {"denoise", Value(0.6)}});
        QVERIFY(request);
        QCOMPARE(request->inputs.value("init_latent").toObject(), (QJsonObject{{"path", "/runs/r/nodes/encode/latent.bin"}}));
        QCOMPARE(request->params.value("strength").toDouble(), 0.6);
        QCOMPARE(request->params.value("width").toInt(), 640);
        QCOMPARE(request->params.value("height").toInt(), 480);
        QVERIFY(!request->params.contains("sample.method")); // unset: the model's own
    }

    void aSampledLatentKnowsItsSizeAndModel() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const Result<Values> empty = bench.run(kEmptyLatent, {});
        QVERIFY(empty);
        const QString file = bench.workDir() + "/latent.bin";
        const Result<Values> out =
            bench.outputs(kSampler, {{"model", Value(model)}, {"latent", empty->value("latent")}},
                          JobResult{QJsonObject{{"latent", QJsonObject{{"path", file}}}}, {}});
        QVERIFY(out);
        QCOMPARE(out->value("latent").json().toObject(),
                 (QJsonObject{{"data", "fugue/latent"},
                              {"path", file},
                              {"meta", QJsonObject{{"width", 1024}, {"height", 1024}, {"model", model}}}}));
    }


    // --- video (#105): the same stages, a video latent between them ---

    void anEmptyVideoLatentIsASizeALengthAndARate() {
        Bench bench;
        const Result<Values> out = bench.run(kEmptyVideoLatent, {{"frames", Value(81)}});
        QVERIFY(out);
        const QJsonObject latent = out->value("latent").json().toObject();
        QCOMPARE(latent.value("data").toString(), QStringLiteral("fugue/latent"));
        QCOMPARE(latent.value("meta").toObject(),
                 (QJsonObject{{"width", 832}, {"height", 480}, {"frames", 81}, {"fps", 16}, {"empty", true}}));
        QVERIFY(!readFile(latent.value("path").toString()).isEmpty());
        QCOMPARE(QJsonDocument::fromJson(readFile(latent.value("path").toString())).object(),
                 (QJsonObject{{"width", 832}, {"height", 480}, {"frames", 81}, {"fps", 16}})); // so a length has its own hash
    }

    void loadModelsMakesWansTwoDiffusionModelsAndItsUmt5OneModel() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const QJsonObject derived = model.value("derived").toObject();
        QCOMPARE(derived.value("high_noise").toObject().value("name").toString(), QStringLiteral("Wan2.2-T2V-A14B-HighNoise-Q4_K_S.gguf"));
        const Result<JobRequest> request = bench.request(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox runs")}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        const QJsonObject context = request->params.value("context").toObject();
        QCOMPARE(context.value("diffusion_model_path").toString(), Bench::modelPath("unet/Wan2.2-T2V-A14B-LowNoise-Q4_K_S.gguf"));
        QCOMPARE(context.value("high_noise_diffusion_model_path").toString(), Bench::modelPath("unet/Wan2.2-T2V-A14B-HighNoise-Q4_K_S.gguf"));
        QCOMPARE(context.value("t5xxl_path").toString(), Bench::modelPath("text_encoders/umt5_xxl_fp8_e4m3fn_scaled.safetensors"));
        QVERIFY(!context.contains("llm_path")); // a T5 is not an LLM
        QCOMPARE(context.value("vae_path").toString(), Bench::modelPath("vae/wan_2.1_vae.safetensors"));
    }

    // FLUX.1 Fill (#241): T5 as t5xxl_path and CLIP-L as clip_l_path.
    QJsonObject flux1(Bench &bench, const QString &clip) {
        const QByteArray t5 = safetensors({{"encoder.embed_tokens.weight", QJsonArray{32128, 4096}},
                                           {"encoder.block.0.layer.0.SelfAttention.q.weight", QJsonArray{4096, 4096}}});
        const QByteArray flux = safetensors({{"txt_in.weight", QJsonArray{3072, 4096}},
                                             {"vector_in.in_layer.weight", QJsonArray{3072, 768}},
                                             {"img_in.weight", QJsonArray{3072, 384}}});
        QJsonObject parts{{"diffusion_model", bench.model("fugue/diffusion-model", "diffusion_models/flux1-fill.safetensors", flux)},
                          {"text_encoder", bench.model("fugue/text-encoder", "text_encoders/t5xxl.safetensors", t5)},
                          {"vae", bench.model("fugue/vae", "vae/ae.safetensors",
                                              safetensors({{"decoder.conv_in.weight", QJsonArray{512, 16, 3, 3}}}))}};
        if (!clip.isEmpty())
            parts.insert("clip_l", bench.model("fugue/text-encoder", "text_encoders/" + clip + ".safetensors",
                                               safetensors({{"text_model.embeddings.token_embedding.weight",
                                                             QJsonArray{49408, clip == "clip_g" ? 1280 : 768}}})));
        return bench.loaded(parts);
    }

    void loadModelsTakesFlux1sT5AndClipL() {
        Bench bench;
        const QJsonObject model = flux1(bench, "clip_l");
        const Result<JobRequest> request = bench.request(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox")}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        const QJsonObject context = request->params.value("context").toObject();
        QCOMPARE(context.value("t5xxl_path").toString(), Bench::modelPath("text_encoders/t5xxl.safetensors"));
        QCOMPARE(context.value("clip_l_path").toString(), Bench::modelPath("text_encoders/clip_l.safetensors"));
        QVERIFY(!context.contains("llm_path"));
    }

    void flux1WithoutClipLOrWithAWideOneIsModelMismatchOnTheClipInput() {
        Bench bench;
        const Result<JobRequest> none = bench.request(kTextEncode, {{"model", Value(flux1(bench, {}))}, {"prompt", Value("a fox")}});
        QVERIFY(!none);
        QCOMPARE(none.error().code, QStringLiteral("model_mismatch"));
        QCOMPARE(none.error().input, QStringLiteral("model"));
        QVERIFY(none.error().message.contains("CLIP-L"));
        const Result<JobRequest> wide = bench.request(kTextEncode, {{"model", Value(flux1(bench, "clip_g"))}, {"prompt", Value("a fox")}});
        QVERIFY(!wide);
        QVERIFY(wide.error().message.contains("1280 wide"));
    }

    void aT5ForAnLlmModelSaysWhatTheModelReadsNotThatLoadModelsTakesIt() {
        Bench bench;
        const QJsonObject parts{
            {"diffusion_model", bench.model("fugue/diffusion-model", "diffusion_models/z.safetensors",
                                            safetensors({{"cap_embedder.1.weight", QJsonArray{3840, 2560}},
                                                         {"x_embedder.weight", QJsonArray{3840, 64}}}))},
            {"text_encoder", bench.model("fugue/text-encoder", "text_encoders/t5.safetensors",
                                         safetensors({{"encoder.embed_tokens.weight", QJsonArray{32128, 4096}},
                                                      {"encoder.block.0.layer.0.SelfAttention.q.weight", QJsonArray{4096, 4096}}}))},
            {"vae", bench.model("fugue/vae", "vae/flux2-vae.safetensors", safetensors({{"decoder.conv_in.weight", QJsonArray{512, 32, 3, 3}}}))}};
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(bench.loaded(parts))}, {"prompt", Value("a fox")}});
        QVERIFY(!request);
        QVERIFY2(!request.error().message.contains("takes a language model or a T5"), qPrintable(request.error().message));
        QVERIFY2(request.error().message.contains("the model reads a language model"), qPrintable(request.error().message));
    }

    void aPairedHostGetsWansT5AsT5Too() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const Result<JobRequest> request = bench.remoteRequest(kTextEncode, {{"model", Value(model)}, {"prompt", Value("a fox runs")}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        const QJsonObject context = request->params.value("context").toObject();
        QVERIFY(context.value("t5xxl_path").toObject().contains("sha256"));
        QVERIFY(context.value("high_noise_diffusion_model_path").toObject().contains("sha256"));
        QVERIFY(!context.contains("llm_path"));
    }

    void theSamplerSendsAVideosLengthAndRate() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const Result<Values> empty = bench.run(kEmptyVideoLatent, {{"frames", Value(49)}, {"fps", Value(24)}});
        QVERIFY(empty);
        const QJsonObject conditioning{{"data", "fugue/conditioning"}, {"path", "/runs/r/nodes/encode/conditioning.bin"}};
        const Result<JobRequest> request =
            bench.request(kSampler, {{"model", Value(model)}, {"conditioning", Value(conditioning)}, {"latent", empty->value("latent")},
                                     {"steps", Value(20)}, {"guidance", Value(4)}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->capability, QStringLiteral("latent.sample"));
        QCOMPARE(request->params.value("video_frames").toInt(), 49);
        QCOMPARE(request->params.value("fps").toInt(), 24);
        QCOMPARE(request->params.value("width").toInt(), 832);
        QVERIFY(!request->params.contains("strength")); // from noise
        QVERIFY(!request->inputs.contains("init_latent"));
    }

    void aSampledVideoLatentKeepsItsLengthAndRate() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const Result<Values> empty = bench.run(kEmptyVideoLatent, {{"frames", Value(49)}, {"fps", Value(24)}});
        QVERIFY(empty);
        const QString file = bench.workDir() + "/latent.bin";
        const Result<Values> out = bench.outputs(kSampler, {{"model", Value(model)}, {"latent", empty->value("latent")}},
                                                 JobResult{QJsonObject{{"latent", QJsonObject{{"path", file}}}}, {}});
        QVERIFY(out);
        QCOMPARE(out->value("latent").json().toObject().value("meta").toObject(),
                 (QJsonObject{{"width", 832}, {"height", 480}, {"frames", 49}, {"fps", 24}, {"model", model}}));
    }

    // Video to video (#130): a sampled or encoded clip is a start like a picture is.
    void theSamplerRedrawsFromAVideoLatentAsFarAsDenoiseSays() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const QJsonObject latent{{"data", "fugue/latent"},
                                 {"path", "/runs/r/nodes/encode/latent.bin"},
                                 {"meta", QJsonObject{{"width", 320}, {"height", 192}, {"frames", 33}, {"fps", 16}, {"model", model}}}};
        const Result<JobRequest> request =
            bench.request(kSampler, {{"model", Value(model)},
                                     {"conditioning", Value(QJsonObject{{"data", "fugue/conditioning"}, {"path", "/c.bin"}})},
                                     {"latent", Value(latent)},
                                     {"denoise", Value(0.6)}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->capability, QStringLiteral("latent.sample"));
        QCOMPARE(request->inputs.value("init_latent").toObject(), (QJsonObject{{"path", "/runs/r/nodes/encode/latent.bin"}}));
        QCOMPARE(request->params.value("strength").toDouble(), 0.6);
        QCOMPARE(request->params.value("video_frames").toInt(), 33);
        QCOMPARE(request->params.value("fps").toInt(), 16);
        QCOMPARE(request->params.value("width").toInt(), 320);
    }

    void vaeEncodeVideoFitsTheClipToTheModelsLengthRateAndSize() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        // 50 frames at 25 a second, 2 seconds: at 16 a second about 32 frames, cut to 4 n + 1 (29 to 33).
        const MediaHandle video = testVideo(bench, "in.mkv", 200, 100, 50, 25);
        const Values inputs{{"model", Value(model)}, {"video", Value(video)}, {"frames", Value(81)}};
        const Result<JobRequest> request = bench.request(kVaeEncodeVideo, inputs);
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->capability, QStringLiteral("latent.encode"));
        // The video's own size in steps of 16.
        QCOMPARE(request->params.value("width").toInt(), 208);
        QCOMPARE(request->params.value("height").toInt(), 96);
        const QString clip = bench.workDir() + "/clip.y4m";
        QCOMPARE(request->inputs, (QJsonObject{{"video", QJsonObject{{"path", clip}}}}));
        QVERIFY(request->params.value("context").toObject().contains("high_noise_diffusion_model_path"));
        const QByteArray y4m = readFile(clip);
        QVERIFY(y4m.startsWith("YUV4MPEG2 W208 H96 F16:1"));
        QVERIFY(y4m.contains("C420jpeg"));
        const int made = int(y4m.count("FRAME\n"));
        QVERIFY2(made >= 29 && made <= 33 && (made - 1) % 4 == 0, qPrintable(QString::number(made)));
        const Result<Values> out = bench.outputs(kVaeEncodeVideo, inputs,
                                                 JobResult{QJsonObject{{"latent", QJsonObject{{"path", bench.workDir() + "/latent.bin"}}}}, {}});
        QVERIFY(out);
        QCOMPARE(out->value("latent").json().toObject().value("meta").toObject(),
                 (QJsonObject{{"width", 208}, {"height", 96}, {"frames", made}, {"fps", 16}, {"model", model}}));
    }

    void vaeEncodeVideoTakesTheFramesAskedAndTheSizeGiven() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const MediaHandle video = testVideo(bench, "in.mkv", 160, 160, 64, 16);
        const Values inputs{{"model", Value(model)}, {"video", Value(video)}, {"frames", Value(17)}, {"width", Value(96)}, {"height", Value(64)}};
        const Result<JobRequest> request = bench.request(kVaeEncodeVideo, inputs);
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->params.value("width").toInt(), 96);
        QCOMPARE(request->params.value("height").toInt(), 64);
        const QByteArray y4m = readFile(bench.workDir() + "/clip.y4m");
        QVERIFY(y4m.startsWith("YUV4MPEG2 W96 H64 F16:1"));
        QCOMPARE(y4m.count("FRAME\n"), 17);
    }

    void vaeEncodeVideoRefusesAVideoTooShortForTheModel() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const MediaHandle video = testVideo(bench, "short.mkv", 64, 64, 3, 16);
        const Result<JobRequest> request = bench.request(kVaeEncodeVideo, {{"model", Value(model)}, {"video", Value(video)}});
        QVERIFY(!request);
        QCOMPARE(request.error().input, QStringLiteral("video"));
    }

    void aPairedHostGetsTheClipAsAY4mFile() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const MediaHandle video = testVideo(bench, "in.mkv", 64, 64, 9, 16);
        const Result<JobRequest> request = bench.remoteRequest(kVaeEncodeVideo, {{"model", Value(model)}, {"video", Value(video)}, {"frames", Value(9)}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QVERIFY(request->inputs.value("video").toObject().value("path").toString().endsWith(".y4m"));
    }

    void vaeDecodeVideoSendsTheSameOptionsAsTheOtherStages() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const QJsonObject latent{{"data", "fugue/latent"},
                                 {"path", "/runs/r/nodes/sample/latent.bin"},
                                 {"meta", QJsonObject{{"width", 832}, {"height", 480}, {"frames", 33}, {"fps", 16}, {"model", model}}}};
        const Result<JobRequest> request = bench.request(kVaeDecodeVideo, {{"latent", Value(latent)}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->capability, QStringLiteral("latent.decode"));
        QCOMPARE(request->params.value("video_frames").toInt(), 33);
        QCOMPARE(request->params.value("fps").toInt(), 16);
        QCOMPARE(request->params.value("context").toObject().value("high_noise_diffusion_model_path").toString(),
                 Bench::modelPath("unet/Wan2.2-T2V-A14B-HighNoise-Q4_K_S.gguf"));
        QCOMPARE(request->inputs, (QJsonObject{{"latent", QJsonObject{{"path", "/runs/r/nodes/sample/latent.bin"}}}}));
    }

    void aLatentOfTheOtherKindIsSaidSoBeforeTheHelperDecodesIt() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const QJsonObject video{{"data", "fugue/latent"},
                                {"path", "/l.bin"},
                                {"meta", QJsonObject{{"width", 832}, {"height", 480}, {"frames", 33}, {"fps", 16}, {"model", model}}}};
        const QJsonObject image{{"data", "fugue/latent"},
                                {"path", "/l.bin"},
                                {"meta", QJsonObject{{"width", 832}, {"height", 480}, {"model", model}}}};
        const Result<JobRequest> asImage = bench.request(kVaeDecode, {{"latent", Value(video)}});
        QVERIFY(!asImage);
        QVERIFY(asImage.error().hint.contains("VAE Decode Video"));
        const Result<JobRequest> asVideo = bench.request(kVaeDecodeVideo, {{"latent", Value(image)}});
        QVERIFY(!asVideo);
        QCOMPARE(asVideo.error().input, QStringLiteral("latent"));
    }

    void vaeDecodeVideoMakesAVideoOfTheHelpersFrames() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const QJsonObject latent{{"data", "fugue/latent"},
                                 {"path", "/l.bin"},
                                 {"meta", QJsonObject{{"width", 16}, {"height", 16}, {"frames", 2}, {"fps", 8}, {"model", model}}}};
        QByteArray y4m = "YUV4MPEG2 W16 H16 F8:1 Ip A1:1 C420jpeg\n";
        for (int frame = 0; frame < 3; ++frame)
            y4m += "FRAME\n" + QByteArray(16 * 16 + 2 * 8 * 8, char(100 + frame));
        const QString file = bench.workDir() + "/video.y4m";
        writeFile(file, y4m);
        const Result<Values> out = bench.outputs(kVaeDecodeVideo, {{"latent", Value(latent)}},
                                                 JobResult{QJsonObject{{"video", QJsonObject{{"path", file}, {"format", "y4m"}}}}, {}});
        QVERIFY2(out, out ? "" : qPrintable(out.error().message));
        const MediaHandle clip = out->value("video").media();
        QCOMPARE(clip.type(), MediaType::Video);
        QCOMPARE(clip.width(), 16);
        QCOMPARE(clip.frames(), qint64(3));
        QCOMPARE(clip.fps(), 8.0);
        QVERIFY(clip.path().startsWith(bench.workDir()));
    }

    void aTextToVideoGraphRunsInStagesOnTheHelper() {
        Bench bench;
        const QJsonObject parts = bench.wan();
        const auto node = [](const QString &type, const QJsonObject &inputs) {
            return QJsonObject{{"type", type}, {"version", "1.0.0"}, {"inputs", inputs}};
        };
        const auto from = [](const QString &output) { return QJsonObject{{"from", output}}; };
        const QJsonObject nodes{
            {"models", node(kLoadModels, parts)},
            {"encode", node(kTextEncode, {{"model", from("models.model")}, {"prompt", "a red fox runs in snow"}})},
            {"size", node(kEmptyVideoLatent, {{"width", 480}, {"height", 272}, {"frames", 33}})},
            {"sample", node(kSampler, {{"model", from("models.model")},
                                       {"conditioning", from("encode.conditioning")},
                                       {"latent", from("size.latent")},
                                       {"seed", 1},
                                       {"steps", 4}})},
            {"decode", node(kVaeDecodeVideo, {{"latent", from("sample.latent")}})}};
        const QString flow = bench.dir.filePath("video-flow.fugue");
        writeFile(flow, QJsonDocument(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}})
                            .toJson(QJsonDocument::Compact));
        const QString log = bench.dir.filePath("helper.log");
        const QList<QJsonObject> events = fugue(bench, log, {"run", flow, "--to", "decode", "--wait", "--json"});
        QVERIFY(!events.isEmpty());
        QCOMPARE(events.last().value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(jobs(log), (QStringList{"text.encode", "latent.sample", "latent.decode"}));
        const QJsonObject clip = doneOutputs(events, "decode").value("video").toObject();
        QCOMPARE(clip.value("type").toString(), QStringLiteral("video"));
        QCOMPARE(clip.value("frames").toInt(), 2);
        QVERIFY(clip.value("hash").toString().startsWith("sha256:"));
    }

    // --- VAE Decode and VAE Encode ---

    void vaeDecodeUsesTheModelThatSampledTheLatent() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const QJsonObject latent{{"data", "fugue/latent"},
                                 {"path", "/runs/r/nodes/sample/latent.bin"},
                                 {"meta", QJsonObject{{"width", 1024}, {"height", 768}, {"model", model}}}};
        const Result<JobRequest> request = bench.request(kVaeDecode, {{"latent", Value(latent)}});
        QVERIFY(request);
        QCOMPARE(request->capability, QStringLiteral("latent.decode"));
        QCOMPARE(request->params.value("context").toObject().value("vae_path").toString(),
                 Bench::modelPath("vae/ae.safetensors"));
        QCOMPARE(request->inputs, (QJsonObject{{"latent", QJsonObject{{"path", "/runs/r/nodes/sample/latent.bin"}}}}));
    }

    void anEmptyLatentCannotBeDecoded() {
        Bench bench;
        const Result<Values> empty = bench.run(kEmptyLatent, {});
        QVERIFY(empty);
        const Result<JobRequest> request = bench.request(kVaeDecode, {{"latent", empty->value("latent")}});
        QVERIFY(!request);
        QCOMPARE(request.error().code, QStringLiteral("node_failed"));
        QCOMPARE(request.error().input, QStringLiteral("latent"));
    }

    void vaeEncodeMakesALatentOfTheImagesSize() {
        Bench bench;
        initVips();
        const QString png = bench.dir.filePath("start.png");
        (vips::VImage::black(320, 192, vips::VImage::option()->set("bands", 3)) + 90)
            .cast(VIPS_FORMAT_UCHAR)
            .write_to_file(png.toUtf8().constData());
        const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, png, bench.hashes);
        QVERIFY(image);
        const QJsonObject model = bench.loaded(bench.zImage());
        const Values inputs{{"model", Value(model)}, {"image", Value(image.value())}};
        const Result<JobRequest> request = bench.request(kVaeEncode, inputs);
        QVERIFY(request);
        QCOMPARE(request->capability, QStringLiteral("latent.encode"));
        QCOMPARE(request->params.value("width").toInt(), 320);
        QCOMPARE(request->params.value("height").toInt(), 192);
        QCOMPARE(request->inputs, (QJsonObject{{"image", QJsonObject{{"path", png}}}}));
        const QString file = bench.workDir() + "/latent.bin";
        const Result<Values> out =
            bench.outputs(kVaeEncode, inputs, JobResult{QJsonObject{{"latent", QJsonObject{{"path", file}}}}, {}});
        QVERIFY(out);
        QCOMPARE(out->value("latent").json().toObject().value("meta").toObject(),
                 (QJsonObject{{"width", 320}, {"height", 192}, {"model", model}}));
    }

    // stb_image in the helper reads PNG and JPEG: anything else goes as a PNG, at a size in
    // steps of 16 that the latent's descriptor then tells truly.
    void vaeEncodeSendsOtherFormatsAsAPngAtASizeInStepsOf16() {
        Bench bench;
        initVips();
        const QString webp = bench.dir.filePath("start.webp");
        (vips::VImage::black(330, 200, vips::VImage::option()->set("bands", 4)) + 90)
            .cast(VIPS_FORMAT_UCHAR)
            .write_to_file(webp.toUtf8().constData());
        const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, webp, bench.hashes);
        QVERIFY(image);
        const QJsonObject model = bench.loaded(bench.zImage());
        const Values inputs{{"model", Value(model)}, {"image", Value(image.value())}};
        const Result<JobRequest> request = bench.request(kVaeEncode, inputs);
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        const QString sent = request->inputs.value("image").toObject().value("path").toString();
        QCOMPARE(QFileInfo(sent).absolutePath(), bench.workDir());
        const vips::VImage png = vips::VImage::new_from_file(sent.toUtf8().constData());
        QCOMPARE(png.bands(), 3);
        QCOMPARE(QString::fromLatin1(png.get_string("vips-loader")), QStringLiteral("pngload"));
        QCOMPARE(request->params.value("width").toInt(), 336);
        QCOMPARE(request->params.value("height").toInt(), 208);
        const Result<Values> out = bench.outputs(
            kVaeEncode, inputs, JobResult{QJsonObject{{"latent", QJsonObject{{"path", bench.workDir() + "/latent.bin"}}}}, {}});
        QVERIFY(out);
        QCOMPARE(out->value("latent").json().toObject().value("meta").toObject().value("width").toInt(), 336);
    }

    // --- a mask (#173, spec 14.4): VAE Encode keeps it on the latent, the Sampler sends it ---

    // The mask is not sent to the encode (the helper has no use for it there): the latent carries it
    // with the picture, as handles, and the hash of the latent changes with the mask.
    void vaeEncodeCarriesTheMaskAndThePictureOnTheLatent() {
        Bench bench;
        const MediaHandle image = picture(bench, "start.png", 330, 200);
        const MediaHandle mask = maskPicture(bench, "mask.png", 330, 200, QRect(100, 50, 80, 60));
        const QJsonObject model = bench.loaded(bench.zImage());
        const Values inputs{{"model", Value(model)}, {"image", Value(image)}, {"mask", Value(mask)}};
        const Result<JobRequest> request = bench.request(kVaeEncode, inputs);
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QVERIFY(!request->inputs.contains("mask"));
        const Result<Values> out = bench.outputs(
            kVaeEncode, inputs, JobResult{QJsonObject{{"latent", QJsonObject{{"path", bench.workDir() + "/latent.bin"}}}}, {}});
        QVERIFY(out);
        QVERIFY(out.warnings().isEmpty());
        const QJsonObject meta = out->value("latent").json().toObject().value("meta").toObject();
        QCOMPARE(meta.value("width").toInt(), 336);
        QCOMPARE(meta.value("image").toObject(), image.toJson());
        QCOMPARE(meta.value("mask").toObject(), mask.toJson());
    }

    void aMaskWithNothingPaintedIsMaskEmpty() {
        Bench bench;
        const MediaHandle image = picture(bench, "start.png", 64, 64);
        const Result<JobRequest> request = bench.request(
            kVaeEncode, {{"model", Value(bench.loaded(bench.zImage()))}, {"image", Value(image)},
                         {"mask", Value(maskPicture(bench, "mask.png", 64, 64, QRect()))}});
        QVERIFY(!request);
        QCOMPARE(request.error().code, QStringLiteral("mask_empty"));
        QCOMPARE(request.error().input, QStringLiteral("mask"));
    }

    void aMaskOfAnotherSizeThanThePictureIsRefused() {
        Bench bench;
        const MediaHandle image = picture(bench, "start.png", 64, 64);
        const Result<JobRequest> request = bench.request(
            kVaeEncode, {{"model", Value(bench.loaded(bench.zImage()))}, {"image", Value(image)},
                         {"mask", Value(maskPicture(bench, "mask.png", 32, 64, QRect(0, 0, 8, 8)))}});
        QVERIFY(!request);
        QCOMPARE(request.error().input, QStringLiteral("mask"));
    }

    void aMaskOfAllWhiteIsTheWarningMaskFull() {
        Bench bench;
        const MediaHandle image = picture(bench, "start.png", 64, 64);
        const Values inputs{{"model", Value(bench.loaded(bench.zImage()))}, {"image", Value(image)},
                            {"mask", Value(maskPicture(bench, "mask.png", 64, 64, QRect(0, 0, 64, 64)))}};
        QVERIFY(bench.request(kVaeEncode, inputs));
        const Result<Values> out = bench.outputs(
            kVaeEncode, inputs, JobResult{QJsonObject{{"latent", QJsonObject{{"path", bench.workDir() + "/latent.bin"}}}}, {}});
        QVERIFY(out);
        QCOMPARE(out.warnings().size(), qsizetype(1));
        QCOMPARE(out.warnings().first().code, QStringLiteral("mask_full"));
        QCOMPARE(out.warnings().first().severity, Error::Severity::Warning);
    }

    // The Sampler sends the mask as a gray PNG at the latent's size (the picture's, in steps of 16),
    // scaled by the nearest pixel, and the picture; the start latent and the strength as before.
    void theSamplerSendsTheMaskAndThePictureAtTheLatentsSize() {
        Bench bench;
        const MediaHandle image = picture(bench, "start.png", 330, 200);
        const MediaHandle mask = maskPicture(bench, "mask.png", 330, 200, QRect(0, 0, 165, 200));
        const QJsonObject model = bench.loaded(bench.zImage());
        const Result<Values> encoded = bench.outputs(
            kVaeEncode, {{"model", Value(model)}, {"image", Value(image)}, {"mask", Value(mask)}},
            JobResult{QJsonObject{{"latent", QJsonObject{{"path", bench.workDir() + "/latent.bin"}}}}, {}});
        QVERIFY(encoded);
        const Values sampling{{"model", Value(model)},
                              {"conditioning", Value(QJsonObject{{"data", "fugue/conditioning"}, {"path", "/c.bin"}})},
                              {"latent", encoded->value("latent")}, {"denoise", Value(0.8)}};
        const Result<JobRequest> request = bench.request(kSampler, sampling);
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->params.value("width").toInt(), 336);
        QCOMPARE(request->params.value("strength").toDouble(), 0.8);
        QCOMPARE(request->inputs.value("init_latent").toObject(), (QJsonObject{{"path", bench.workDir() + "/latent.bin"}}));
        const QString sentMask = request->inputs.value("mask").toObject().value("path").toString();
        const vips::VImage gray = vips::VImage::new_from_file(sentMask.toUtf8().constData());
        QCOMPARE(QSize(gray.width(), gray.height()), QSize(336, 208));
        QCOMPARE(gray.bands(), 1);
        QCOMPARE(gray.format(), VIPS_FORMAT_UCHAR);
        QCOMPARE(gray(10, 100)[0], 255.0);
        QCOMPARE(gray(300, 100)[0], 0.0);
        QCOMPARE(gray.min(), 0.0);
        QCOMPARE(gray.max(), 255.0); // nothing between: a hard mask stays hard
        const QString sentImage = request->inputs.value("image").toObject().value("path").toString();
        QCOMPARE(sentImage, image.path()); // the helper scales it to the request's size, as it does the encode's

        // The result is a new picture: it carries no mask.
        const Result<Values> sampled = bench.outputs(
            kSampler, sampling, JobResult{QJsonObject{{"latent", QJsonObject{{"path", bench.workDir() + "/sampled.bin"}}}}, {}});
        QVERIFY(sampled);
        QCOMPARE(sampled->value("latent").json().toObject().value("meta").toObject(),
                 (QJsonObject{{"width", 336}, {"height", 208}, {"model", model}}));

        // The mask is the file VAE Encode read: another one at its path is an error.
        maskPicture(bench, "mask.png", 330, 200, QRect(0, 0, 10, 10));
        const Result<JobRequest> changed = bench.request(kSampler, sampling);
        QVERIFY(!changed);
        QCOMPARE(changed.error().code, QStringLiteral("file_not_found"));
    }

    // Soft mask (#179, spec 14.4): the option goes to the helper only for a latent with a mask, and the
    // mask's gray reaches it as it is (the helper reads it as a strength).
    void theSamplerSendsTheSoftMaskOptionAndKeepsTheGray() {
        Bench bench;
        const MediaHandle image = picture(bench, "start.png", 64, 64);
        const MediaHandle mask = maskPicture(bench, "mask.png", 64, 64, QRect(0, 0, 32, 64), 128.0);
        const QJsonObject model = bench.loaded(bench.zImage());
        const Result<Values> encoded = bench.outputs(
            kVaeEncode, {{"model", Value(model)}, {"image", Value(image)}, {"mask", Value(mask)}},
            JobResult{QJsonObject{{"latent", QJsonObject{{"path", bench.workDir() + "/latent.bin"}}}}, {}});
        QVERIFY(encoded);
        Values sampling{{"model", Value(model)},
                        {"conditioning", Value(QJsonObject{{"data", "fugue/conditioning"}, {"path", "/c.bin"}})},
                        {"latent", encoded->value("latent")}};
        const Result<JobRequest> off = bench.request(kSampler, sampling);
        QVERIFY(off);
        QVERIFY(!off->params.contains("fugue.soft_mask"));
        sampling.insert("soft_mask", Value(true));
        const Result<JobRequest> on = bench.request(kSampler, sampling);
        QVERIFY2(on, on ? "" : qPrintable(on.error().message));
        QCOMPARE(on->params.value("fugue.soft_mask").toBool(), true);
        const vips::VImage gray = vips::VImage::new_from_file(on->inputs.value("mask").toObject().value("path").toString().toUtf8().constData());
        QCOMPARE(gray(10, 10)[0], 128.0);
        QCOMPARE(gray(50, 10)[0], 0.0);

        // No mask on the latent: nothing to read softly, nothing sent.
        const Result<Values> plain = bench.outputs(
            kVaeEncode, {{"model", Value(model)}, {"image", Value(image)}},
            JobResult{QJsonObject{{"latent", QJsonObject{{"path", "/runs/r/latent.bin"}}}}, {}});
        QVERIFY(plain);
        sampling.insert("latent", plain->value("latent"));
        const Result<JobRequest> unmasked = bench.request(kSampler, sampling);
        QVERIFY(unmasked);
        QVERIFY(!unmasked->params.contains("fugue.soft_mask"));
    }

    // Without a mask the Sampler sends what it sent before (the regression guard of #173).
    void theSamplerWithoutAMaskSendsNoMaskAndNoPicture() {
        Bench bench;
        const MediaHandle image = picture(bench, "start.png", 64, 64);
        const QJsonObject model = bench.loaded(bench.zImage());
        const Values encoding{{"model", Value(model)}, {"image", Value(image)}};
        const Result<Values> encoded = bench.outputs(
            kVaeEncode, encoding, JobResult{QJsonObject{{"latent", QJsonObject{{"path", "/runs/r/latent.bin"}}}}, {}});
        QVERIFY(encoded);
        QVERIFY(!encoded->value("latent").json().toObject().value("meta").toObject().contains("mask"));
        QVERIFY(!encoded->value("latent").json().toObject().value("meta").toObject().contains("image"));
        const Result<JobRequest> request =
            bench.request(kSampler, {{"model", Value(model)},
                                     {"conditioning", Value(QJsonObject{{"data", "fugue/conditioning"}, {"path", "/c.bin"}})},
                                     {"latent", encoded->value("latent")}});
        QVERIFY(request);
        QCOMPARE(request->inputs, (QJsonObject{{"conditioning", QJsonObject{{"path", "/c.bin"}}},
                                               {"init_latent", QJsonObject{{"path", "/runs/r/latent.bin"}}}}));
    }

    // --- references (image editing: FLUX.2 Klein, Qwen-Image Edit) ---

    // stable-diffusion.cpp takes the references as the generation option ref_images; each
    // stage of one generation sends the same options, so Text Encode sends them as well.
    void textEncodeSendsItsReferences() {
        Bench bench;
        const MediaHandle png = picture(bench, "first.png", 320, 192);
        const MediaHandle webp = picture(bench, "second.webp", 200, 200, 4);
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(bench.loaded(bench.zImage()))}, {"prompt", Value("make it night")},
                                        {"references", Value::list({Value(png), Value(webp)})}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        const QJsonArray sent = request->params.value("ref_images").toArray();
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent.at(0).toObject(), (QJsonObject{{"image", png.path()}})); // a PNG goes as it is
        const QString converted = sent.at(1).toObject().value("image").toString();
        QCOMPARE(QFileInfo(converted).absolutePath(), bench.workDir()); // anything else as a PNG
        QCOMPARE(QString::fromLatin1(vips::VImage::new_from_file(converted.toUtf8().constData()).get_string("vips-loader")),
                 QStringLiteral("pngload"));
    }

    void noReferencesSendsNone() {
        Bench bench;
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(bench.loaded(bench.zImage()))}, {"prompt", Value("a fox")}});
        QVERIFY(request);
        QVERIFY(!request->params.contains("ref_images"));
    }

    // The references ride the conditioning to the Sampler, as ComfyUI's ReferenceLatent puts
    // them on it; so the conditioning of one prompt with other pictures is another value.
    void theConditioningCarriesItsReferencesToTheSampler() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const MediaHandle first = picture(bench, "first.png", 320, 192);
        const MediaHandle second = picture(bench, "second.jpg", 256, 256);
        const Values inputs{{"model", Value(model)}, {"prompt", Value("put the hat on the fox")},
                            {"references", Value::list({Value(first), Value(second)})}};
        const QString file = bench.workDir() + "/conditioning.bin";
        const Result<Values> out =
            bench.outputs(kTextEncode, inputs, JobResult{QJsonObject{{"conditioning", QJsonObject{{"path", file}}}}, {}});
        QVERIFY(out);
        const QJsonObject conditioning = out->value("conditioning").json().toObject();
        QCOMPARE(conditioning.value("meta").toObject().value("references").toArray(),
                 (QJsonArray{first.toJson(), second.toJson()}));

        const Result<Values> empty = bench.run(kEmptyLatent, {{"width", Value(320)}, {"height", Value(192)}});
        const Result<JobRequest> sample =
            bench.request(kSampler, {{"model", Value(model)}, {"conditioning", Value(conditioning)},
                                     {"latent", empty->value("latent")}, {"seed", Value(1)}, {"steps", Value(4)}});
        QVERIFY2(sample, sample ? "" : qPrintable(sample.error().message));
        QCOMPARE(sample->params.value("ref_images").toArray(),
                 (QJsonArray{QJsonObject{{"image", first.path()}}, QJsonObject{{"image", second.path()}}}));
    }

    // A reference whose file is gone or changed since Text Encode ran (a cached conditioning
    // outlives it) is said as such, not sent to the helper.
    void aReferenceThatChangedSinceTheEncodeIsAnError() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.zImage());
        const MediaHandle first = picture(bench, "first.png", 320, 192);
        const Result<Values> out = bench.outputs(
            kTextEncode, {{"model", Value(model)}, {"prompt", Value("x")}, {"references", Value::list({Value(first)})}},
            JobResult{QJsonObject{{"conditioning", QJsonObject{{"path", bench.workDir() + "/conditioning.bin"}}}}, {}});
        const Result<Values> empty = bench.run(kEmptyLatent, {{"width", Value(320)}, {"height", Value(192)}});
        const Values inputs{{"model", Value(model)}, {"conditioning", out->value("conditioning")}, {"latent", empty->value("latent")}};
        picture(bench, "first.png", 320, 192, 4); // other content at the same path
        const Result<JobRequest> changed = bench.request(kSampler, inputs);
        QVERIFY(!changed);
        QCOMPARE(changed.error().code, QStringLiteral("file_not_found"));
        QCOMPARE(changed.error().input, QStringLiteral("conditioning"));
        QFile::remove(first.path());
        QVERIFY(!bench.request(kSampler, inputs));
    }

    // A JPEG that is turned by its EXIF orientation goes to the helper upright, as a PNG: stb_image
    // does not read the orientation.
    void aTurnedJpegGoesUpright() {
        Bench bench;
        initVips();
        const QString jpeg = bench.dir.filePath("turned.jpg");
        vips::VImage image = (vips::VImage::black(320, 192, vips::VImage::option()->set("bands", 3)) + 90).cast(VIPS_FORMAT_UCHAR);
        image = image.copy();
        image.set("orientation", 6); // turned 90 degrees
        image.jpegsave(jpeg.toUtf8().constData());
        const Result<MediaHandle> handle = MediaHandle::probe(MediaType::Image, jpeg, bench.hashes);
        QVERIFY(handle);
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(bench.loaded(bench.zImage()))}, {"prompt", Value("x")},
                                        {"references", Value::list({Value(handle.value())})}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        const QString sent = request->params.value("ref_images").toArray().first().toObject().value("image").toString();
        QVERIFY(sent != jpeg);
        const vips::VImage upright = vips::VImage::new_from_file(sent.toUtf8().constData());
        QCOMPARE(QSize(upright.width(), upright.height()), QSize(192, 320));
    }

    // --- image to video (#130): a first frame, encoded for the clip's size and length ---

    // The first frame is cut to the clip's size and goes as the encode's image; the clip's own
    // size, length and rate are the generation options, as every stage of the generation sends them.
    void textEncodeSendsAFirstFrameAtTheClipsSize() {
        Bench bench;
        const MediaHandle frame = picture(bench, "start.png", 400, 300);
        const Result<Values> clip = bench.run(kEmptyVideoLatent, {{"width", Value(320)}, {"height", Value(192)},
                                                                  {"frames", Value(33)}, {"fps", Value(16)}});
        QVERIFY(clip);
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(bench.loaded(bench.wan()))}, {"prompt", Value("a fox runs")},
                                        {"first_frame", Value(frame)}, {"clip", clip->value("latent")}});
        QVERIFY2(request, request ? "" : qPrintable(request.error().message));
        QCOMPARE(request->params.value("width").toInt(), 320);
        QCOMPARE(request->params.value("height").toInt(), 192);
        QCOMPARE(request->params.value("video_frames").toInt(), 33);
        QCOMPARE(request->params.value("fps").toInt(), 16);
        const QString sent = request->inputs.value("image").toObject().value("path").toString();
        QCOMPARE(QFileInfo(sent).absolutePath(), bench.workDir());
        const vips::VImage png = vips::VImage::new_from_file(sent.toUtf8().constData());
        QCOMPARE(QSize(png.width(), png.height()), QSize(320, 192));
    }

    void textToVideoEncodesWithoutAFirstFrame() {
        Bench bench;
        const Result<Values> clip = bench.run(kEmptyVideoLatent, {});
        const Result<JobRequest> request =
            bench.request(kTextEncode, {{"model", Value(bench.loaded(bench.wan()))}, {"prompt", Value("a fox runs")},
                                        {"clip", clip->value("latent")}});
        QVERIFY(request);
        QVERIFY(!request->inputs.contains("image"));
        QVERIFY(!request->params.contains("video_frames")); // the clip is the Sampler's and the first frame's
    }

    void aFirstFrameNeedsAVideoClip() {
        Bench bench;
        const MediaHandle frame = picture(bench, "start.png", 320, 192);
        const QJsonObject model = bench.loaded(bench.wan());
        const Result<JobRequest> none =
            bench.request(kTextEncode, {{"model", Value(model)}, {"prompt", Value("x")}, {"first_frame", Value(frame)}});
        QVERIFY(!none);
        QCOMPARE(none.error().input, QStringLiteral("clip"));
        const Result<Values> image = bench.run(kEmptyLatent, {});
        const Result<JobRequest> asImage = bench.request(
            kTextEncode, {{"model", Value(model)}, {"prompt", Value("x")}, {"first_frame", Value(frame)}, {"clip", image->value("latent")}});
        QVERIFY(!asImage);
        QCOMPARE(asImage.error().input, QStringLiteral("clip"));
        QVERIFY(asImage.error().hint.contains("Empty Video Latent"));
    }

    // The frame rides the conditioning to the Sampler (Wan 2.2 TI2V samples from it), which sends the
    // same picture at the same size; a latent of another clip is said so, not sampled.
    void theConditioningCarriesTheFirstFrameToTheSampler() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const MediaHandle frame = picture(bench, "start.png", 400, 300);
        const Result<Values> clip = bench.run(kEmptyVideoLatent, {{"width", Value(320)}, {"height", Value(192)},
                                                                  {"frames", Value(33)}, {"fps", Value(16)}});
        const Values inputs{{"model", Value(model)}, {"prompt", Value("a fox runs")}, {"first_frame", Value(frame)},
                            {"clip", clip->value("latent")}};
        const Result<Values> out = bench.outputs(
            kTextEncode, inputs, JobResult{QJsonObject{{"conditioning", QJsonObject{{"path", bench.workDir() + "/conditioning.bin"}}}}, {}});
        QVERIFY(out);
        const QJsonObject meta = out->value("conditioning").json().toObject().value("meta").toObject();
        QCOMPARE(meta.value("first_frame").toObject(), frame.toJson());
        QCOMPARE(meta.value("clip").toObject().value("frames").toInt(), 33);

        const Values sampling{{"model", Value(model)}, {"conditioning", out->value("conditioning")}, {"latent", clip->value("latent")},
                              {"seed", Value(1)}, {"steps", Value(4)}};
        const Result<JobRequest> sample = bench.request(kSampler, sampling);
        QVERIFY2(sample, sample ? "" : qPrintable(sample.error().message));
        const QString sent = sample->inputs.value("image").toObject().value("path").toString();
        QCOMPARE(QFileInfo(sent).absolutePath(), bench.workDir());
        const vips::VImage png = vips::VImage::new_from_file(sent.toUtf8().constData());
        QCOMPARE(QSize(png.width(), png.height()), QSize(320, 192));

        const Result<Values> other = bench.run(kEmptyVideoLatent, {{"width", Value(320)}, {"height", Value(192)}, {"frames", Value(81)}});
        Values wrong = sampling;
        wrong.insert("latent", other->value("latent"));
        const Result<JobRequest> mismatch = bench.request(kSampler, wrong);
        QVERIFY(!mismatch);
        QCOMPARE(mismatch.error().input, QStringLiteral("latent"));
        QVERIFY(mismatch.error().message.contains("33 frames"));

        picture(bench, "start.png", 400, 300, 4); // another picture at the same path
        const Result<JobRequest> changed = bench.request(kSampler, sampling);
        QVERIFY(!changed);
        QCOMPARE(changed.error().code, QStringLiteral("file_not_found"));
    }

    void aTextToVideoConditioningSendsNoFirstFrame() {
        Bench bench;
        const QJsonObject model = bench.loaded(bench.wan());
        const Result<Values> clip = bench.run(kEmptyVideoLatent, {});
        const Result<Values> out = bench.outputs(
            kTextEncode, {{"model", Value(model)}, {"prompt", Value("x")}},
            JobResult{QJsonObject{{"conditioning", QJsonObject{{"path", bench.workDir() + "/conditioning.bin"}}}}, {}});
        const Result<JobRequest> sample =
            bench.request(kSampler, {{"model", Value(model)}, {"conditioning", out->value("conditioning")},
                                     {"latent", clip->value("latent")}, {"seed", Value(1)}, {"steps", Value(4)}});
        QVERIFY(sample);
        QVERIFY(!sample->inputs.contains("image"));
    }

    // --- end to end: the shipped binary, the fake helper (no inference here) ---

    // Text to image in stages, run by the helper Fugue starts itself (no providers.json: the
    // defaults end with it, 8.2). A new seed samples and decodes again, but the prompt's
    // conditioning comes from the cache (2.6).
    void aTextToImageGraphRunsInStagesOnTheHelper() {
        Bench bench;
        const QJsonObject parts = bench.zImage();
        const auto node = [](const QString &type, const QJsonObject &inputs) {
            return QJsonObject{{"type", type}, {"version", "1.0.0"}, {"inputs", inputs}};
        };
        const auto from = [](const QString &output) { return QJsonObject{{"from", output}}; };
        const QJsonObject nodes{
            {"models", node(kLoadModels, parts)},
            {"encode", node(kTextEncode, {{"model", from("models.model")}, {"prompt", "a red fox in snow"}})},
            {"size", node(kEmptyLatent, {{"width", 512}, {"height", 512}})},
            {"sample", node(kSampler, {{"model", from("models.model")},
                                       {"conditioning", from("encode.conditioning")},
                                       {"latent", from("size.latent")},
                                       {"seed", 1},
                                       {"steps", 4}})},
            {"decode", node(kVaeDecode, {{"latent", from("sample.latent")}})}};
        const QString flow = bench.dir.filePath("flow.fugue");
        writeFile(flow, QJsonDocument(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}})
                            .toJson(QJsonDocument::Compact));
        const QString log = bench.dir.filePath("helper.log");

        const QList<QJsonObject> first = fugue(bench, log, {"run", flow, "--to", "decode", "--wait", "--json"});
        QVERIFY(!first.isEmpty());
        QCOMPARE(first.last().value("event").toString(), QStringLiteral("run.done"));
        QCOMPARE(first.last().value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(jobs(log), (QStringList{"text.encode", "latent.sample", "latent.decode"}));
        const QJsonObject decoded = doneOutputs(first, "decode").value("image").toObject();
        QCOMPARE(decoded.value("width").toInt(), 1);
        QVERIFY(decoded.value("hash").toString().startsWith("sha256:"));

        QFile::remove(log);
        const QList<QJsonObject> again =
            fugue(bench, log, {"run", flow, "--to", "decode", "--set", "sample.seed=2", "--wait", "--json"});
        QVERIFY(!again.isEmpty());
        QCOMPARE(again.last().value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(jobs(log), (QStringList{"latent.sample", "latent.decode"})); // the conditioning was kept
    }

private:
    // The shipped binary with the fake helper as this machine's helper; its events.
    static QList<QJsonObject> fugue(const Bench &bench, const QString &log, const QStringList &args) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *name : {"QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "FUGUE_NODE_MANIFESTS"})
            env.remove(QString::fromLatin1(name));
        // Its own cache and runs (ctest keeps one home per test across runs); the models stay
        // in the data folder Bench wrote them to.
        env.insert("XDG_CACHE_HOME", bench.dir.filePath("cache"));
        env.insert("XDG_STATE_HOME", bench.dir.filePath("state"));
        env.insert("FUGUE_HELPER", QStringLiteral(FUGUE_FAKE_HELPER));
        env.insert("FAKE_HELPER_LOG", log);
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(bench.dir.path()); // inside the write roots (13.6)
        process.start(QStringLiteral(FUGUE_BINARY), args);
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            qWarning("fugue timed out: %s", qPrintable(args.join(' ')));
            return {};
        }
        const QByteArray out = process.readAllStandardOutput();
        if (process.exitCode() != 0)
            qWarning("fugue %s: %s %s", qPrintable(args.join(' ')), out.constData(),
                     process.readAllStandardError().constData());
        QList<QJsonObject> events;
        for (const QByteArray &line : out.trimmed().split('\n'))
            events.append(QJsonDocument::fromJson(line).object());
        return events;
    }

    // The capabilities of the jobs the helper ran, in order.
    static QStringList jobs(const QString &log) {
        QStringList ran;
        for (const QString &line : QString::fromUtf8(readFile(log)).split('\n', Qt::SkipEmptyParts))
            if (line.startsWith(QLatin1String("job ")))
                ran << line.mid(4);
        return ran;
    }

    static QJsonObject doneOutputs(const QList<QJsonObject> &events, const QString &node) {
        for (const QJsonObject &event : events)
            if (event.value("event").toString() == QLatin1String("node.done") && event.value("node").toString() == node)
                return event.value("outputs").toObject();
        return {};
    }
};

QTEST_GUILESS_MAIN(TestComponentNodes)
#include "test_component_nodes.moc"
