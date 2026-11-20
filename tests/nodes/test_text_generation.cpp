// Text Generate and Enhance Prompt (spec 14.2, 8.1): the text nodes, served by a provider's
// text.generate.
//
// Seam: the ProviderKernel each node type registers (requirement, request, outputs), driven
// with a hand-built Selection the way the scheduler drives it. The end-to-end test at the
// bottom runs the shipped `fugue` binary against a fake omarchy text and image server.

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

#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"
#include "../engine/fake_http.h"

using namespace fugue;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

const QString kTextGenerate = QStringLiteral("fugue/text-generate");
const QString kEnhance = QStringLiteral("fugue/enhance-prompt");

QJsonObject handle(const QString &name) { return QJsonObject{{"resource", "fugue/text-model"}, {"name", name}}; }

// What a provider offers for a text model (8.1): what it takes and its longest answer.
Selection selection(const QJsonObject &properties = {}, const QJsonObject &limits = {}) {
    Selection chosen;
    chosen.instance = QStringLiteral("gx10");
    chosen.engine = QStringLiteral("sdcpp");
    chosen.model.handle = handle("qwen3.5-2b");
    chosen.model.status = ModelStatus::Ready;
    chosen.model.properties = properties;
    chosen.model.limits = limits;
    return chosen;
}

QString writePng(const QString &path, int width, int height) {
    initVips();
    const vips::VImage image =
        (vips::VImage::black(width, height, vips::VImage::option()->set("bands", 3)) + 128).cast(VIPS_FORMAT_UCHAR);
    image.write_to_file(path.toUtf8().constData());
    return path;
}

QSize sizeOf(const QString &path) {
    const vips::VImage image = vips::VImage::new_from_file(path.toUtf8().constData());
    return {image.width(), image.height()};
}

QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

class Bench {
public:
    Bench() : hashes(dir.filePath("hashes.json")) {
        nodes::addBuiltinNodeTypes(registry);
        nodes::addTextGenerationKernels(kernels);
        QDir().mkpath(workDir());
    }

    Value image(const QString &path) {
        const Result<MediaHandle> probed = MediaHandle::probe(MediaType::Image, path, hashes);
        if (!probed)
            qFatal("cannot probe %s", qPrintable(path));
        return Value(probed.value());
    }

    QString workDir() const { return dir.filePath("work"); }
    QString file(const QString &name) const { return dir.filePath(name); }
    const NodeType &type(const QString &id) const { return *registry.nodeType(id); }
    std::shared_ptr<ProviderKernel> kernel(const QString &id) const { return kernels.provider(id); }

    // The scheduler fills defaults in before a kernel sees the inputs (node_kernel.h).
    NodeContext context(const QString &id, Values inputs) {
        for (const InputSpec &spec : type(id).inputs)
            if (!inputs.contains(spec.name) && !spec.defaultValue.isUndefined())
                inputs.insert(spec.name, Value(spec.defaultValue));
        NodeContext context;
        context.node = QStringLiteral("t");
        context.run = QStringLiteral("20261120-120000-000-test");
        context.type = &type(id);
        context.inputs = inputs;
        context.workDir = workDir();
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        context.hashes = &hashes;
        return context;
    }

    Result<JobRequest> request(const QString &id, const Values &inputs, const Selection &chosen = selection()) {
        return kernel(id)->request(context(id, inputs), chosen);
    }

    Result<Values> outputs(const QString &id, const QString &text, const QJsonObject &used = {}) {
        return kernel(id)->outputs(context(id, {{"prompt", "x"}}), selection(),
                                   JobResult{QJsonObject{{"text", text}}, used});
    }

    QTemporaryDir dir;
    FileHashes hashes;
    Registry registry;
    Kernels kernels;
};

// This machine with a text model and an image model, as omarchy-local-ai leaves it, served by
// one fake server: the text model streams one answer, the image model makes a picture. The
// shipped `fugue` finds it through $HOME.
class FakeMachine {
public:
    static constexpr const char *kKey = "local-key-0123456789";

    explicit FakeMachine(QByteArray answer) {
        server.listen();
        const QString endpoint = QString::fromLatin1(server.origin()) + "/v1";
        const auto row = [&endpoint](const QString &recipe, const QString &kind) {
            return QJsonObject{{"recipe", recipe}, {"kind", kind}, {"edit", false}, {"name", recipe},
                               {"runtime", kind == "image" ? "sdcpp" : "tensorfold-x86"}, {"unit", "active"},
                               {"endpoint", endpoint}, {"fits", true}, {"fit_reason", ""}, {"error", ""}};
        };
        writeFile(home("/.local/state/omarchy/agent-local/status.json"),
                  compact(QJsonObject{{"models", QJsonObject{{"qwen3.5-2b", row("qwen3.5-2b", "text")},
                                                             {"z-image", row("z-image", "image")}}},
                                      {"last", QJsonValue::Null}}));
        writeFile(home("/.config/omarchy/agent-local/key"), QByteArray(kKey) + "\n");

        server.on("GET", "/v1/models", [](const HttpRequest &) {
            return HttpResponse{200, "{\"object\":\"list\",\"data\":[{\"id\":\"qwen3.5-2b\"}]}"};
        });
        server.on("POST", "/v1/chat/completions", [answer](const HttpRequest &) {
            const QJsonObject chunk{{"choices", QJsonArray{QJsonObject{{"index", 0},
                                                                       {"delta", QJsonObject{{"content", QString::fromUtf8(answer)}}}}}}};
            return HttpResponse{200, "data: " + compact(chunk) + "\n\ndata: [DONE]\n\n", "text/event-stream"};
        });
        server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            return HttpResponse{200, "{\"supported_modes\":[\"img_gen\"],\"limits\":{}}"};
        });
        server.on("POST", "/sdcpp/v1/img_gen", [this](const HttpRequest &) {
            const QByteArray png = [this] {
                QFile file(writePng(m_dir.filePath("made.png"), 64, 64));
                return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
            }();
            const QJsonObject done{
                {"id", "job_1"}, {"kind", "img_gen"}, {"status", "completed"}, {"error", QJsonValue::Null},
                {"result", QJsonObject{{"output_format", "png"},
                                       {"images", QJsonArray{QJsonObject{{"index", 0},
                                                                         {"b64_json", QString::fromLatin1(png.toBase64())},
                                                                         {"generation_info", QJsonObject{}}}}}}}};
            server.on("GET", "/sdcpp/v1/jobs/job_1", [done](const HttpRequest &) { return HttpResponse{200, compact(done)}; });
            return HttpResponse{202, "{\"id\":\"job_1\",\"kind\":\"img_gen\",\"status\":\"queued\"}"};
        });
    }

    QString home(const QString &path = {}) const { return m_dir.filePath("home") + path; }
    QString file(const QString &name) const { return m_dir.filePath(name); }

    QJsonObject sent(const QByteArray &path) const {
        const auto posts = server.requestsTo("POST", path);
        return posts.isEmpty() ? QJsonObject() : QJsonDocument::fromJson(posts.last().body).object();
    }

    // The shipped binary with this machine as its home, waited for with the event loop running
    // so the fake server in this process can answer.
    int fugue(const QStringList &args) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *name : {"QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "FUGUE_NODE_MANIFESTS"})
            env.remove(QString::fromLatin1(name));
        env.insert("HOME", home());
        env.insert("XDG_CACHE_HOME", file("cache"));
        env.insert("XDG_STATE_HOME", file("state"));
        env.insert("XDG_CONFIG_HOME", file("config"));
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(m_dir.path());
        process.start(QStringLiteral(FUGUE_BINARY), args);
        if (!process.waitForStarted())
            return -1;
        QElapsedTimer clock;
        clock.start();
        while (process.state() != QProcess::NotRunning && clock.elapsed() < 60000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
            return -1;
        }
        const int code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        if (code != 0)
            qWarning("fugue %s: %s %s", qPrintable(args.join(' ')), process.readAllStandardOutput().constData(),
                     process.readAllStandardError().constData());
        return code;
    }

    FakeHttpServer server;

private:
    QTemporaryDir m_dir;
};

} // namespace

class TestTextGeneration : public QObject {
    Q_OBJECT

private slots:
    // --- requirement (8.3, 8.4) ---

    void bothNodesAskForTextGenerateAndTheModelInput() {
        Bench bench;
        for (const QString &id : {kTextGenerate, kEnhance}) {
            QVERIFY(bench.kernel(id));
            const Requirement wanted = bench.kernel(id)->requirement(bench.type(id), {{"prompt", "a fox"}, {"model", handle("qwen")}});
            QCOMPARE(wanted.capability, QStringLiteral("text.generate"));
            QCOMPARE(wanted.model, handle("qwen"));
            QVERIFY(wanted.properties.isEmpty());
        }
        // Under validate nothing may be known yet.
        QVERIFY(bench.kernel(kTextGenerate)->requirement(bench.type(kTextGenerate), {}).model.isEmpty());
    }

    // --- request (8.1) ---

    void textGenerateSendsItsSystemPromptAndSettings() {
        Bench bench;
        const Result<JobRequest> job = bench.request(kTextGenerate, {{"system", "Answer in French."}, {"prompt", "a fox"},
                                                                    {"seed", qint64(3)}, {"max_tokens", 300},
                                                                    {"temperature", 0.8}});
        QVERIFY(job.ok());
        QCOMPARE(job->capability, QStringLiteral("text.generate"));
        QCOMPARE(job->model, handle("qwen3.5-2b"));
        QCOMPARE(job->params, (QJsonObject{{"system", "Answer in French."}, {"prompt", "a fox"}, {"seed", 3},
                                           {"max_tokens", 300}, {"temperature", 0.8}, {"thinking", false}}));
        QVERIFY(job->inputs.isEmpty());
    }

    void unsetSettingsAreTheServersOwn() {
        Bench bench;
        const Result<JobRequest> job = bench.request(kTextGenerate, {{"prompt", "a fox"}});
        QVERIFY(job.ok());
        // The seed's default (0) and thinking's (off) are sent; an empty system prompt,
        // max_tokens and temperature are not.
        QCOMPARE(job->params, (QJsonObject{{"prompt", "a fox"}, {"seed", 0}, {"thinking", false}}));
    }

    void textGenerateCanThink() {
        Bench bench;
        QCOMPARE(bench.request(kTextGenerate, {{"prompt", "a fox"}, {"thinking", true}})->params.value("thinking"), QJsonValue(true));
    }

    // A thinking model spent all 4096 tokens of the DGX's default answer reasoning about a fox
    // and wrote nothing (Qwen3.5 2B on tensorfold, 2026-11-20); without it, 41 tokens.
    void enhancePromptNeverThinks() {
        Bench bench;
        QCOMPARE(bench.request(kEnhance, {{"prompt", "a fox"}})->params.value("thinking"), QJsonValue(false));
    }

    void enhancePromptAsksForItsStyle() {
        Bench bench;
        const Result<JobRequest> detailed = bench.request(kEnhance, {{"prompt", "a fox in snow"}});
        QVERIFY(detailed.ok());
        QCOMPARE(detailed->params.value("prompt").toString(), QStringLiteral("a fox in snow"));
        const QString paragraph = detailed->params.value("system").toString();
        QVERIFY2(paragraph.contains("one paragraph"), qPrintable(paragraph));
        QVERIFY(paragraph.contains("the prompt alone"));

        const Result<JobRequest> tags = bench.request(kEnhance, {{"prompt", "a fox in snow"}, {"style", "tags"}});
        QVERIFY(tags->params.value("system").toString().contains("comma-separated"));
        const Result<JobRequest> photo = bench.request(kEnhance, {{"prompt", "a fox in snow"}, {"style", "photograph"}});
        QVERIFY(photo->params.value("system").toString().contains("lens"));
    }

    void enhancePromptAddsTheUsersInstructions() {
        Bench bench;
        const Result<JobRequest> job = bench.request(kEnhance, {{"prompt", "a fox"}, {"instructions", "Make it night."}});
        QVERIFY(job->params.value("system").toString().endsWith("Make it night."));
    }

    void anImageGoesAlongFittedWithinThe1024Square() {
        Bench bench;
        const Result<JobRequest> large =
            bench.request(kTextGenerate, {{"prompt", "Describe it."}, {"image", bench.image(writePng(bench.file("big.png"), 2048, 1536))}});
        QVERIFY2(large.ok(), qPrintable(large.ok() ? QString() : large.error().message));
        const QString sent = large->inputs.value("image").toObject().value("path").toString();
        QVERIFY(sent.startsWith(bench.workDir()));
        QCOMPARE(sizeOf(sent), QSize(1024, 768));

        const QString small = writePng(bench.file("small.png"), 512, 384);
        const Result<JobRequest> asItIs = bench.request(kTextGenerate, {{"prompt", "Describe it."}, {"image", bench.image(small)}});
        QCOMPARE(asItIs->inputs.value("image").toObject().value("path").toString(), small);
    }

    void anImageToAModelThatSaysItTakesNoneIsRefused() {
        Bench bench;
        const Value image = bench.image(writePng(bench.file("small.png"), 64, 64));
        const Result<JobRequest> refused =
            bench.request(kTextGenerate, {{"prompt", "Describe it."}, {"image", image}}, selection({{"input", QJsonArray{"text"}}}));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().input, QStringLiteral("image"));
        QVERIFY(refused.error().message.contains("qwen3.5-2b"));
        // A model that does not say (this machine's) is asked; one that takes images is too.
        QVERIFY(bench.request(kTextGenerate, {{"prompt", "Describe it."}, {"image", image}}).ok());
        QVERIFY(bench.request(kTextGenerate, {{"prompt", "Describe it."}, {"image", image}},
                              selection({{"input", QJsonArray{"text", "image"}}}))
                    .ok());
    }

    void aLongerAnswerThanTheModelAllowsIsRefused() {
        Bench bench;
        const Result<JobRequest> refused =
            bench.request(kTextGenerate, {{"prompt", "x"}, {"max_tokens", 10000}}, selection({}, {{"max_tokens", 8192}}));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, QStringLiteral("value_out_of_range"));
        QCOMPARE(refused.error().input, QStringLiteral("max_tokens"));
    }

    // --- outputs (3.3) ---

    void outputsAreTheAnswerWithoutReasoningAndTheValuesUsed() {
        Bench bench;
        const Result<Values> out = bench.outputs(kTextGenerate, "<think>\nThe user wants a fox.\n</think>\n\n  A red fox.  ",
                                                 {{"seed", 11}, {"model", handle("qwen-used")}});
        QVERIFY(out.ok());
        QCOMPARE(out->value("text").text(), QStringLiteral("A red fox."));
        QCOMPARE(out->value("seed").number(), 11.0);
        QCOMPARE(out->value("model").json().toObject(), handle("qwen-used"));
    }

    void textGenerateKeepsQuotesButEnhancePromptDropsTheWrapping() {
        Bench bench;
        QCOMPARE(bench.outputs(kTextGenerate, "\"A red fox.\"")->value("text").text(), QStringLiteral("\"A red fox.\""));
        QCOMPARE(bench.outputs(kEnhance, "Enhanced prompt: \"A red fox in deep snow.\"")->value("text").text(),
                 QStringLiteral("A red fox in deep snow."));
        QCOMPARE(bench.outputs(kEnhance, "**Prompt:**\n“A red fox.”")->value("text").text(), QStringLiteral("A red fox."));
        // Words of the prompt that only look like a label stay.
        QCOMPARE(bench.outputs(kEnhance, "A fox: red, small, in snow")->value("text").text(),
                 QStringLiteral("A fox: red, small, in snow"));
    }

    void anEmptyAnswerIsAFailure() {
        Bench bench;
        const Result<Values> out = bench.outputs(kEnhance, "<think>hmm</think>  ");
        QVERIFY(!out.ok());
        QCOMPARE(out.error().code, QStringLiteral("node_failed"));
    }

    // --- end to end ---

    // Enhance Prompt feeds Generate Image: the image server gets the cleaned answer.
    void anEnhancedPromptReachesTheImageModel() {
        FakeMachine machine("<think>ok</think>Prompt: \"A red fox curled up in deep snow at dusk, soft blue light.\"");
        const auto node = [](const QString &type, const QJsonObject &inputs) {
            return QJsonObject{{"type", type}, {"version", "1.0.0"}, {"inputs", inputs}};
        };
        const QJsonObject nodes{
            {"enhance", node(kEnhance, {{"prompt", "fox in snow"}, {"seed", 5}})},
            {"generate", node("fugue/generate-image", {{"prompt", QJsonObject{{"from", "enhance.text"}}}, {"seed", 1}})},
            {"preview", node("fugue/preview", {{"value", QJsonObject{{"from", "generate.image"}}}})}};
        writeFile(machine.file("flow.fugue"), compact(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}}));

        QCOMPARE(machine.fugue({"run", machine.file("flow.fugue"), "--wait", "--json"}), 0);
        const QJsonObject chat = machine.sent("/v1/chat/completions");
        QCOMPARE(chat.value("model").toString(), QStringLiteral("qwen3.5-2b"));
        QCOMPARE(chat.value("seed").toInt(), 5);
        QCOMPARE(chat.value("messages").toArray().last().toObject().value("content").toString(), QStringLiteral("fox in snow"));
        QCOMPARE(machine.sent("/sdcpp/v1/img_gen").value("prompt").toString(),
                 QStringLiteral("A red fox curled up in deep snow at dusk, soft blue light."));
    }
};

QTEST_GUILESS_MAIN(TestTextGeneration)
#include "test_text_generation.moc"
