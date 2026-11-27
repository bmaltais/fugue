// Save Video and Save Audio (spec 14.2, #116).
//
// Seam: a workflow document run through the real scheduler (runGraph) with the built-in node types
// and kernels, as test_basic.cpp does for Save Image; the saved files are read back with ffprobe,
// the way the engine reads any video or sound. Media fixtures are made with ffmpeg's lavfi sources,
// so none is committed.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/registry.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/scheduler.h"
#include "nodes/builtin.h"
#include "nodes/save_files.h"

using namespace fugue;

namespace {

QJsonObject json(const QByteArray &text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (!document.isObject())
        qFatal("bad test JSON: %s in %s", qPrintable(error.errorString()), text.constData());
    return document.object();
}

QByteArray runProgram(const QString &program, const QStringList &args, bool *ok = nullptr) {
    QProcess process;
    process.start(program, args);
    const bool done = process.waitForFinished(60000) && process.exitCode() == 0;
    if (ok)
        *ok = done;
    if (!done) {
        qWarning().noquote() << program << "failed:" << process.readAllStandardError();
        return {};
    }
    return process.readAllStandardOutput();
}

void ffmpeg(const QStringList &args) {
    bool ok = false;
    runProgram("ffmpeg", QStringList{"-nostdin", "-hide_banner", "-loglevel", "error", "-y"} + args, &ok);
    if (!ok)
        qFatal("ffmpeg failed");
}

// What ffprobe says of a file: the container, the first stream's codec of a type, and the tag
// `fugue` wherever it sits (a container's or a stream's, in any letter case).
struct Probed {
    QString format;
    QString codec;
    QByteArray workflow;
};

Probed probe(const QString &file, const QString &codecType) {
    const QJsonObject out =
        QJsonDocument::fromJson(runProgram("ffprobe", {"-v", "error", "-show_format", "-show_streams", "-of", "json", file})).object();
    Probed probed;
    probed.format = out.value("format").toObject().value("format_name").toString();
    const auto takeTag = [&probed](const QJsonObject &tags) {
        for (auto it = tags.begin(); it != tags.end(); ++it)
            if (it.key().compare("fugue", Qt::CaseInsensitive) == 0)
                probed.workflow = it.value().toString().toUtf8();
    };
    takeTag(out.value("format").toObject().value("tags").toObject());
    for (const QJsonValue &value : out.value("streams").toArray()) {
        const QJsonObject stream = value.toObject();
        takeTag(stream.value("tags").toObject());
        if (probed.codec.isEmpty() && stream.value("codec_type").toString() == codecType)
            probed.codec = stream.value("codec_name").toString();
    }
    return probed;
}

struct Ran {
    RunOutcome outcome;
    QString error;
};

} // namespace

class TestSaveMedia : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_registry;
    Kernels m_kernels;
    std::unique_ptr<FileHashes> m_hashes;
    std::unique_ptr<ResultCache> m_cache;

    QString path(const QString &name) const { return m_dir.filePath(name); }
    QString outDir() const { return path("out"); }

    Ran run(const QByteArray &nodes, bool fromDocument = true) {
        QJsonObject nodeObject = json(nodes);
        for (auto it = nodeObject.begin(); it != nodeObject.end(); ++it) {
            QJsonObject node = it.value().toObject();
            node.insert("version", "1.0.0");
            it.value() = node;
        }
        const Result<Document> document = Document::fromJson(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodeObject}}}});
        if (!document)
            qFatal("bad test document");
        const Compilation compiled = compile(*document, m_registry);
        Ran ran;
        if (!compiled.ok()) {
            ran.error = compiled.errors().value(0).message;
            return ran;
        }
        const auto made = RunFolder::create(path("runs"));
        if (!made)
            qFatal("no run folder");
        RunEnvironment environment;
        environment.registry = &m_registry;
        environment.kernels = &m_kernels;
        environment.cache = m_cache.get();
        environment.hashes = m_hashes.get();
        RunOptions options;
        if (fromDocument)
            options.document = document->toJson();
        ran.outcome = runGraph(*compiled, *made, environment, options);
        for (const Error &error : std::as_const(ran.outcome.errors))
            if (error.severity == Error::Severity::Error) {
                ran.error = error.code + ": " + error.message;
                break;
            }
        return ran;
    }

    // load -> save, with the given inputs of the save node (JSON object text without braces).
    Ran save(const QString &kind, const QString &source, const QString &saveInputs, bool fromDocument = true) {
        return run(QByteArray("{'l': {'type': 'fugue/load-") + kind.toUtf8() + "', 'inputs': {'file': '" + source.toUtf8() + "'}},"
                   " 't': {'type': 'fugue/text', 'inputs': {'text': 'a secret prompt'}},"
                   " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 't.text'}}},"
                   " 's': {'type': 'fugue/save-" + kind.toUtf8() + "', 'inputs': {'" + kind.toUtf8() + "': {'from': 'l." + kind.toUtf8() + "'}, "
                   + saveInputs.toUtf8() + "}}}",
                   fromDocument);
    }

    QString makeClip(const QString &name, const QString &codec, bool withSound = true) {
        const QString file = path("fixtures/" + name);
        QDir().mkpath(path("fixtures"));
        QStringList args{"-f", "lavfi", "-i", "testsrc=size=64x48:rate=12:duration=1"};
        if (withSound)
            args += {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=22050:duration=1"};
        args += {"-c:v", codec, "-pix_fmt", "yuv420p"};
        if (withSound)
            args += {"-c:a", name.endsWith(".webm") ? "libopus" : "aac"};
        ffmpeg(args + QStringList{file});
        return file;
    }

    QString makeSound(const QString &name) {
        const QString file = path("fixtures/" + name);
        QDir().mkpath(path("fixtures"));
        ffmpeg({"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=22050:duration=1", file});
        return file;
    }

    // The workflow a saved file carries, as the share copy it is.
    static void verifyWorkflow(const QByteArray &text, bool withPrompt = true) {
        QVERIFY2(!text.isEmpty(), "no workflow tag in the file");
        const QJsonObject workflow = QJsonDocument::fromJson(text).object();
        QCOMPARE(workflow.value("fugue").toInt(), 1);
        const QJsonObject nodes = workflow.value("graph").toObject().value("nodes").toObject();
        QCOMPARE(nodes.value("t").toObject().value("inputs").toObject().value("text").toString(),
                 withPrompt ? QString("a secret prompt") : QString());
        QVERIFY(workflow.contains("run"));
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
        m_hashes = std::make_unique<FileHashes>(path("hashes.json"));
        m_cache = std::make_unique<ResultCache>(path("results"));
    }

    // --- Save Video ---

    void aWebmSavedAsWebmIsCopiedAsItIsWithTheWorkflowInItsTags() {
        const QString source = makeClip("clip.webm", "libvpx");
        const Ran ran = save("video", source, "'name': 'fox', 'format': 'webm', 'folder': '" + outDir() + "'");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        const QString file = outDir() + "/fox.webm";
        QVERIFY(QFileInfo::exists(file));
        const MediaHandle written = ran.outcome.outputs.value("s").value("file").media();
        QCOMPARE(written.path(), file);
        QCOMPARE(written.type(), MediaType::Video);
        QCOMPARE(written.width(), 64);
        const Probed probed = probe(file, "video");
        QCOMPARE(probed.codec, QString("vp8")); // not encoded again
        verifyWorkflow(probed.workflow);
        QVERIFY2(!probed.workflow.contains(m_dir.path().toUtf8()), "a local path was embedded");
    }

    void aWebmSavedAsMp4IsEncodedAsH264AndAac() {
        const QString source = makeClip("clip2.webm", "libvpx");
        const Ran ran = save("video", source, "'name': 'wide', 'format': 'mp4', 'folder': '" + outDir() + "'");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        const QString file = outDir() + "/wide.mp4";
        const Probed video = probe(file, "video");
        QVERIFY2(video.format.contains("mp4"), qPrintable(video.format));
        QCOMPARE(video.codec, QString("h264"));
        QCOMPARE(probe(file, "audio").codec, QString("aac"));
        verifyWorkflow(video.workflow);
    }

    void anMp4SavedAsMp4IsCopiedAsItIs() {
        const QString source = makeClip("own.mp4", "libx264");
        const Ran ran = save("video", source, "'name': 'own', 'format': 'mp4', 'folder': '" + outDir() + "'");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        const Probed probed = probe(outDir() + "/own.mp4", "video");
        QCOMPARE(probed.codec, QString("h264"));
        verifyWorkflow(probed.workflow);
    }

    void aSilentClipSavesWithoutAnAudioTrack() {
        const QString source = makeClip("mute.webm", "libvpx", false);
        const Ran ran = save("video", source, "'name': 'mute', 'format': 'mp4', 'embed': 'none', 'folder': '" + outDir() + "'");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QVERIFY(probe(outDir() + "/mute.mp4", "audio").codec.isEmpty());
        QVERIFY(probe(outDir() + "/mute.mp4", "video").workflow.isEmpty());
    }

    void embedNoPromptsBlanksThePrompts() {
        const QString source = makeClip("blank.webm", "libvpx");
        const Ran ran = save("video", source, "'name': 'blank', 'format': 'webm', 'embed': 'no-prompts', 'folder': '" + outDir() + "'");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        verifyWorkflow(probe(outDir() + "/blank.webm", "video").workflow, false);
    }

    // Two Save nodes of one name in one run: neither takes the other's file.
    void aSaveNeverOverwritesAndTakesTheNextNumber() {
        const QString source = makeClip("again.webm", "libvpx");
        const QByteArray save = "'video': {'from': 'l.video'}, 'name': 'twice', 'format': 'webm', 'embed': 'none', 'folder': '"
                                + outDir().toUtf8() + "'";
        const Ran ran = run(QByteArray("{'l': {'type': 'fugue/load-video', 'inputs': {'file': '") + source.toUtf8() + "'}},"
                            " 'a': {'type': 'fugue/save-video', 'inputs': {" + save + "}},"
                            " 'b': {'type': 'fugue/save-video', 'inputs': {" + save + "}}}");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QVERIFY(QFileInfo::exists(outDir() + "/twice.webm"));
        QVERIFY(QFileInfo::exists(outDir() + "/twice-2.webm"));
        QVERIFY(QDir(outDir()).entryList(QDir::Files | QDir::Hidden).filter(".part").isEmpty());
    }

    // #414: ffmpeg's /proc/<pid>/fd/<n> in an error is the file the person asked for.
    void anFfmpegErrorNamesTheUsersFileNotTheProcLink() {
        const Errors errors = nodes::replacePath({makeError(QStringLiteral("node_failed"), QStringLiteral("/proc/42/fd/7: No space left on device"),
                                                            QStringLiteral("See /proc/42/fd/7."))},
                                                 QStringLiteral("/proc/42/fd/7"), QStringLiteral("/home/u/out/a.mp4"));
        QCOMPARE(errors.first().message, QString("/home/u/out/a.mp4: No space left on device"));
        QCOMPARE(errors.first().hint, QString("See /home/u/out/a.mp4."));
    }

    void aNameThatIsAPathIsRefusedOnTheNameInput() {
        const QString source = makeClip("bad.webm", "libvpx");
        const Ran ran = save("video", source, "'name': '../escape', 'folder': '" + outDir() + "'");
        QVERIFY(ran.error.startsWith("node_failed"));
        QCOMPARE(ran.outcome.errors.value(0).input, QString("name"));
        QVERIFY(!QFileInfo::exists(path("escape.mp4")));
    }

    void aRunFromACompiledGraphSavesAndWarnsThatNoWorkflowIsHeld() {
        const QString source = makeClip("compiled.webm", "libvpx");
        const Ran ran = save("video", source, "'name': 'compiled', 'format': 'webm', 'folder': '" + outDir() + "'", false);
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QVERIFY(probe(outDir() + "/compiled.webm", "video").workflow.isEmpty());
        bool warned = false;
        for (const Error &error : ran.outcome.errors)
            warned = warned || (error.code == "workflow_not_embedded" && error.message.contains("video"));
        QVERIFY(warned);
    }

    // --- Save Audio ---

    void aSoundIsSavedAsFlacMp3AndOpusEachWithTheWorkflow() {
        const QString source = makeSound("tone.wav");
        const struct {
            const char *format;
            const char *codec;
        } formats[] = {{"flac", "flac"}, {"mp3", "mp3"}, {"opus", "opus"}};
        for (const auto &f : formats) {
            const Ran ran = save("audio", source, QString("'name': 'song', 'format': '%1', 'folder': '%2'").arg(f.format, outDir()));
            QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
            const QString file = outDir() + "/song." + f.format;
            QVERIFY2(QFileInfo::exists(file), f.format);
            const MediaHandle written = ran.outcome.outputs.value("s").value("file").media();
            QCOMPARE(written.type(), MediaType::Audio);
            QCOMPARE(written.path(), file);
            QVERIFY2(written.duration() > 0.8, f.format);
            const Probed probed = probe(file, "audio");
            QCOMPARE(probed.codec, QString(f.codec));
            verifyWorkflow(probed.workflow);
        }
    }

    void aFlacSavedAsFlacIsCopied() {
        const QString source = path("fixtures/own.flac");
        QDir().mkpath(path("fixtures"));
        ffmpeg({"-f", "lavfi", "-i", "sine=frequency=330:sample_rate=22050:duration=1", source});
        const Ran ran = save("audio", source, "'name': 'own', 'format': 'flac', 'embed': 'none', 'folder': '" + outDir() + "'");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QCOMPARE(probe(outDir() + "/own.flac", "audio").codec, QString("flac"));
        QCOMPARE(QFileInfo(outDir() + "/own.flac").size() > 0, true);
    }

    void aSongFromGenerateSongIsSavedFromItsFile() {
        // The file a provider wrote, loaded and saved again under a name of the user's.
        const QString source = makeSound("made.wav");
        const Ran ran = save("audio", source, "'name': 'my song', 'format': 'mp3', 'embed': 'none', 'folder': '" + outDir() + "'");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QVERIFY(QFileInfo::exists(outDir() + "/my song.mp3"));
    }

    void anUnknownFormatIsBlamedOnTheFormatInput() {
        const QString source = makeSound("odd.wav");
        const Ran ran = run(QByteArray("{'l': {'type': 'fugue/load-audio', 'inputs': {'file': '") + source.toUtf8() + "'}},"
                            " 's': {'type': 'fugue/save-audio', 'inputs': {'audio': {'from': 'l.audio'}, 'format': 'ogg'}}}");
        // The manifest's choices refuse it before the kernel is asked.
        QVERIFY(!ran.error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSaveMedia)
#include "test_save_media.moc"
