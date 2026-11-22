// Load Video, Load Audio, Generate Video and Generate Song (spec 14.2, 8.1, #28).
//
// Seams: the NodeKernel of the Load nodes and the ProviderKernel of the Generate nodes (requirement,
// request, outputs), driven with a hand-built Selection the way the scheduler drives them; and the
// shipped `fugue` binary run against a fake omarchy video and music server. Media fixtures are made
// with ffmpeg's lavfi sources, so none is committed. No real model is involved.

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

const QString kVideoNode = QStringLiteral("fugue/generate-video");
const QString kSongNode = QStringLiteral("fugue/generate-song");
const QString kRemixNode = QStringLiteral("fugue/remix-song");
const QString kRepaintNode = QStringLiteral("fugue/repaint-song");

bool ffmpeg(const QStringList &args) {
    QProcess process;
    process.start(QStringLiteral("ffmpeg"), QStringList{"-nostdin", "-hide_banner", "-loglevel", "error", "-y"} + args);
    if (!process.waitForFinished(60000) || process.exitCode() != 0) {
        qWarning().noquote() << "ffmpeg failed:" << process.readAllStandardError();
        return false;
    }
    return true;
}

QString makeVideo(const QString &path, int width, int height, double seconds = 1, int fps = 12) {
    if (!ffmpeg({"-f", "lavfi", "-i", QString("testsrc=size=%1x%2:rate=%3:duration=%4").arg(width).arg(height).arg(fps).arg(seconds),
                 "-c:v", "libvpx", "-b:v", "200k", path}))
        qFatal("cannot make %s", qPrintable(path));
    return path;
}

QString makeAudio(const QString &path, double seconds = 1.5) {
    if (!ffmpeg({"-f", "lavfi", "-i", QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds), path}))
        qFatal("cannot make %s", qPrintable(path));
    return path;
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

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

Selection selection(const QString &resource, const QJsonObject &defaults = {}, const QJsonObject &limits = {}) {
    Selection chosen;
    chosen.instance = QStringLiteral("gx10");
    chosen.engine = QStringLiteral("sdcpp");
    chosen.model.handle = QJsonObject{{"resource", resource}, {"name", "the-model"}};
    chosen.model.status = ModelStatus::Ready;
    chosen.model.defaults = defaults;
    chosen.model.limits = limits;
    return chosen;
}

Selection videoModel(const QJsonObject &defaults = {}, const QJsonObject &limits = {}) {
    return selection("fugue/video-model", defaults, limits);
}
Selection musicModel(const QJsonObject &limits = {}) { return selection("fugue/music-model", {}, limits); }

class Bench {
public:
    Bench() : hashes(dir.filePath("hashes.json")) {
        nodes::addBuiltinNodeTypes(registry);
        nodes::addBuiltinKernels(kernels);
        QDir().mkpath(workDir());
    }

    Value media(MediaType type, const QString &path) {
        const Result<MediaHandle> probed = MediaHandle::probe(type, path, hashes);
        if (!probed)
            qFatal("cannot probe %s", qPrintable(path));
        return Value(probed.value());
    }

    QString workDir() const { return dir.filePath("work"); }
    QString file(const QString &name) const { return dir.filePath(name); }
    const NodeType &type(const QString &id) const { return *registry.nodeType(id); }

    // The scheduler fills defaults in before a kernel sees the inputs (node_kernel.h).
    NodeContext context(const QString &id, Values inputs) {
        for (const InputSpec &spec : type(id).inputs)
            if (!inputs.contains(spec.name) && !spec.defaultValue.isUndefined())
                inputs.insert(spec.name, Value(spec.defaultValue));
        NodeContext context;
        context.node = QStringLiteral("t");
        context.run = QStringLiteral("20261121-120000-000-test");
        context.type = &type(id);
        context.inputs = inputs;
        context.workDir = workDir();
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        context.hashes = &hashes;
        return context;
    }

    Result<Values> load(const QString &id, const QString &path) { return kernels.node(id)->run(context(id, {{"file", path}})); }

    Result<JobRequest> request(const QString &id, const Values &inputs, const Selection &chosen) {
        return kernels.provider(id)->request(context(id, inputs), chosen);
    }

    Result<Values> outputs(const QString &id, const Values &inputs, const Selection &chosen, const JobResult &result) {
        return kernels.provider(id)->outputs(context(id, inputs), chosen, result);
    }

    QTemporaryDir dir;
    FileHashes hashes;
    Registry registry;
    Kernels kernels;
};

// This machine with a video model and a music model, as omarchy-local-ai leaves it, on one fake
// server: sd-server's vid_gen job API and ace-server's /lm and /synth. The shipped `fugue` finds
// it through $HOME.
class FakeMachine {
public:
    static constexpr const char *kKey = "local-key-0123456789";

    explicit FakeMachine(const QString &clip) {
        server.listen();
        const QString endpoint = QString::fromLatin1(server.origin()) + "/v1";
        const auto row = [&endpoint](const QString &recipe, const QString &kind) {
            return QJsonObject{{"recipe", recipe}, {"kind", kind}, {"edit", false}, {"name", recipe},
                               {"runtime", kind == "video" ? "sdcpp" : "acestep"}, {"unit", "active"},
                               {"endpoint", endpoint}, {"fits", true}, {"fit_reason", ""}, {"error", ""}};
        };
        writeFile(home("/.local/state/omarchy/agent-local/status.json"),
                  compact(QJsonObject{{"models", QJsonObject{{"wan", row("wan", "video")}, {"ace", row("ace", "music")}}},
                                      {"last", QJsonValue::Null}}));
        writeFile(home("/.config/omarchy/agent-local/key"), QByteArray(kKey) + "\n");

        server.on("GET", "/sdcpp/v1/capabilities", [](const HttpRequest &) {
            return HttpResponse{200, compact(QJsonObject{{"supported_modes", QJsonArray{"vid_gen"}},
                                                         {"limits", QJsonObject{}},
                                                         {"loras", QJsonArray{QJsonObject{{"path", "/m/turbo.safetensors"}}}}})};
        });
        const QByteArray webm = readFile(clip);
        server.on("POST", "/sdcpp/v1/vid_gen", [this, webm](const HttpRequest &) {
            const QJsonObject done{{"id", "v1"}, {"kind", "vid_gen"}, {"status", "completed"}, {"error", QJsonValue::Null},
                                   {"result", QJsonObject{{"output_format", "webm"}, {"fps", 12}, {"frame_count", 12},
                                                          {"b64_json", QString::fromLatin1(webm.toBase64())}}}};
            server.on("GET", "/sdcpp/v1/jobs/v1", [done](const HttpRequest &) { return HttpResponse{200, compact(done)}; });
            return HttpResponse{202, "{\"id\":\"v1\",\"kind\":\"vid_gen\",\"status\":\"queued\"}"};
        });

        server.on("GET", "/health", [](const HttpRequest &) { return HttpResponse{200, "{\"status\":\"ok\"}"}; });
        server.on("POST", "/lm", [](const HttpRequest &) { return HttpResponse{200, "{\"id\":\"1\"}"}; });
        server.on("GET", "/job?id=1", [](const HttpRequest &) { return HttpResponse{200, "{\"status\":\"done\"}"}; });
        server.on("GET", "/job?id=1&result=1", [](const HttpRequest &) {
            return HttpResponse{200, "[{\"caption\":\"x\",\"lyrics\":\"[Verse]\\nla\",\"seed\":7}]"};
        });
        server.on("POST", "/synth", [](const HttpRequest &) { return HttpResponse{200, "{\"id\":\"2\"}"}; });
        server.on("GET", "/job?id=2", [](const HttpRequest &) { return HttpResponse{200, "{\"status\":\"done\"}"}; });
        const QByteArray mp3 = readFile(makeAudio(file("song.mp3"), 2));
        server.on("GET", "/job?id=2&result=1", [mp3](const HttpRequest &) {
            return HttpResponse{200, "--BB\r\nContent-Type: audio/mpeg\r\n\r\n" + mp3 + "\r\n--BB\r\nContent-Type: x\r\n\r\nL\r\n--BB--\r\n",
                                "multipart/mixed; boundary=BB"};
        });
    }

    QString home(const QString &path = {}) const { return m_dir.filePath("home") + path; }
    QString file(const QString &name) const { return m_dir.filePath(name); }

    QJsonObject sent(const QByteArray &path) const {
        const auto posts = server.requestsTo("POST", path);
        return posts.isEmpty() ? QJsonObject() : QJsonDocument::fromJson(posts.last().body).object();
    }

    // The shipped binary with this machine as its home, waited for with the event loop running
    // so the fake server in this process can answer. Its output (JSON) is in `output`.
    int fugue(const QStringList &args, QByteArray *output = nullptr) const {
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
        while (process.state() != QProcess::NotRunning && clock.elapsed() < 90000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
            return -1;
        }
        const QByteArray out = process.readAllStandardOutput();
        if (output)
            *output = out;
        const int code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        if (code != 0)
            qWarning("fugue %s: %s %s", qPrintable(args.join(' ')), out.constData(), process.readAllStandardError().constData());
        return code;
    }

    FakeHttpServer server;

private:
    QTemporaryDir m_dir;
};

} // namespace

class TestMediaNodes : public QObject {
    Q_OBJECT

private slots:
    // --- Load Video, Load Audio ---

    void loadVideoGivesTheClipAndWhatItIs() {
        Bench bench;
        const QString clip = makeVideo(bench.file("clip.webm"), 128, 96, 1, 12);
        const Result<Values> out = bench.load("fugue/load-video", clip);
        QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().message));
        const MediaHandle video = out->value("video").media();
        QCOMPARE(video.type(), MediaType::Video);
        QCOMPARE(video.path(), clip);
        QCOMPARE(out->value("width").number(), 128.0);
        QCOMPARE(out->value("height").number(), 96.0);
        QCOMPARE(out->value("fps").number(), 12.0);
        QVERIFY(qAbs(out->value("duration").number() - 1.0) < 0.2);
    }

    void loadAudioGivesTheSoundAndWhatItIs() {
        Bench bench;
        const QString sound = makeAudio(bench.file("tone.wav"), 1.5);
        const Result<Values> out = bench.load("fugue/load-audio", sound);
        QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().message));
        QCOMPARE(out->value("audio").media().type(), MediaType::Audio);
        QCOMPARE(out->value("sample_rate").number(), 22050.0);
        QVERIFY(qAbs(out->value("duration").number() - 1.5) < 0.1);
    }

    void aMissingOrWrongFileIsNamed() {
        Bench bench;
        const Result<Values> missing = bench.load("fugue/load-video", bench.file("none.webm"));
        QVERIFY(!missing.ok());
        QCOMPARE(missing.error().code, QStringLiteral("file_not_found"));
        QVERIFY(missing.error().message.contains("video"));

        writeFile(bench.file("text.mp4"), "this is text");
        QCOMPARE(bench.load("fugue/load-video", bench.file("text.mp4")).error().code, QStringLiteral("media_unreadable"));
        // A picture is no audio, and a sound is no video.
        QCOMPARE(bench.load("fugue/load-audio", writePng(bench.file("p.png"), 8, 8)).error().code, QStringLiteral("media_unreadable"));
        QCOMPARE(bench.load("fugue/load-video", makeAudio(bench.file("a.wav"))).error().code, QStringLiteral("media_unreadable"));
    }

    // --- Generate Video: requirement and request ---

    void generateVideoAsksForTheCapabilityAndTheModelInput() {
        Bench bench;
        const auto kernel = bench.kernels.provider(kVideoNode);
        QVERIFY(kernel);
        const QJsonObject handle{{"resource", "fugue/video-model"}, {"name", "wan"}};
        const Requirement wanted = kernel->requirement(bench.type(kVideoNode), {{"prompt", "x"}, {"model", handle}});
        QCOMPARE(wanted.capability, QStringLiteral("video.generate"));
        QCOMPARE(wanted.model, handle);
        QVERIFY(kernel->requirement(bench.type(kVideoNode), {}).model.isEmpty());
        QCOMPARE(bench.kernels.provider(kSongNode)->requirement(bench.type(kSongNode), {}).capability, QStringLiteral("audio.generate"));
    }

    void aClipWithNothingSetIsTheModelsDefaultSizeAtTwoSeconds() {
        Bench bench;
        const Result<JobRequest> job = bench.request(kVideoNode, {{"prompt", "a fox"}}, videoModel({{"width", 832}, {"height", 480}, {"fps", 16}, {"steps", 4}}));
        QVERIFY2(job.ok(), qPrintable(job.ok() ? QString() : job.error().message));
        QCOMPARE(job->capability, QStringLiteral("video.generate"));
        QCOMPARE(job->params, (QJsonObject{{"prompt", "a fox"}, {"seed", 0}, {"width", 832}, {"height", 480}, {"fps", 16},
                                           {"video_frames", 32}, {"steps", 4}}));
        QVERIFY(job->inputs.isEmpty());
    }

    void aModelWithNoDefaultsGets864By480At24FramesASecond() {
        Bench bench;
        const Result<JobRequest> job = bench.request(kVideoNode, {{"prompt", "a fox"}, {"duration", 1}}, videoModel());
        QCOMPARE(job->params.value("width").toInt(), 864);
        QCOMPARE(job->params.value("height").toInt(), 480);
        QCOMPARE(job->params.value("fps").toInt(), 24);
        QCOMPARE(job->params.value("video_frames").toInt(), 24);
    }

    void whatTheNodeSetsBeatsTheDefaults() {
        Bench bench;
        const Result<JobRequest> job = bench.request(
            kVideoNode, {{"prompt", "a fox"}, {"negative_prompt", "blur"}, {"width", 640}, {"height", 352}, {"fps", 8}, {"duration", 3},
                         {"steps", 10}, {"guidance", 2.5}, {"seed", qint64(5)}},
            videoModel({{"width", 832}, {"height", 480}, {"fps", 16}, {"steps", 4}, {"guidance", 1}}));
        QCOMPARE(job->params, (QJsonObject{{"prompt", "a fox"}, {"negative_prompt", "blur"}, {"seed", 5}, {"width", 640},
                                           {"height", 352}, {"fps", 8}, {"video_frames", 24}, {"steps", 10}, {"guidance", 2.5}}));
    }

    // The first frame sets the clip's shape at about 0.4 megapixel in steps of 32 (Fantasi's
    // fitVideo), and goes along as a PNG of exactly that size.
    void aFirstFrameSetsTheShapeAndGoesAlongFitted() {
        Bench bench;
        const Value frame = bench.media(MediaType::Image, writePng(bench.file("photo.png"), 4000, 3000));
        const Result<JobRequest> job = bench.request(kVideoNode, {{"prompt", "a fox"}, {"image", frame}}, videoModel({{"width", 832}, {"height", 480}}));
        QVERIFY2(job.ok(), qPrintable(job.ok() ? QString() : job.error().message));
        QCOMPARE(job->params.value("width").toInt(), 736);
        QCOMPARE(job->params.value("height").toInt(), 576);
        const QString sent = job->inputs.value("image").toObject().value("path").toString();
        QVERIFY(sent.startsWith(bench.workDir()));
        QCOMPARE(sizeOf(sent), QSize(736, 576));

        // One side set keeps the frame's shape.
        const Result<JobRequest> wide = bench.request(kVideoNode, {{"prompt", "a fox"}, {"image", frame}, {"width", 640}}, videoModel());
        QCOMPARE(wide->params.value("width").toInt(), 640);
        QCOMPARE(wide->params.value("height").toInt(), 480);
        QCOMPARE(sizeOf(wide->inputs.value("image").toObject().value("path").toString()), QSize(640, 480));
    }

    void aSizeOutsideTheModelsLimitsIsBlamedOnTheInputThatSetIt() {
        Bench bench;
        const Selection limited = videoModel({}, {{"min_width", 256}, {"max_width", 1280}});
        const Result<JobRequest> refused = bench.request(kVideoNode, {{"prompt", "x"}, {"width", 1920}}, limited);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, QStringLiteral("value_out_of_range"));
        QCOMPARE(refused.error().input, QStringLiteral("width"));
        // The default size is the model's own, so it passes; the frame's shape is blamed on the frame.
        const Value frame = bench.media(MediaType::Image, writePng(bench.file("tall.png"), 100, 3000));
        const Result<JobRequest> sliver = bench.request(kVideoNode, {{"prompt", "x"}, {"image", frame}}, videoModel({}, {{"min_width", 256}}));
        QVERIFY(!sliver.ok());
        QCOMPARE(sliver.error().input, QStringLiteral("image"));
    }

    // --- Generate Video: outputs ---

    void theClipComesBackAsAFileWithItsOwnSizeRateAndLength() {
        Bench bench;
        const QString clip = makeVideo(bench.file("made.webm"), 160, 96, 1, 12);
        const JobResult result{QJsonObject{{"video", QJsonObject{{"path", clip}, {"format", "webm"}}}},
                               QJsonObject{{"seed", 77}, {"model", QJsonObject{{"resource", "fugue/video-model"}, {"name", "used"}}}}};
        const Result<Values> out = bench.outputs(kVideoNode, {{"prompt", "x"}, {"seed", 1}}, videoModel(), result);
        QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().message));
        const MediaHandle video = out->value("video").media();
        QCOMPARE(video.type(), MediaType::Video);
        QCOMPARE(video.path(), clip);
        QCOMPARE(out->value("seed").number(), 77.0);
        QCOMPARE(out->value("width").number(), 160.0);
        QCOMPARE(out->value("height").number(), 96.0);
        QCOMPARE(out->value("fps").number(), 12.0);
        QCOMPARE(out->value("model").json().toObject().value("name").toString(), QStringLiteral("used"));

        // A provider that says nothing of the seed or model: the ones asked for.
        const Result<Values> bare = bench.outputs(kVideoNode, {{"prompt", "x"}, {"seed", 1}}, videoModel(),
                                                  JobResult{QJsonObject{{"video", QJsonObject{{"path", clip}}}}, {}});
        QCOMPARE(bare->value("seed").number(), 1.0);
        QCOMPARE(bare->value("model").json().toObject().value("name").toString(), QStringLiteral("the-model"));
        QVERIFY(!bench.outputs(kVideoNode, {{"prompt", "x"}}, videoModel(), JobResult{}).ok());
    }

    // --- Generate Song ---

    void aSongAsksForTheDescriptionLyricsAndLength() {
        Bench bench;
        const Result<JobRequest> job = bench.request(kSongNode, {{"prompt", "  calm piano  "}, {"lyrics", "[Verse]\nla la\n"}, {"duration", 45},
                                                                 {"language", "pt-BR"}, {"seed", qint64(3)}}, musicModel());
        QVERIFY2(job.ok(), qPrintable(job.ok() ? QString() : job.error().message));
        QCOMPARE(job->capability, QStringLiteral("audio.generate"));
        QCOMPARE(job->params, (QJsonObject{{"prompt", "calm piano"}, {"lyrics", "[Verse]\nla la"}, {"duration", 45},
                                           {"language", "pt"}, {"seed", 3}}));
    }

    void instrumentalSingsNothingAndEmptyLyricsLeaveTheWordsToTheModel() {
        Bench bench;
        const Result<JobRequest> instrumental = bench.request(kSongNode, {{"prompt", "x"}, {"lyrics", "ignored"}, {"instrumental", true}}, musicModel());
        QCOMPARE(instrumental->params.value("lyrics").toString(), QStringLiteral("[Instrumental]"));
        QCOMPARE(bench.request(kSongNode, {{"prompt", "x"}}, musicModel())->params.value("lyrics").toString(), QString());
    }

    void aLengthOutsideTheModelsLimitsIsRefused() {
        Bench bench;
        const Result<JobRequest> refused = bench.request(kSongNode, {{"prompt", "x"}, {"duration", 700}},
                                                         musicModel({{"min_duration", 10}, {"max_duration", 600}}));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, QStringLiteral("value_out_of_range"));
        QCOMPARE(refused.error().input, QStringLiteral("duration"));
    }

    // Tempo, key, time signature, creativity and "keep my description" go along only when they are set:
    // an unset one is left for the model to work out.
    void aSongSendsTempoKeyAndTheRestOnlyWhenSet() {
        Bench bench;
        const Result<JobRequest> plain = bench.request(kSongNode, {{"prompt", "x"}, {"bpm", 0}, {"key", ""}, {"time_signature", ""},
                                                                   {"keep_description", false}}, musicModel());
        QVERIFY(plain.ok());
        for (const char *key : {"bpm", "key", "time_signature", "creativity", "keep_description"})
            QVERIFY2(!plain->params.contains(QLatin1String(key)), key);
        const Result<JobRequest> set = bench.request(kSongNode, {{"prompt", "x"}, {"bpm", 96}, {"key", " F# minor "}, {"time_signature", "3"},
                                                                 {"creativity", 0.4}, {"keep_description", true}}, musicModel());
        QVERIFY(set.ok());
        QCOMPARE(set->params.value("bpm").toInt(), 96);
        QCOMPARE(set->params.value("key").toString(), QStringLiteral("F# minor"));
        QCOMPARE(set->params.value("time_signature").toString(), QStringLiteral("3"));
        QCOMPARE(set->params.value("creativity").toDouble(), 0.4);
        QVERIFY(set->params.value("keep_description").toBool());
    }

    // Quality (#258) goes along only when it is not the default mp3, on all three song nodes.
    void aSongNodeSendsItsQualityOnlyWhenItIsNotThePlainMp3() {
        Bench bench;
        const Value song = bench.media(MediaType::Audio, makeAudio(bench.file("q.mp3"), 3));
        for (const QString &node : {kSongNode, kRemixNode, kRepaintNode}) {
            const auto make = [&](const QString &quality) {
                Values given{{"prompt", "x"}, {"quality", quality}};
                if (node != kSongNode)
                    given.insert(QStringLiteral("audio"), song);
                return bench.request(node, given, musicModel());
            };
            const Result<JobRequest> plain = make("mp3");
            QVERIFY2(plain.ok(), qPrintable(plain.ok() ? QString() : plain.error().message));
            QVERIFY(!plain->params.contains("quality"));
            QCOMPARE(make("lossless")->params.value("quality").toString(), QStringLiteral("lossless"));
            QCOMPARE(make("mp3-320")->params.value("quality").toString(), QStringLiteral("mp3-320"));
        }
    }

    // A style reference (#259) is the song's `ref_audio` input when one is wired, on all three song nodes; none, no input.
    void aSongNodePassesItsStyleReferenceOn() {
        Bench bench;
        const Value song = bench.media(MediaType::Audio, makeAudio(bench.file("s.mp3"), 3));
        const Value voice = bench.media(MediaType::Audio, makeAudio(bench.file("voice.mp3"), 2));
        for (const QString &node : {kSongNode, kRemixNode, kRepaintNode}) {
            const auto make = [&](const Value &reference) {
                Values given{{"prompt", "x"}};
                if (node != kSongNode)
                    given.insert(QStringLiteral("audio"), song);
                if (!reference.isEmpty())
                    given.insert(QStringLiteral("reference"), reference);
                return bench.request(node, given, musicModel());
            };
            const Result<JobRequest> without = make(Value());
            QVERIFY2(without.ok(), qPrintable(without.ok() ? QString() : without.error().message));
            QVERIFY(!without->inputs.contains("ref_audio"));
            const Result<JobRequest> with = make(voice);
            QVERIFY2(with.ok(), qPrintable(with.ok() ? QString() : with.error().message));
            QCOMPARE(with->inputs.value("ref_audio").toObject().value("path").toString(), voice.media().path());
        }
    }

    // An adapter (#260) is a music-adapter handle and its strength, sent together and only when one is picked; the nodes
    // declare audio.adapter so that the model picker lists the adapters.
    void aSongNodeSendsAnAdapterWithItsStrengthOnlyWhenOneIsPicked() {
        Bench bench;
        const Value song = bench.media(MediaType::Audio, makeAudio(bench.file("a.mp3"), 3));
        const Value adapter(QJsonObject{{"resource", "fugue/music-adapter"}, {"name", "singer-v2.safetensors"}});
        for (const QString &node : {kSongNode, kRemixNode, kRepaintNode}) {
            QVERIFY2(bench.type(node).capabilities.toArray().contains(QStringLiteral("audio.adapter")), qPrintable(node));
            const auto make = [&](const Value &picked) {
                Values given{{"prompt", "x"}, {"adapter_scale", 0.7}};
                if (node != kSongNode)
                    given.insert(QStringLiteral("audio"), song);
                if (!picked.isEmpty())
                    given.insert(QStringLiteral("adapter"), picked);
                return bench.request(node, given, musicModel());
            };
            const Result<JobRequest> without = make(Value());
            QVERIFY2(without.ok(), qPrintable(without.ok() ? QString() : without.error().message));
            QVERIFY(!without->params.contains("adapter") && !without->params.contains("adapter_scale"));
            const Result<JobRequest> with = make(adapter);
            QVERIFY2(with.ok(), qPrintable(with.ok() ? QString() : with.error().message));
            QCOMPARE(with->params.value("adapter").toString(), QStringLiteral("singer-v2.safetensors"));
            QCOMPARE(with->params.value("adapter_scale").toDouble(), 0.7);
        }
    }

    // --- Remix Song and Repaint Song ---

    void aRemixSendsTheSongAsACoverOrAStayCloseCover() {
        Bench bench;
        const Value song = bench.media(MediaType::Audio, makeAudio(bench.file("in.mp3"), 3));
        const Result<JobRequest> loose = bench.request(kRemixNode, {{"audio", song}, {"prompt", " jazz "}, {"strength", 0.6}}, musicModel());
        QVERIFY2(loose.ok(), qPrintable(loose.ok() ? QString() : loose.error().message));
        QCOMPARE(loose->capability, QStringLiteral("audio.generate"));
        QCOMPARE(loose->params.value("task").toString(), QStringLiteral("cover"));
        QCOMPARE(loose->params.value("prompt").toString(), QStringLiteral("jazz"));
        QCOMPARE(loose->params.value("strength").toDouble(), 0.6);
        QCOMPARE(loose->params.value("lyrics").toString(), QString());
        QCOMPARE(loose->inputs.value("audio").toObject().value("path").toString(), song.media().path());
        const Result<JobRequest> close = bench.request(kRemixNode, {{"audio", song}, {"prompt", "x"}, {"stay_close", true}, {"instrumental", true}}, musicModel());
        QCOMPARE(close->params.value("task").toString(), QStringLiteral("cover-nofsq"));
        QCOMPARE(close->params.value("lyrics").toString(), QStringLiteral("[Instrumental]"));
    }

    void aRepaintNamesTheStretchAndZeroEndsAtTheEndOfTheSong() {
        Bench bench;
        const Value song = bench.media(MediaType::Audio, makeAudio(bench.file("in.mp3"), 4));
        const Result<JobRequest> job = bench.request(kRepaintNode, {{"audio", song}, {"prompt", "guitar solo"}, {"start", 1.0}}, musicModel());
        QVERIFY2(job.ok(), qPrintable(job.ok() ? QString() : job.error().message));
        QCOMPARE(job->params.value("task").toString(), QStringLiteral("repaint"));
        QCOMPARE(job->params.value("start").toDouble(), 1.0);
        QVERIFY(qAbs(job->params.value("end").toDouble() - 4.0) < 0.2);
        const Result<JobRequest> backwards = bench.request(kRepaintNode, {{"audio", song}, {"prompt", "x"}, {"start", 3.0}, {"end", 2.0}}, musicModel());
        QVERIFY(!backwards.ok());
        QCOMPARE(backwards.error().input, QStringLiteral("end"));
    }

    void aRemixComesBackAsAFileWithTheSeedUsed() {
        Bench bench;
        const QString made = makeAudio(bench.file("made.mp3"), 2);
        const Result<Values> out = bench.outputs(kRemixNode, {{"prompt", "x"}, {"seed", 1}}, musicModel(),
                                                 JobResult{QJsonObject{{"audio", QJsonObject{{"path", made}, {"format", "mp3"}}}},
                                                           QJsonObject{{"seed", 8}}});
        QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().message));
        QCOMPARE(out->value("audio").media().type(), MediaType::Audio);
        QCOMPARE(out->value("seed").number(), 8.0);
    }

    void theSongComesBackAsAFileWithItsLyricsAndLength() {
        Bench bench;
        const QString song = makeAudio(bench.file("made.mp3"), 2);
        const Result<Values> out = bench.outputs(kSongNode, {{"prompt", "x"}, {"seed", 1}}, musicModel(),
                                                 JobResult{QJsonObject{{"audio", QJsonObject{{"path", song}, {"format", "mp3"}}}, {"lyrics", "[Verse]\nla"}},
                                                           QJsonObject{{"seed", 9}}});
        QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().message));
        QCOMPARE(out->value("audio").media().type(), MediaType::Audio);
        QCOMPARE(out->value("lyrics").text(), QStringLiteral("[Verse]\nla"));
        QCOMPARE(out->value("seed").number(), 9.0);
        QVERIFY(qAbs(out->value("duration").number() - 2.0) < 0.2);
    }

    // --- end to end: the shipped binary against a fake video and music server ---

    void aClipAndASongAreMadeLoadedAndPassedOn() {
        Bench bench;
        FakeMachine machine(makeVideo(bench.file("served.webm"), 128, 96, 1, 12));
        const auto node = [](const QString &type, const QJsonObject &inputs) {
            return QJsonObject{{"type", type}, {"version", "1.0.0"}, {"inputs", inputs}};
        };
        const auto from = [](const QString &port) { return QJsonObject{{"from", port}}; };
        const QJsonObject nodes{
            {"clip", node(kVideoNode, {{"prompt", "a fox"}, {"duration", 1}, {"seed", 4}})},
            {"loaded", node("fugue/load-video", {{"file", machine.home("/served.webm")}})},
            {"song", node(kSongNode, {{"prompt", "calm piano"}, {"duration", 20}})},
            {"words", node("fugue/preview", {{"value", from("song.lyrics")}})},
            {"shown", node("fugue/preview", {{"value", from("song.audio")}})},
            {"seen", node("fugue/preview", {{"value", from("clip.video")}})},
            {"loadedshown", node("fugue/preview", {{"value", from("loaded.video")}})}};
        writeFile(machine.file("flow.fugue"), compact(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}}));
        writeFile(machine.home("/served.webm"), readFile(bench.file("served.webm")));

        QByteArray output;
        QCOMPARE(machine.fugue({"run", machine.file("flow.fugue"), "--wait", "--json"}, &output), 0);
        const QJsonObject vid = machine.sent("/sdcpp/v1/vid_gen");
        QCOMPARE(vid.value("prompt").toString(), QStringLiteral("a fox"));
        QCOMPARE(vid.value("video_frames").toInt(), 24);
        QCOMPARE(vid.value("seed").toInt(), 4);
        QCOMPARE(vid.value("lora").toArray().size(), 1);
        const QJsonObject lm = machine.sent("/lm");
        QCOMPARE(lm.value("caption").toString(), QStringLiteral("calm piano"));
        QCOMPARE(lm.value("duration").toInt(), 20);
        QCOMPARE(machine.sent("/synth").value("lyrics").toString(), QStringLiteral("[Verse]\nla"));
        QVERIFY2(output.contains("video"), output.constData());
        QVERIFY2(output.contains("audio"), output.constData());
    }
};

QTEST_GUILESS_MAIN(TestMediaNodes)
#include "test_media_nodes.moc"
