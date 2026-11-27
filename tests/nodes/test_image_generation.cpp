// Generate Image and Edit Image (spec 14.1, 8.1, 8.4, 3.3).
//
// Seam: the ProviderKernel each node type registers (requirement, request, outputs), driven
// with a hand-built Selection the way the scheduler drives it. The end-to-end tests at the
// bottom run the shipped `fugue` binary against a fake omarchy model server.

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

#include "../test_paths.h"
#include "engine/embed.h"
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

const QString kGenerate = QStringLiteral("fugue/generate-image");
const QString kEdit = QStringLiteral("fugue/edit-image");

QJsonObject handle(const QString &name) { return QJsonObject{{"resource", "fugue/image-model"}, {"name", name}}; }

// What a provider offers for a model: defaults from the server's capabilities (8.1).
Selection selection(const QJsonObject &defaults = {}, const QJsonObject &limits = {}) {
    Selection chosen;
    chosen.instance = QStringLiteral("This machine");
    chosen.engine = QStringLiteral("sdcpp");
    chosen.model.handle = handle("z-image");
    chosen.model.status = ModelStatus::Ready;
    chosen.model.defaults = defaults;
    chosen.model.limits = limits;
    return chosen;
}

// A PNG of the given size, written to path.
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

// A picture whose every pixel differs (a gradient with a checker), so a pixel taken from the wrong
// place shows. 8-bit RGB.
QString writeGradient(const QString &path, int width, int height) {
    initVips();
    const vips::VImage x = vips::VImage::xyz(width, height).extract_band(0);
    const vips::VImage y = vips::VImage::xyz(width, height).extract_band(1);
    const vips::VImage red = (x * 255.0 / width).cast(VIPS_FORMAT_UCHAR);
    const vips::VImage green = (y * 255.0 / height).cast(VIPS_FORMAT_UCHAR);
    const vips::VImage blue = (((x + y) % 2.0) * 90.0 + 20.0).cast(VIPS_FORMAT_UCHAR);
    vips::VImage::bandjoin(std::vector<vips::VImage>{red, green, blue}).write_to_file(path.toUtf8().constData());
    return path;
}

// An 8-bit gray mask of the given size: white in the rectangle, black elsewhere.
QString writeMask(const QString &path, int width, int height, const QRect &white) {
    initVips();
    if (white.isEmpty()) {
        vips::VImage::black(width, height).cast(VIPS_FORMAT_UCHAR).write_to_file(path.toUtf8().constData());
        return path;
    }
    const vips::VImage x = vips::VImage::xyz(width, height).extract_band(0);
    const vips::VImage y = vips::VImage::xyz(width, height).extract_band(1);
    const vips::VImage inside = (x >= white.left()) & (x <= white.right()) & (y >= white.top()) & (y <= white.bottom());
    inside.copy(vips::VImage::option()->set("interpretation", VIPS_INTERPRETATION_B_W))
        .write_to_file(path.toUtf8().constData());
    return path;
}

// What a provider that takes a mask offers for a model (8.1: the `inputs` property).
Selection maskSelection() {
    Selection chosen = selection();
    chosen.model.properties = QJsonObject{{"inputs", QJsonArray{"mask"}}};
    return chosen;
}

// The 8-bit values of an image file or handle, band-interleaved.
QByteArray eightBit(const vips::VImage &image) {
    vips::VImage values = image;
    if (values.format() == VIPS_FORMAT_USHORT)
        values = (values.cast(VIPS_FORMAT_FLOAT) / 257.0).cast(VIPS_FORMAT_FLOAT);
    else if (values.format() == VIPS_FORMAT_FLOAT)
        values = values * 255.0;
    values = (values + 0.5).floor().cast(VIPS_FORMAT_UCHAR);
    size_t size = 0;
    void *memory = values.write_to_memory(&size);
    const QByteArray bytes(static_cast<const char *>(memory), qsizetype(size));
    g_free(memory);
    return bytes;
}

class Bench {
public:
    Bench() : hashes(dir.filePath("hashes.json")) {
        nodes::addBuiltinNodeTypes(registry);
        nodes::addImageGenerationKernels(kernels);
        QDir().mkpath(workDir());
    }

    // An image input as the scheduler hands it over: a probed file.
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
        context.node = QStringLiteral("g");
        context.run = QStringLiteral("20261119-120000-000-test");
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

    QTemporaryDir dir;
    FileHashes hashes;
    Registry registry;
    Kernels kernels;
};

QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// This machine as omarchy-local-ai leaves it (status.json and the key, spec 8.7), with one
// image model that edits, served by a fake sd-server (stable-diffusion.cpp's sdcpp API, the
// shapes test_omarchy_provider.cpp uses). The shipped `fugue` finds it through $HOME, the
// way OmarchyPaths::standard() does; providers.json is absent, so "This machine" is used.
class FakeMachine {
public:
    static constexpr const char *kKey = "local-key-0123456789";
    static constexpr const char *kRecipe = "flux2-klein-4b-sdcpp";

    FakeMachine() {
        server.listen();
        const QJsonObject row{{"recipe", kRecipe}, {"kind", "image"}, {"edit", true}, {"name", kRecipe},
                              {"runtime", "sdcpp"}, {"unit", "active"},
                              {"endpoint", QString::fromLatin1(server.origin()) + "/v1"}, {"fits", true},
                              {"fit_reason", ""}, {"error", ""}};
        writeFile(home("/.local/state/omarchy/agent-local/status.json"),
                  compact(QJsonObject{{"models", QJsonObject{{kRecipe, row}}}, {"last", QJsonValue::Null}}));
        writeFile(home("/.config/omarchy/agent-local/key"), QByteArray(kKey) + "\n");

        server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &request) {
            if (request.headers.value("authorization") != QByteArray("Bearer ") + kKey)
                return HttpResponse{401, "{\"error\":\"unauthorized\"}"};
            const QJsonObject imgGen{{"width", 1024}, {"height", 1024}, {"seed", -1},
                                     {"sample_params", QJsonObject{{"sample_steps", 4},
                                                                   {"guidance", QJsonObject{{"txt_cfg", 1.0}}}}}};
            return HttpResponse{200, compact(QJsonObject{
                                         {"supported_modes", QJsonArray{"img_gen"}},
                                         {"defaults_by_mode", QJsonObject{{"img_gen", imgGen}}},
                                         {"limits", QJsonObject{{"min_width", 64}, {"max_width", 4096},
                                                                {"min_height", 64}, {"max_height", 4096}}}})};
        });
        // Each job is done at its first poll, with a picture of the size asked for and the
        // seed as sd-server reports it (a decimal string in generation_info).
        server.on("POST", "/sdcpp/v1/img_gen", [this](const HttpRequest &request) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            const QString id = QStringLiteral("job_%1").arg(++m_jobs);
            const int width = body.value("width").toInt(), height = body.value("height").toInt();
            const QByteArray png = readFile(writePng(m_dir.filePath(id + ".png"), width, height));
            const QJsonObject info{{"seed", QString::number(body.value("seed").toInteger())},
                                   {"width", width}, {"height", height},
                                   {"sample_params", QJsonObject{{"sample_steps", body.value("sample_params").toObject().value("sample_steps")}}}};
            const QJsonObject done{
                {"id", id}, {"kind", "img_gen"}, {"status", "completed"}, {"error", QJsonValue::Null},
                {"result", QJsonObject{{"output_format", "png"},
                                       {"images", QJsonArray{QJsonObject{{"index", 0},
                                                                         {"b64_json", QString::fromLatin1(png.toBase64())},
                                                                         {"generation_info", info}}}}}}};
            server.on("GET", "/sdcpp/v1/jobs/" + id.toLatin1(), [done](const HttpRequest &) {
                return HttpResponse{200, compact(done)};
            });
            return HttpResponse{202, compact(QJsonObject{{"id", id}, {"kind", "img_gen"}, {"status", "queued"},
                                                         {"poll_url", "/sdcpp/v1/jobs/" + id}})};
        });
    }

    QString home(const QString &path = {}) const { return m_dir.filePath("home") + path; }
    QString file(const QString &name) const { return m_dir.filePath(name); }

    // The img_gen bodies the server was sent, in order.
    QList<QJsonObject> jobsSent() const {
        QList<QJsonObject> bodies;
        for (const HttpRequest &request : server.requestsTo("POST", "/sdcpp/v1/img_gen"))
            bodies.append(QJsonDocument::fromJson(request.body).object());
        return bodies;
    }

    // The shipped binary, with this machine as its home. It is waited for with the event loop
    // running, so the fake server in this process can answer the run's worker.
    struct Ran {
        int exitCode = -1;
        QByteArray out;
        QList<QJsonObject> lines() const {
            QList<QJsonObject> list;
            for (const QByteArray &line : out.trimmed().split('\n'))
                list.append(QJsonDocument::fromJson(line).object());
            return list;
        }
    };
    Ran fugue(const QStringList &args) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *name : {"QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "FUGUE_NODE_MANIFESTS"})
            env.remove(QString::fromLatin1(name));
        env.insert("HOME", home());
        env.insert("XDG_CACHE_HOME", file("cache"));
        env.insert("XDG_STATE_HOME", file("state"));
        env.insert("XDG_CONFIG_HOME", file("config"));
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(m_dir.path()); // inside the write roots (13.6)
        process.start(fugueBinary(), args);
        Ran ran;
        if (!process.waitForStarted())
            return ran;
        QElapsedTimer clock;
        clock.start();
        while (process.state() != QProcess::NotRunning && clock.elapsed() < 60000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
            qWarning("fugue timed out: %s", qPrintable(args.join(' ')));
            return ran;
        }
        ran.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        ran.out = process.readAllStandardOutput();
        if (ran.exitCode != 0)
            qWarning("fugue %s: %s %s", qPrintable(args.join(' ')), ran.out.constData(),
                     process.readAllStandardError().constData());
        return ran;
    }

    FakeHttpServer server;

private:
    QTemporaryDir m_dir;
    int m_jobs = 0;
};

} // namespace

class TestImageGeneration : public QObject {
    Q_OBJECT

private slots:
    // --- requirement (8.3, 8.4) ---

    void bothNodesHaveAProviderKernel() {
        Bench bench;
        QVERIFY(bench.kernel(kGenerate));
        QVERIFY(bench.kernel(kEdit));
    }

    void generateAsksForImageGenerateAndTheModelInput() {
        Bench bench;
        const Requirement wanted =
            bench.kernel(kGenerate)->requirement(bench.type(kGenerate), {{"prompt", "a cat"}, {"model", handle("flux")}});
        QCOMPARE(wanted.capability, QStringLiteral("image.generate"));
        QCOMPARE(wanted.model, handle("flux"));
        QVERIFY(wanted.properties.isEmpty());
        QVERIFY(wanted.nodePin.isEmpty());
    }

    void editAsksForImageEdit() {
        Bench bench;
        const Requirement wanted = bench.kernel(kEdit)->requirement(bench.type(kEdit), {{"instruction", "make it red"}});
        QCOMPARE(wanted.capability, QStringLiteral("image.edit"));
        QVERIFY(wanted.model.isEmpty());
    }

    // Under validate a random seed or a link that has not run is missing (M0, #14).
    void requirementCopesWithNoInputsAtAll() {
        Bench bench;
        QCOMPARE(bench.kernel(kGenerate)->requirement(bench.type(kGenerate), {}).capability,
                 QStringLiteral("image.generate"));
        QCOMPARE(bench.kernel(kEdit)->requirement(bench.type(kEdit), {}).capability, QStringLiteral("image.edit"));
    }

    // --- Generate Image: the request (8.1) ---

    void generateSendsThePromptSeedAndTheModelChosen() {
        Bench bench;
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}, {"seed", qint64(42)}});
        QVERIFY2(sent.ok(), qPrintable(sent.ok() ? QString() : sent.error().message));
        QCOMPARE(sent->capability, QStringLiteral("image.generate"));
        QCOMPARE(sent->model, handle("z-image"));
        QCOMPARE(sent->params.value("prompt").toString(), QStringLiteral("a cat"));
        QCOMPARE(sent->params.value("seed").toInteger(), qint64(42));
        // An empty negative prompt and a strength with no start image say nothing.
        QVERIFY(!sent->params.contains("negative_prompt"));
        QVERIFY(!sent->params.contains("strength"));
        QVERIFY(sent->inputs.isEmpty());
    }

    void generateSendsANegativePromptWhenThereIsOne() {
        Bench bench;
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}, {"negative_prompt", "dogs"}});
        QCOMPARE(sent->params.value("negative_prompt").toString(), QStringLiteral("dogs"));
    }

    void unsetOptionalInputsTakeTheModelsDefaults() {
        Bench bench;
        const QJsonObject defaults{{"width", 768}, {"height", 1344}, {"steps", 8}, {"guidance", 3.5}};
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}}, selection(defaults));
        QCOMPARE(sent->params.value("width").toInt(), 768);
        QCOMPARE(sent->params.value("height").toInt(), 1344);
        QCOMPARE(sent->params.value("steps").toInt(), 8);
        QCOMPARE(sent->params.value("guidance").toDouble(), 3.5);
    }

    void setInputsWinOverTheModelsDefaults() {
        Bench bench;
        const QJsonObject defaults{{"width", 768}, {"height", 1344}, {"steps", 8}, {"guidance", 3.5}};
        const Result<JobRequest> sent = bench.request(
            kGenerate, {{"prompt", "a cat"}, {"width", 512}, {"height", 640}, {"steps", 20}, {"guidance", 7.0}},
            selection(defaults));
        QCOMPARE(sent->params.value("width").toInt(), 512);
        QCOMPARE(sent->params.value("height").toInt(), 640);
        QCOMPARE(sent->params.value("steps").toInt(), 20);
        QCOMPARE(sent->params.value("guidance").toDouble(), 7.0);
    }

    // #72: a default size of 0 (or less) is no default: left to the server, never sent as a
    // size, so the model's limits do not fail it.
    void aDefaultSizeOfZeroIsLeftToTheServer() {
        Bench bench;
        const QJsonObject defaults{{"width", 0}, {"height", 0}, {"steps", 8}};
        const QJsonObject limits{{"min_width", 64}, {"max_width", 4096}, {"min_height", 64}, {"max_height", 4096}};
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}}, selection(defaults, limits));
        QVERIFY2(sent.ok(), qPrintable(sent.ok() ? QString() : sent.error().message));
        QVERIFY(!sent->params.contains("width"));
        QVERIFY(!sent->params.contains("height"));
        QCOMPARE(sent->params.value("steps").toInt(), 8);
    }

    // A model that reports no defaults leaves them to the server.
    void noDefaultsLeavesTheParametersOut() {
        Bench bench;
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}});
        for (const char *key : {"width", "height", "steps", "guidance"})
            QVERIFY2(!sent->params.contains(QLatin1String(key)), key);
    }

    // --- Generate Image: the start image (Fantasi's prepareBase) ---

    // A PNG a model works in (a multiple of 64, near a megapixel) goes as it is, at its own
    // size, with the strength.
    void aStartImageTheModelWorksInGoesAsItIs() {
        Bench bench;
        const Value start = bench.image(writePng(bench.file("start-in.png"), 1024, 768));
        const QJsonObject defaults{{"width", 512}, {"height", 512}};
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}}, selection(defaults));
        QVERIFY2(sent.ok(), qPrintable(sent.ok() ? QString() : sent.error().message));
        QCOMPARE(sent->inputs.value("image").toObject().value("path").toString(), start.media().path());
        QCOMPARE(sent->params.value("width").toInt(), 1024);
        QCOMPARE(sent->params.value("height").toInt(), 768);
        QCOMPARE(sent->params.value("strength").toDouble(), 0.7);
    }

    // Anything else covers the nearest size in its own shape near a megapixel, cut from the
    // middle: 1000x600 is 1344x768 (Fantasi's fitMegapixel).
    void anyOtherStartImageIsFittedNearAMegapixel() {
        Bench bench;
        const Value start = bench.image(writePng(bench.file("odd.png"), 1000, 600));
        const Result<JobRequest> sent =
            bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}, {"strength", 0.4}});
        const QString path = sent->inputs.value("image").toObject().value("path").toString();
        QCOMPARE(QFileInfo(path).dir().absolutePath(), QDir(bench.workDir()).absolutePath());
        QCOMPARE(sizeOf(path), QSize(1344, 768));
        QCOMPARE(sent->params.value("width").toInt(), 1344);
        QCOMPARE(sent->params.value("height").toInt(), 768);
        QCOMPARE(sent->params.value("strength").toDouble(), 0.4);
    }

    // A photo with an EXIF orientation is upright: its shape sets the size, the way it is seen.
    void aRotatedStartImageIsSizedUpright() {
        Bench bench;
        const QString photo = bench.file("turned.jpg");
        vips::VImage pixels = (vips::VImage::black(1024, 512, vips::VImage::option()->set("bands", 3)) + 90).cast(VIPS_FORMAT_UCHAR).copy();
        pixels.set("orientation", 6); // seen as 512 x 1024
        pixels.jpegsave(photo.toUtf8().constData());
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}, {"image", bench.image(photo)}});
        QVERIFY2(sent.ok(), qPrintable(sent.ok() ? QString() : sent.error().message));
        // Nearest a megapixel in steps of 64 for 1:2 is 704 x 1472 (fitMegapixel).
        QCOMPARE(sent->params.value("width").toInt(), 704);
        QCOMPARE(sent->params.value("height").toInt(), 1472);
        QCOMPARE(sizeOf(sent->inputs.value("image").toObject().value("path").toString()), QSize(704, 1472));
    }

    // A size set on the node wins: the start image is fitted to it.
    void aSetSizeFitsTheStartImageToIt() {
        Bench bench;
        const Value start = bench.image(writePng(bench.file("start-in.png"), 1024, 768));
        const Result<JobRequest> sent =
            bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}, {"width", 512}, {"height", 512}});
        QCOMPARE(sizeOf(sent->inputs.value("image").toObject().value("path").toString()), QSize(512, 512));
        QCOMPARE(sent->params.value("width").toInt(), 512);
        QCOMPARE(sent->params.value("height").toInt(), 512);
    }

    // One side set: the other follows the start image's shape, to a multiple of 64.
    void oneSetSideKeepsTheStartImagesShape() {
        Bench bench;
        const Value start = bench.image(writePng(bench.file("wide.png"), 2048, 1024));
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}, {"width", 768}});
        QCOMPARE(sent->params.value("width").toInt(), 768);
        QCOMPARE(sent->params.value("height").toInt(), 384);
        QCOMPARE(sizeOf(sent->inputs.value("image").toObject().value("path").toString()), QSize(768, 384));
    }

    // The model's limits (8.1) say what it can make; outside them is a clear error, not a job
    // the server refuses later.
    void aSizeOutsideTheModelsLimitsIsRefused() {
        Bench bench;
        const Selection limited = selection({}, QJsonObject{{"min_width", 256}, {"max_width", 2048},
                                                            {"min_height", 256}, {"max_height", 2048}});
        const Result<JobRequest> wide = bench.request(kGenerate, {{"prompt", "a cat"}, {"width", 4096}}, limited);
        QVERIFY(!wide.ok());
        QCOMPARE(wide.error().code, QStringLiteral("value_out_of_range"));
        QCOMPARE(wide.error().input, QStringLiteral("width"));
        QVERIFY(wide.error().message.contains("2048"));
        const Result<JobRequest> flat = bench.request(kGenerate, {{"prompt", "a cat"}, {"height", 128}}, limited);
        QCOMPARE(flat.error().input, QStringLiteral("height"));
        QVERIFY(bench.request(kGenerate, {{"prompt", "a cat"}, {"width", 2048}, {"height", 256}}, limited).ok());
    }

    // --- Edit Image: the request ---

    void editSendsTheInstructionAsThePromptWithItsReferences() {
        Bench bench;
        const Value first = bench.image(writePng(bench.file("a.png"), 1024, 768));
        const Value second = bench.image(writePng(bench.file("b.png"), 1000, 600));
        const QJsonObject defaults{{"width", 512}, {"height", 512}, {"steps", 4}};
        const Result<JobRequest> sent = bench.request(
            kEdit, {{"instruction", "put the cat on the sofa"}, {"images", Value::list({first, second})}, {"seed", qint64(7)}},
            selection(defaults));
        QVERIFY2(sent.ok(), qPrintable(sent.ok() ? QString() : sent.error().message));
        QCOMPARE(sent->capability, QStringLiteral("image.edit"));
        QCOMPARE(sent->params.value("prompt").toString(), QStringLiteral("put the cat on the sofa"));
        QCOMPARE(sent->params.value("seed").toInteger(), qint64(7));
        QCOMPARE(sent->params.value("steps").toInt(), 4);
        const QJsonArray images = sent->inputs.value("images").toArray();
        QCOMPARE(images.size(), 2);
        QCOMPARE(images.at(0).toObject().value("path").toString(), first.media().path());
        QCOMPARE(sizeOf(images.at(1).toObject().value("path").toString()), QSize(1344, 768));
        // The result takes the first reference's size, not the model's default.
        QCOMPARE(sent->params.value("width").toInt(), 1024);
        QCOMPARE(sent->params.value("height").toInt(), 768);
    }

    void editTakesFourReferencesByDefault() {
        Bench bench;
        QList<Value> four, five;
        for (int i = 0; i < 5; ++i) {
            const Value image = bench.image(writePng(bench.file(QString("r%1.png").arg(i)), 1024, 1024));
            if (i < 4)
                four.append(image);
            five.append(image);
        }
        QVERIFY(bench.request(kEdit, {{"instruction", "mix"}, {"images", Value::list(four)}}).ok());
        const Result<JobRequest> tooMany = bench.request(kEdit, {{"instruction", "mix"}, {"images", Value::list(five)}});
        QVERIFY(!tooMany.ok());
        QCOMPARE(tooMany.error().code, QStringLiteral("value_out_of_range"));
        QCOMPARE(tooMany.error().input, QStringLiteral("images"));
        QVERIFY(tooMany.error().message.contains("4"));
    }

    // Four is Fugue's starting choice (14.1); a limit the model reports is the one that holds,
    // higher or lower, since the model knows what it takes.
    void aModelsReportedReferenceLimitHolds() {
        Bench bench;
        QList<Value> five;
        for (int i = 0; i < 5; ++i)
            five.append(bench.image(writePng(bench.file(QString("r%1.png").arg(i)), 1024, 1024)));
        const Selection roomy = selection({}, QJsonObject{{"max_reference_images", 8}});
        QVERIFY(bench.request(kEdit, {{"instruction", "mix"}, {"images", Value::list(five)}}, roomy).ok());
        const Selection tight = selection({}, QJsonObject{{"max_reference_images", 2}});
        QVERIFY(bench.request(kEdit, {{"instruction", "mix"}, {"images", Value::list(five.mid(0, 2))}}, tight).ok());
        const Result<JobRequest> three = bench.request(kEdit, {{"instruction", "mix"}, {"images", Value::list(five.mid(0, 3))}}, tight);
        QVERIFY(!three.ok());
        QCOMPARE(three.error().code, QStringLiteral("value_out_of_range"));
        QCOMPARE(three.error().input, QStringLiteral("images"));
        QVERIFY2(three.error().message.contains("at most 2"), qPrintable(three.error().message));
    }

    // --- outputs: the image and the values actually used (3.3, 8.1) ---

    void outputsAreTheImageAndTheValuesTheProviderUsed() {
        for (const QString &id : {kGenerate, kEdit}) {
            Bench bench;
            const QString made = writePng(QDir(bench.workDir()).filePath("image.png"), 832, 1216);
            const JobResult job{
                QJsonObject{{"image", QJsonObject{{"path", made}, {"format", "png"}}}},
                QJsonObject{{"seed", qint64(123456789012)}, {"width", 832}, {"height", 1216}, {"steps", 4},
                            {"model", handle("z-image")}, {"engine", "sdcpp"}}};
            const Result<Values> out = bench.kernel(id)->outputs(bench.context(id, {{"seed", qint64(5)}}), selection(), job);
            QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().message));
            QCOMPARE(QStringList(out->keys()), (QStringList{"height", "image", "model", "seed", "width"}));
            QCOMPARE(out->value("image").media().path(), made);
            QCOMPARE(out->value("image").media().width(), 832);
            QCOMPARE(out->value("seed").json().toInteger(), qint64(123456789012));
            QCOMPARE(out->value("width").number(), 832.0);
            QCOMPARE(out->value("height").number(), 1216.0);
            QCOMPARE(out->value("model").json().toObject(), handle("z-image"));
        }
    }

    // What the provider does not report is what was asked for: the seed sent, the model
    // chosen, and the size of the image itself.
    void unreportedValuesComeFromTheRequestAndTheImage() {
        Bench bench;
        const QString made = writePng(QDir(bench.workDir()).filePath("image.png"), 640, 448);
        const JobResult job{QJsonObject{{"image", QJsonObject{{"path", made}, {"format", "png"}}}}, QJsonObject{}};
        const Result<Values> out =
            bench.kernel(kGenerate)->outputs(bench.context(kGenerate, {{"seed", qint64(99)}}), selection(), job);
        QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().message));
        QCOMPARE(out->value("seed").json().toInteger(), qint64(99));
        QCOMPARE(out->value("width").number(), 640.0);
        QCOMPARE(out->value("height").number(), 448.0);
        QCOMPARE(out->value("model").json().toObject(), handle("z-image"));
    }

    void aJobWithNoImageFailsTheNode() {
        Bench bench;
        const Result<Values> out = bench.kernel(kGenerate)->outputs(bench.context(kGenerate, {}), selection(), JobResult{});
        QVERIFY(!out.ok());
        QCOMPARE(out.error().code, QStringLiteral("node_failed"));
    }

    // --- Generate Image: a mask (14.4) ---

    // Asked with a mask, "any ready model" prefers one that takes it; without, nothing changes.
    void aMaskMakesTheRequirementPreferAModelThatTakesOne() {
        Bench bench;
        const Value start = bench.image(writePng(bench.file("start.png"), 1024, 768));
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 1024, 768, QRect(100, 100, 50, 50)));
        const Requirement with = bench.kernel(kGenerate)->requirement(
            bench.type(kGenerate), {{"prompt", "a cat"}, {"image", start}, {"mask", mask}});
        QCOMPARE(with.preferred, (QJsonObject{{"inputs", QJsonArray{"mask"}}}));
        QVERIFY(with.properties.isEmpty()); // a preference: the node says mask_unsupported itself
        const Requirement without =
            bench.kernel(kGenerate)->requirement(bench.type(kGenerate), {{"prompt", "a cat"}, {"image", start}});
        QVERIFY(without.preferred.isEmpty());
        QVERIFY(bench.kernel(kEdit)->requirement(bench.type(kEdit), {}).preferred.isEmpty());
    }

    // The mask goes as an 8-bit gray PNG of the size the start image goes at, cut as the image is:
    // 1000x600 goes at 1344x768, so the mask does too.
    void theMaskIsSentAtTheStartImagesSize() {
        Bench bench;
        const Value start = bench.image(writeGradient(bench.file("odd.png"), 1000, 600));
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 1000, 600, QRect(200, 100, 300, 200)));
        const Result<JobRequest> sent = bench.request(
            kGenerate, {{"prompt", "a cat"}, {"image", start}, {"mask", mask}}, maskSelection());
        QVERIFY2(sent.ok(), qPrintable(sent.ok() ? QString() : sent.error().message));
        const QString image = sent->inputs.value("image").toObject().value("path").toString();
        const QString path = sent->inputs.value("mask").toObject().value("path").toString();
        QVERIFY(!path.isEmpty());
        QCOMPARE(sizeOf(path), QSize(1344, 768));
        QCOMPARE(sizeOf(path), sizeOf(image));
        const vips::VImage sentMask = vips::VImage::new_from_file(path.toUtf8().constData());
        QCOMPARE(sentMask.bands(), 1);
        QCOMPARE(int(sentMask.format()), int(VIPS_FORMAT_UCHAR));
        // The painted box lands where the picture's pixels under it do: 1344/1000 scale, the middle kept.
        const auto at = [&sentMask](int x, int y) { return int(sentMask.getpoint(x, y).at(0)); };
        QVERIFY(at(int(350 * 1.344), int(200 * 1.28)) > 250);
        QVERIFY(at(int(50 * 1.344), int(50 * 1.28)) < 5);
        // Nothing else of the request changed for the mask.
        QCOMPARE(sent->params.value("width").toInt(), 1344);
        QCOMPARE(sent->params.value("height").toInt(), 768);
    }

    // A mask the size of an image the model works in as it is goes as it is.
    void aMaskOfTheImagesOwnSizeIsWrittenAsItIs() {
        Bench bench;
        const Value start = bench.image(writeGradient(bench.file("start.png"), 1024, 768));
        const QString painted = writeMask(bench.file("mask.png"), 1024, 768, QRect(10, 20, 30, 40));
        const Result<JobRequest> sent = bench.request(
            kGenerate, {{"prompt", "a cat"}, {"image", start}, {"mask", bench.image(painted)}}, maskSelection());
        QVERIFY2(sent.ok(), qPrintable(sent.ok() ? QString() : sent.error().message));
        const QString path = sent->inputs.value("mask").toObject().value("path").toString();
        QCOMPARE(eightBit(vips::VImage::new_from_file(path.toUtf8().constData())),
                 eightBit(vips::VImage::new_from_file(painted.toUtf8().constData())));
    }

    // Without a mask the request is what it was: no mask file, no new parameter.
    void noMaskLeavesTheRequestAsItWas() {
        Bench bench;
        const Value start = bench.image(writeGradient(bench.file("start.png"), 1024, 768));
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}}, maskSelection());
        QVERIFY(sent.ok());
        QCOMPARE(QStringList(sent->inputs.keys()), (QStringList{"image"}));
        QCOMPARE(QStringList(sent->params.keys()), (QStringList{"height", "prompt", "seed", "strength", "width"}));
        QVERIFY(!QFileInfo::exists(QDir(bench.workDir()).filePath("mask.png")));
        const Result<JobRequest> plain = bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}});
        QCOMPARE(compact(sent->params), compact(plain->params));
        QCOMPARE(compact(sent->inputs), compact(plain->inputs));
    }

    // A provider that does not say it takes a mask is refused before the job, naming the ones that do.
    void aModelThatCannotTakeAMaskIsMaskUnsupported() {
        Bench bench;
        const Value start = bench.image(writeGradient(bench.file("start.png"), 1024, 768));
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 1024, 768, QRect(0, 0, 100, 100)));
        Selection chosen = selection();
        chosen.preferredOffers = {QStringLiteral("gx: FLUX.2 Klein 4B"), QStringLiteral("This machine: SDXL")};
        const Result<JobRequest> sent =
            bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}, {"mask", mask}}, chosen);
        QVERIFY(!sent.ok());
        QCOMPARE(sent.error().code, QStringLiteral("mask_unsupported"));
        QCOMPARE(sent.error().input, QStringLiteral("mask"));
        QCOMPARE(sent.error().valid, (QJsonArray{"gx: FLUX.2 Klein 4B", "This machine: SDXL"}));
        QVERIFY(sent.error().hint.contains(QStringLiteral("gx: FLUX.2 Klein 4B")));

        // Nobody can: the hint says what to do instead.
        const Result<JobRequest> nobody =
            bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}, {"mask", mask}}, selection());
        QCOMPARE(nobody.error().code, QStringLiteral("mask_unsupported"));
        QVERIFY(nobody.error().hint.contains(QStringLiteral("mask_image")));
        QVERIFY(nobody.error().valid.isEmpty());
    }

    void aMaskWithNothingPaintedIsMaskEmpty() {
        Bench bench;
        const Value start = bench.image(writeGradient(bench.file("start.png"), 1024, 768));
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 1024, 768, QRect()));
        const Result<JobRequest> sent =
            bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}, {"mask", mask}}, maskSelection());
        QVERIFY(!sent.ok());
        QCOMPARE(sent.error().code, QStringLiteral("mask_empty"));
        QCOMPARE(sent.error().input, QStringLiteral("mask"));
    }

    void aMaskWithoutAStartImageIsAnInputError() {
        Bench bench;
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 512, 512, QRect(0, 0, 10, 10)));
        const Result<JobRequest> sent = bench.request(kGenerate, {{"prompt", "a cat"}, {"mask", mask}}, maskSelection());
        QVERIFY(!sent.ok());
        QCOMPARE(sent.error().code, QStringLiteral("missing_input"));
        QCOMPARE(sent.error().input, QStringLiteral("image"));
    }

    void aMaskOfAnotherSizeThanTheStartImageFails() {
        Bench bench;
        const Value start = bench.image(writeGradient(bench.file("start.png"), 1024, 768));
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 512, 384, QRect(0, 0, 100, 100)));
        const Result<JobRequest> sent =
            bench.request(kGenerate, {{"prompt", "a cat"}, {"image", start}, {"mask", mask}}, maskSelection());
        QVERIFY(!sent.ok());
        QCOMPARE(sent.error().code, QStringLiteral("node_failed"));
        QCOMPARE(sent.error().input, QStringLiteral("mask"));
    }

    // What the model drew is put back through the mask: outside it the result is the start image,
    // pixel for pixel, whatever the model did there; inside it is the model's.
    void outsideTheMaskTheResultIsTheStartImage() {
        Bench bench;
        const QString startFile = writeGradient(bench.file("start.png"), 1024, 768);
        const QRect white(300, 200, 400, 300);
        const Value start = bench.image(startFile);
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 1024, 768, white));
        const Values inputs{{"prompt", "a cat"}, {"image", start}, {"mask", mask}};
        const Result<JobRequest> sent = bench.request(kGenerate, inputs, maskSelection());
        QVERIFY2(sent.ok(), qPrintable(sent.ok() ? QString() : sent.error().message));

        // The "model": a flat orange picture of the size asked for.
        const QString made = QDir(bench.workDir()).filePath("image.png");
        initVips();
        vips::VImage::black(1024, 768, vips::VImage::option()->set("bands", 3))
            .new_from_image(std::vector<double>{240, 120, 10})
            .cast(VIPS_FORMAT_UCHAR)
            .write_to_file(made.toUtf8().constData());
        const JobResult job{QJsonObject{{"image", QJsonObject{{"path", made}, {"format", "png"}}}}, QJsonObject{}};
        const Result<Values> out = bench.kernel(kGenerate)->outputs(bench.context(kGenerate, inputs), maskSelection(), job);
        QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().message));
        QVERIFY(out.warnings().isEmpty());

        // As the store saves it (16 bits for a working image), read back at 8.
        const Result<MediaHandle> saved =
            out->value("image").media().saved(bench.file("store"), QStringLiteral("out"), bench.hashes);
        QVERIFY2(saved.ok(), qPrintable(saved.ok() ? QString() : saved.error().message));
        QCOMPARE(saved->width(), 1024);
        QCOMPARE(saved->height(), 768);
        const vips::VImage result = vips::VImage::new_from_file(saved->path().toUtf8().constData());
        const vips::VImage original = vips::VImage::new_from_file(startFile.toUtf8().constData());
        const QByteArray got = eightBit(result);
        const QByteArray want = eightBit(original);
        QCOMPARE(got.size(), want.size());
        const QByteArray flat = eightBit(vips::VImage::new_from_file(made.toUtf8().constData()));
        int outside = 0;
        int inside = 0;
        for (int y = 0; y < 768; ++y)
            for (int x = 0; x < 1024; ++x) {
                const qsizetype at = (qsizetype(y) * 1024 + x) * 3;
                const bool masked = white.contains(x, y);
                const QByteArray pixel = got.mid(at, 3);
                if (masked ? pixel != flat.mid(at, 3) : pixel != want.mid(at, 3))
                    QFAIL(qPrintable(QStringLiteral("pixel %1,%2 is wrong").arg(x).arg(y)));
                (masked ? inside : outside)++;
            }
        QVERIFY(outside > 0 && inside > 0);
    }

    // The start image the provider's result goes back over is the one that went to the model (cut
    // from the middle, in steps of 64), not the file as it was.
    void theStartImageIsPutBackAsItWentToTheModel() {
        Bench bench;
        const QString startFile = writeGradient(bench.file("odd.png"), 1000, 600);
        const Value start = bench.image(startFile);
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 1000, 600, QRect(0, 0, 40, 40)));
        const Values inputs{{"prompt", "a cat"}, {"image", start}, {"mask", mask}};
        const Result<JobRequest> sent = bench.request(kGenerate, inputs, maskSelection());
        QVERIFY(sent.ok());
        const QString went = sent->inputs.value("image").toObject().value("path").toString();
        const QString made = QDir(bench.workDir()).filePath("image.png");
        writePng(made, 1344, 768);
        const JobResult job{QJsonObject{{"image", QJsonObject{{"path", made}, {"format", "png"}}}}, QJsonObject{}};
        const Result<Values> out = bench.kernel(kGenerate)->outputs(bench.context(kGenerate, inputs), maskSelection(), job);
        QVERIFY(out.ok());
        const Result<MediaHandle> saved =
            out->value("image").media().saved(bench.file("store"), QStringLiteral("out"), bench.hashes);
        QVERIFY(saved.ok());
        // The bottom right corner is far from the mask: it is the picture as sent.
        const vips::VImage result = vips::VImage::new_from_file(saved->path().toUtf8().constData());
        const vips::VImage sentImage = vips::VImage::new_from_file(went.toUtf8().constData());
        const QByteArray got = eightBit(result.extract_area(600, 300, 700, 400));
        const QByteArray want = eightBit(sentImage.extract_area(600, 300, 700, 400));
        QCOMPARE(got, want);
    }

    // A mask over everything is plain image to image, said as a warning; the result is the model's.
    void aFullMaskWarns() {
        Bench bench;
        const Value start = bench.image(writeGradient(bench.file("start.png"), 1024, 768));
        const Value mask = bench.image(writeMask(bench.file("mask.png"), 1024, 768, QRect(0, 0, 1024, 768)));
        const Values inputs{{"prompt", "a cat"}, {"image", start}, {"mask", mask}};
        QVERIFY(bench.request(kGenerate, inputs, maskSelection()).ok());
        const QString made = writePng(QDir(bench.workDir()).filePath("image.png"), 1024, 768);
        const JobResult job{QJsonObject{{"image", QJsonObject{{"path", made}, {"format", "png"}}}}, QJsonObject{}};
        const Result<Values> out = bench.kernel(kGenerate)->outputs(bench.context(kGenerate, inputs), maskSelection(), job);
        QVERIFY(out.ok());
        QCOMPARE(out.warnings().size(), qsizetype(1));
        QCOMPARE(out.warnings().first().code, QStringLiteral("mask_full"));
        QCOMPARE(out.warnings().first().severity, Error::Severity::Warning);
    }

    // Without a mask the result is the provider's file itself (no composite, no copy).
    void withoutAMaskTheResultIsTheProvidersFile() {
        Bench bench;
        const Value start = bench.image(writeGradient(bench.file("start.png"), 1024, 768));
        const Values inputs{{"prompt", "a cat"}, {"image", start}};
        const QString made = writePng(QDir(bench.workDir()).filePath("image.png"), 1024, 768);
        const JobResult job{QJsonObject{{"image", QJsonObject{{"path", made}, {"format", "png"}}}}, QJsonObject{}};
        const Result<Values> out = bench.kernel(kGenerate)->outputs(bench.context(kGenerate, inputs), selection(), job);
        QVERIFY(out.ok());
        QCOMPARE(out->value("image").media().path(), made);
    }

    // --- end to end: the shipped binary, a fake omarchy server (no live model here) ---

    // Generate Image with only a prompt and a seed, its picture edited by Edit Image: `fugue
    // run` from the workflow file to the images and the values used.
    void aGraphRunsEndToEndOnTheOmarchyProvider() {
        FakeMachine machine;
        const QJsonObject generate{{"type", kGenerate}, {"version", "1.0.0"},
                                   {"inputs", QJsonObject{{"prompt", "a lighthouse at dusk"}, {"seed", 42}}}};
        const QJsonObject edit{{"type", kEdit}, {"version", "1.0.0"},
                               {"inputs", QJsonObject{{"instruction", "make it night"},
                                                      {"images", QJsonArray{QJsonObject{{"from", "g.image"}}}},
                                                      {"steps", 6},
                                                      {"seed", 7}}}};
        writeFile(machine.file("flow.fugue"),
                  compact(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", QJsonObject{{"g", generate}, {"e", edit}}}}}}));

        const FakeMachine::Ran ran = machine.fugue({"run", machine.file("flow.fugue"), "--to", "e", "--wait", "--json"});
        QCOMPARE(ran.exitCode, 0);
        const QList<QJsonObject> events = ran.lines();
        QCOMPARE(events.last().value("event").toString(), QStringLiteral("run.done"));
        QCOMPARE(events.last().value("status").toString(), QStringLiteral("ok"));

        // What went to the server: the model's defaults for what the node left unset, the
        // instruction as the prompt, the generated picture as the reference.
        const QList<QJsonObject> sent = machine.jobsSent();
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent.at(0).value("prompt").toString(), QStringLiteral("a lighthouse at dusk"));
        QCOMPARE(sent.at(0).value("seed").toInteger(), qint64(42));
        QCOMPARE(sent.at(0).value("width").toInt(), 1024);
        QCOMPARE(sent.at(0).value("height").toInt(), 1024);
        QCOMPARE(sent.at(0).value("sample_params").toObject().value("sample_steps").toInt(), 4);
        QCOMPARE(sent.at(0).value("sample_params").toObject().value("guidance").toObject().value("txt_cfg").toDouble(), 1.0);
        QVERIFY(!sent.at(0).contains("init_image"));
        QCOMPARE(sent.at(1).value("prompt").toString(), QStringLiteral("make it night"));
        QCOMPARE(sent.at(1).value("seed").toInteger(), qint64(7));
        QCOMPARE(sent.at(1).value("sample_params").toObject().value("sample_steps").toInt(), 6);
        QCOMPARE(sent.at(1).value("ref_images").toArray().size(), 1);

        // The run's result: both images and the values used.
        const QString run = events.first().value("run").toString();
        const FakeMachine::Ran result = machine.fugue({"runs", "result", run, "--json"});
        QCOMPARE(result.exitCode, 0);
        const QJsonObject outputs =
            QJsonDocument::fromJson(result.out).object().value("data").toObject().value("outputs").toObject();
        const QJsonObject e = outputs.value("e").toObject();
        QVERIFY2(!e.isEmpty(), result.out.constData());
        const QString image = e.value("image").toObject().value("path").toString();
        QVERIFY2(QFile::exists(image), qPrintable(image));
        QCOMPARE(sizeOf(image), QSize(1024, 1024));
        QCOMPARE(e.value("seed").toInteger(), qint64(7));
        QCOMPARE(e.value("width").toInt(), 1024);
        QCOMPARE(e.value("height").toInt(), 1024);
        QCOMPARE(e.value("model").toObject(), handle(FakeMachine::kRecipe));
        // The reference sent was the generated picture, byte for byte (it already fits).
        QCOMPARE(QByteArray::fromBase64(sent.at(1).value("ref_images").toArray().at(0).toString().toLatin1()),
                 readFile(machine.file("job_1.png")));
    }

    // The same nodes ending in sinks: Save Image writes the edited picture with the workflow
    // embedded (engine and model from the run), and Previews show Generate Image's values used.
    void aGraphEndsInSaveImageAndPreviews() {
        FakeMachine machine;
        const auto node = [](const QString &type, const QJsonObject &inputs) {
            return QJsonObject{{"type", type}, {"version", "1.0.0"}, {"inputs", inputs}};
        };
        const auto from = [](const QString &port) { return QJsonObject{{"from", port}}; };
        const QJsonObject nodes{
            {"g", node(kGenerate, {{"prompt", "a lighthouse at dusk"}, {"seed", 42}, {"width", 768}})},
            {"e", node(kEdit, {{"instruction", "make it night"}, {"images", QJsonArray{from("g.image")}}})},
            {"s", node("fugue/save-image", {{"image", from("e.image")}, {"name", "{model}/night"},
                                            {"folder", machine.file("out")}})},
            {"seed", node("fugue/preview", {{"value", from("g.seed")}})},
            {"size", node("fugue/preview", {{"value", from("g.height")}})},
            {"model", node("fugue/preview", {{"value", from("g.model")}})}};
        writeFile(machine.file("flow.fugue"), compact(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}}));

        const FakeMachine::Ran ran = machine.fugue({"run", machine.file("flow.fugue"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 0);
        QCOMPARE(ran.lines().last().value("status").toString(), QStringLiteral("ok"));
        // Width set, height the model's default.
        QCOMPARE(machine.jobsSent().at(0).value("width").toInt(), 768);
        QCOMPARE(machine.jobsSent().at(0).value("height").toInt(), 1024);

        const FakeMachine::Ran result =
            machine.fugue({"runs", "result", ran.lines().first().value("run").toString(), "--json"});
        const QJsonObject outputs =
            QJsonDocument::fromJson(result.out).object().value("data").toObject().value("outputs").toObject();
        QCOMPARE(outputs.value("seed").toObject().value("value").toInteger(), qint64(42));
        QCOMPARE(outputs.value("size").toObject().value("value").toInt(), 1024);
        QCOMPARE(outputs.value("model").toObject().value("value").toObject(), handle(FakeMachine::kRecipe));

        const QString saved = outputs.value("s").toObject().value("file").toObject().value("path").toString();
        // `{model}` is the model the run used upstream of the Save node (a subfolder here).
        QCOMPARE(saved, machine.file("out") + "/" + FakeMachine::kRecipe + "/night.png");
        QCOMPARE(sizeOf(saved), QSize(768, 1024));
        const Result<QJsonObject> embedded = readEmbeddedWorkflow(saved);
        QVERIFY2(embedded.ok(), qPrintable(embedded.ok() ? QString() : embedded.error().message));
        const QJsonObject ranNodes = embedded->value("run").toObject().value("nodes").toObject();
        QCOMPARE(ranNodes.value("e").toObject().value("engine").toString(), QStringLiteral("sdcpp"));
        QCOMPARE(ranNodes.value("g").toObject().value("model").toObject(), handle(FakeMachine::kRecipe));
    }
};

QTEST_GUILESS_MAIN(TestImageGeneration)
#include "test_image_generation.moc"
