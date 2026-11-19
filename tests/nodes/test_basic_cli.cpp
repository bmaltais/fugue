// The basic nodes through the real `fugue` binary (spec 13): a workflow that loads an
// image, saves it and previews a text is run with `fugue run --wait`, and its result is
// read with `fugue runs result`. No display. The file written is read back for its
// embedded workflow.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <vips/vips8>

#include "engine/embed.h"
#include "engine/file_hashes.h"
#include "engine/media.h"

using namespace fugue;

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QList<QJsonObject> lines() const {
        QList<QJsonObject> list;
        for (const QByteArray &line : out.trimmed().split('\n'))
            list.append(QJsonDocument::fromJson(line).object());
        return list;
    }
};

} // namespace

class TestBasicCli : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    Outcome fugue(const QStringList &args) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("DISPLAY");
        env.remove("WAYLAND_DISPLAY");
        env.insert("XDG_CACHE_HOME", path("cache"));
        env.insert("XDG_STATE_HOME", path("state"));
        env.insert("XDG_CONFIG_HOME", path("config"));
        env.insert("XDG_PICTURES_DIR", path("pictures"));
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(FUGUE_BINARY, args);
        Outcome outcome;
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            qWarning("fugue timed out: %s", qPrintable(args.join(' ')));
            return outcome;
        }
        outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    void write(const QString &file, const QByteArray &bytes) const {
        QDir().mkpath(QFileInfo(file).path());
        QFile handle(file);
        QVERIFY2(handle.open(QIODevice::WriteOnly), qPrintable(file));
        handle.write(bytes);
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void cleanupTestCase() {
        // The worker removes its queue entry just before it exits; let it go before the folders do.
        QTRY_VERIFY_WITH_TIMEOUT(QDir(path("state/fugue/queue")).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(), 10000);
    }

    void loadSaveAndPreviewRunFromTheCommandLine() {
        FileHashes hashes(path("hashes.json"));
        const vips::VImage pixels = (vips::VImage::black(12, 9, vips::VImage::option()->set("bands", 3)) + 120).cast(VIPS_FORMAT_UCHAR);
        const MediaHandle fixture = MediaHandle::fromImage(pixels).value().saved(path("in"), "fox", hashes).value();

        const QString out = path("out");
        const QString source = fixture.path();
        write(path("flow.fugue"),
              QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                         "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"") +
                  source.toUtf8() + "\"}},"
                  "\"words\": {\"type\": \"fugue/text\", \"version\": \"1.0.0\", \"inputs\": {\"text\": \"a red fox\"}},"
                  "\"look\": {\"type\": \"fugue/preview\", \"version\": \"1.0.0\", \"inputs\": {\"value\": {\"from\": \"words.text\"}}},"
                  "\"save\": {\"type\": \"fugue/save-image\", \"version\": \"1.0.0\", \"inputs\": {\"image\": {\"from\": \"load.image\"},"
                  " \"name\": \"fox\", \"folder\": \"" + out.toUtf8() + "\"}}"
                  "}}, \"layout\": {\"nodes\": {\"load\": {\"pos\": [5, 6]}}}}");

        const Outcome ran = fugue({"run", path("flow.fugue"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 0);
        const QList<QJsonObject> events = ran.lines();
        QCOMPARE(events.last().value("event").toString(), QString("run.done"));
        QCOMPARE(events.last().value("status").toString(), QString("ok"));
        const QString run = events.first().value("run").toString();
        QVERIFY(!run.isEmpty());

        // The result has the preview's value and the file that was saved.
        const Outcome result = fugue({"runs", "result", run, "--json"});
        QCOMPARE(result.exitCode, 0);
        const QJsonObject outputs = result.data().value("outputs").toObject();
        QCOMPARE(outputs.value("look").toObject().value("value").toString(), QString("a red fox"));
        const QJsonObject file = outputs.value("save").toObject().value("file").toObject();
        QCOMPARE(file.value("path").toString(), out + "/fox.png");
        QCOMPARE(file.value("width").toInt(), 12);
        QVERIFY(!outputs.contains("load")); // only the sinks

        // The saved file carries the workflow: layout kept, the local path gone.
        const Result<QJsonObject> embedded = readEmbeddedWorkflow(out + "/fox.png");
        QVERIFY2(embedded.ok(), qPrintable(embedded.error().message));
        QCOMPARE(embedded->value("layout").toObject().value("nodes").toObject().value("load").toObject().value("pos").toArray().at(0).toInt(), 5);
        const QJsonObject nodes = embedded->value("graph").toObject().value("nodes").toObject();
        QCOMPARE(nodes.value("load").toObject().value("inputs").toObject().value("file").toString(), QString("fox.png"));
        QVERIFY(embedded->contains("run"));

        // Running the same workflow again changes nothing on disk (the sink is cached) and the
        // preview still reports its value.
        const Outcome again = fugue({"run", path("flow.fugue"), "--wait", "--json"});
        QCOMPARE(again.exitCode, 0);
        const QString second = again.lines().first().value("run").toString();
        const Outcome secondResult = fugue({"runs", "result", second, "--json"});
        QCOMPARE(secondResult.data().value("outputs").toObject().value("look").toObject().value("value").toString(), QString("a red fox"));
        QVERIFY(!QFileInfo::exists(out + "/fox-2.png"));
    }

    // The pixel nodes hand Save Image float images (#15); they are saved at 16 bits in PNG and as
    // 8 bits in WebP.
    void aResizedImageIsSavedAsPngAndAsWebp() {
        FileHashes hashes(path("hashes2.json"));
        const vips::VImage pixels = (vips::VImage::black(10, 8, vips::VImage::option()->set("bands", 3)) + 200).cast(VIPS_FORMAT_UCHAR);
        const MediaHandle fixture = MediaHandle::fromImage(pixels).value().saved(path("in2"), "src", hashes).value();
        const QString out = path("out2");
        auto save = [&](const QString &id, const QString &format) {
            return QByteArray("\"") + id.toUtf8() + "\": {\"type\": \"fugue/save-image\", \"version\": \"1.0.0\", \"inputs\": {"
                   "\"image\": {\"from\": \"small.image\"}, \"name\": \"small\", \"format\": \"" + format.toUtf8() +
                   "\", \"folder\": \"" + out.toUtf8() + "\"}}";
        };
        write(path("resize.fugue"),
              QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                         "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"") + fixture.path().toUtf8() + "\"}},"
                         "\"small\": {\"type\": \"fugue/resize\", \"version\": \"1.0.0\", \"inputs\": {\"image\": {\"from\": \"load.image\"}, \"width\": 5, \"height\": 4}},"
                  + save("png", "png") + "," + save("webp", "webp") + "}}}");
        const Outcome ran = fugue({"run", path("resize.fugue"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 0);
        QCOMPARE(vips::VImage::new_from_file((out + "/small.png").toUtf8().constData()).width(), 5);
        QCOMPARE(vips::VImage::new_from_file((out + "/small.png").toUtf8().constData()).format(), VIPS_FORMAT_USHORT);
        const vips::VImage webp = vips::VImage::new_from_file((out + "/small.webp").toUtf8().constData());
        QCOMPARE(webp.height(), 4);
        QVERIFY(readEmbeddedWorkflow(out + "/small.webp").ok());
        QVERIFY(readEmbeddedWorkflow(out + "/small.png").ok());
    }

    // A compiled graph holds no workflow to embed: the image is saved, the run is a success, and
    // the warning says why the file holds no workflow.
    void aRunFromACompiledGraphSavesAndWarns() {
        FileHashes hashes(path("hashes3.json"));
        const vips::VImage pixels = (vips::VImage::black(7, 5, vips::VImage::option()->set("bands", 3)) + 60).cast(VIPS_FORMAT_UCHAR);
        const MediaHandle fixture = MediaHandle::fromImage(pixels).value().saved(path("in3"), "src", hashes).value();
        const QString out = path("out3");
        write(path("tocompile.fugue"),
              QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                         "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"") +
                  fixture.path().toUtf8() + "\"}},"
                  "\"save\": {\"type\": \"fugue/save-image\", \"version\": \"1.0.0\", \"inputs\": {\"image\": {\"from\": \"load.image\"},"
                  " \"name\": \"compiled\", \"folder\": \"" + out.toUtf8() + "\"}}}}}");
        const Outcome compiled = fugue({"compile", path("tocompile.fugue")});
        QCOMPARE(compiled.exitCode, 0);
        write(path("compiled.json"), compiled.out);

        const Outcome ran = fugue({"run", path("compiled.json"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 0);
        const QJsonObject done = ran.lines().last();
        QCOMPARE(done.value("status").toString(), QString("ok"));
        const QJsonArray errors = done.value("errors").toArray();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.at(0).toObject().value("code").toString(), QString("workflow_not_embedded"));
        QCOMPARE(errors.at(0).toObject().value("severity").toString(), QString("warning"));
        QCOMPARE(errors.at(0).toObject().value("node").toString(), QString("save"));
        QVERIFY(QFileInfo::exists(out + "/compiled.png"));

        const QString run = ran.lines().first().value("run").toString();
        for (const char *command : {"result", "status"}) {
            const Outcome asked = fugue({"runs", command, run, "--json"});
            QVERIFY2(asked.exitCode == 0, asked.out.constData());
            QVERIFY2(asked.json().value("ok").toBool(), asked.out.constData());
        }
    }

    // Number gives what was typed in it, and a Preview of it shows that in the run result.
    void aNumberRunsFromTheCommandLine() {
        write(path("number.fugue"),
              "{\"fugue\": 1, \"graph\": {\"nodes\": {"
              "\"n\": {\"type\": \"fugue/number\", \"version\": \"1.0.0\", \"inputs\": {\"value\": 2.5}},"
              "\"look\": {\"type\": \"fugue/preview\", \"version\": \"1.0.0\", \"inputs\": {\"value\": {\"from\": \"n.value\"}}}}}}");
        const Outcome ran = fugue({"run", path("number.fugue"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 0);
        const QList<QJsonObject> events = ran.lines();
        QCOMPARE(events.last().value("status").toString(), QString("ok"));
        for (const QJsonObject &event : events)
            if (event.value("event").toString() == "node.done" && event.value("node").toString() == "n")
                QCOMPARE(event.value("outputs").toObject().value("value").toDouble(), 2.5);
        const Outcome result = fugue({"runs", "result", events.first().value("run").toString(), "--json"});
        QCOMPARE(result.exitCode, 0);
        QCOMPARE(result.data().value("outputs").toObject().value("look").toObject().value("value").toDouble(), 2.5);

        // --set changes it for one run.
        const Outcome set = fugue({"run", path("number.fugue"), "--set", "n.value=-4", "--wait", "--json"});
        QCOMPARE(set.exitCode, 0);
        const Outcome setResult = fugue({"runs", "result", set.lines().first().value("run").toString(), "--json"});
        QCOMPARE(setResult.data().value("outputs").toObject().value("look").toObject().value("value").toDouble(), -4.0);
    }

    void aMissingImageIsReportedByName() {
        write(path("missing.fugue"),
              "{\"fugue\": 1, \"graph\": {\"nodes\": {"
              "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"/nonexistent/none.png\"}},"
              "\"look\": {\"type\": \"fugue/preview\", \"version\": \"1.0.0\", \"inputs\": {\"value\": {\"from\": \"load.width\"}}}}}}");
        const Outcome ran = fugue({"run", path("missing.fugue"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 1);
        QVERIFY(ran.out.contains("file_not_found"));
        QVERIFY(ran.out.contains("/nonexistent/none.png"));
    }
};

QTEST_GUILESS_MAIN(TestBasicCli)
#include "test_basic_cli.moc"
