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
#include <QScopeGuard>
#include <QtTest>

#include <vips/vips8>

#include "../test_paths.h"
#include "engine/embed.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "../test_shard.h"

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

    // The binary with the temporary folder as its home and its working directory, so the
    // fixtures there are inside the default file roots (13.6): read under home, write under
    // the working directory, the output folder and ~/Documents/Fugue.
    Outcome fugue(const QStringList &args, const QString &workingDirectory = {}) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", m_dir.path());
        env.remove("QT_QPA_PLATFORM");
        env.remove("DISPLAY");
        env.remove("WAYLAND_DISPLAY");
        env.insert("XDG_CACHE_HOME", path("cache"));
        env.insert("XDG_STATE_HOME", path("state"));
        env.insert("XDG_CONFIG_HOME", path("config"));
        env.insert("XDG_PICTURES_DIR", path("pictures"));
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(workingDirectory.isEmpty() ? m_dir.path() : workingDirectory);
        process.start(fugueBinary(), args);
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

    // A Dynamic Prompt upstream of a Save Image: its picked text is in the embedded run section, with the template and
    // the concrete seed in the workflow, and left out when the Save node embeds without prompts (#556).
    void thePickedPromptIsEmbeddedWithTheImage() {
        FileHashes hashes(path("hashes3.json"));
        const vips::VImage pixels = (vips::VImage::black(40, 30, vips::VImage::option()->set("bands", 3)) + 90).cast(VIPS_FORMAT_UCHAR);
        const MediaHandle fixture = MediaHandle::fromImage(pixels).value().saved(path("in3"), "base", hashes).value();
        // The binary shares this process's scratch data folder (tests/test_environment.cpp).
        const QString wildcard = qEnvironmentVariable("XDG_DATA_HOME") + "/fugue/wildcards/animal.txt";
        write(wildcard, "# animals\nfox\n");
        const auto removeWildcard = qScopeGuard([&] { QFile::remove(wildcard); });
        const QString out = path("out3");
        const auto save = [&](const QString &id, const QString &embed) {
            return QByteArray("\"") + id.toUtf8() + "\": {\"type\": \"fugue/save-image\", \"version\": \"1.0.0\", \"inputs\": {"
                   "\"image\": {\"from\": \"label.image\"}, \"name\": \"" + id.toUtf8() + "\", \"embed\": \"" + embed.toUtf8() +
                   "\", \"folder\": \"" + out.toUtf8() + "\"}}";
        };
        write(path("dyn.fugue"),
              QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                         "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"") + fixture.path().toUtf8() + "\"}},"
                  "\"dyn\": {\"type\": \"fugue/dynamic-prompt\", \"version\": \"1.0.0\", \"inputs\": {\"template\": \"a __animal__ {big|big}\", \"seed\": {\"auto\": \"random\"}}},"
                  "\"label\": {\"type\": \"fugue/draw-text\", \"version\": \"1.0.0\", \"inputs\": {\"image\": {\"from\": \"load.image\"}, \"text\": {\"from\": \"dyn.text\"}}},"
                  + save("full", "full") + "," + save("blank", "no-prompts") + "}}}");
        const Outcome ran = fugue({"run", path("dyn.fugue"), "--wait", "--json"});
        QVERIFY2(ran.exitCode == 0, qPrintable(ran.out + ran.err));
        const Result<QJsonObject> full = readEmbeddedWorkflow(out + "/full.png");
        QVERIFY2(full.ok(), qPrintable(full.error().message));
        const QJsonObject entry = full->value("run").toObject().value("nodes").toObject().value("dyn").toObject();
        QCOMPARE(entry.value("prompt").toString(), QString("a fox big"));
        QVERIFY(entry.value("auto").toObject().value("seed").isDouble()); // the seed that was used
        QCOMPARE(full->value("graph").toObject().value("nodes").toObject().value("dyn").toObject().value("inputs").toObject().value("template").toString(),
                 QString("a __animal__ {big|big}"));
        const Result<QJsonObject> blank = readEmbeddedWorkflow(out + "/blank.png");
        QVERIFY2(blank.ok(), qPrintable(blank.error().message));
        QVERIFY(!blank->value("run").toObject().value("nodes").toObject().value("dyn").toObject().contains("prompt"));
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

    // A value that arrives by link meets the same input rules as one typed in: a Number below
    // Crop's minimum fails that node on that input, instead of reading outside the image.
    void aLinkedNumberOutsideTheInputsRangeFailsTheNode_data() {
        QTest::addColumn<double>("x");
        QTest::addRow("just below") << -3.0;
        QTest::addRow("far below") << -100000.0;
        QTest::addRow("not whole") << 1.5;
    }
    void aLinkedNumberOutsideTheInputsRangeFailsTheNode() {
        QFETCH(double, x);
        FileHashes hashes(path("hashes-r1.json"));
        const vips::VImage pixels = (vips::VImage::black(10, 8, vips::VImage::option()->set("bands", 3)) + 30).cast(VIPS_FORMAT_UCHAR);
        const MediaHandle fixture = MediaHandle::fromImage(pixels).value().saved(path("in-r1"), "src", hashes).value();
        write(path("crop.fugue"),
              QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                         "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"") +
                  fixture.path().toUtf8() + "\"}},"
                  "\"n\": {\"type\": \"fugue/number\", \"version\": \"1.0.0\", \"inputs\": {\"value\": " + QByteArray::number(x) + "}},"
                  "\"crop\": {\"type\": \"fugue/crop\", \"version\": \"1.0.0\", \"inputs\": {\"image\": {\"from\": \"load.image\"},"
                  " \"x\": {\"from\": \"n.value\"}, \"y\": 0, \"width\": 4, \"height\": 4}},"
                  "\"look\": {\"type\": \"fugue/preview\", \"version\": \"1.0.0\", \"inputs\": {\"value\": {\"from\": \"crop.image\"}}}}}}");
        const Outcome ran = fugue({"run", path("crop.fugue"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 1);
        const QJsonObject done = ran.lines().last();
        QCOMPARE(done.value("status").toString(), QString("failed"));
        const QJsonObject error = done.value("errors").toArray().at(0).toObject();
        QCOMPARE(error.value("code").toString(), QString("value_out_of_range"));
        QCOMPARE(error.value("node").toString(), QString("crop"));
        QCOMPARE(error.value("input").toString(), QString("x"));
    }

    // A linked Text that is not one of the input's choices fails the node: Save Image never
    // writes PNG bytes under a .jpg name.
    void aLinkedTextOutsideTheChoicesFailsTheNode() {
        FileHashes hashes(path("hashes-r1b.json"));
        const vips::VImage pixels = (vips::VImage::black(6, 6, vips::VImage::option()->set("bands", 3)) + 40).cast(VIPS_FORMAT_UCHAR);
        const MediaHandle fixture = MediaHandle::fromImage(pixels).value().saved(path("in-r1b"), "src", hashes).value();
        const QString out = path("out-r1b");
        write(path("format.fugue"),
              QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                         "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"") +
                  fixture.path().toUtf8() + "\"}},"
                  "\"f\": {\"type\": \"fugue/text\", \"version\": \"1.0.0\", \"inputs\": {\"text\": \"jpg\"}},"
                  "\"save\": {\"type\": \"fugue/save-image\", \"version\": \"1.0.0\", \"inputs\": {\"image\": {\"from\": \"load.image\"},"
                  " \"name\": \"shot\", \"format\": {\"from\": \"f.text\"}, \"folder\": \"" + out.toUtf8() + "\"}}}}}");
        const Outcome ran = fugue({"run", path("format.fugue"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 1);
        const QJsonObject error = ran.lines().last().value("errors").toArray().at(0).toObject();
        QCOMPARE(error.value("code").toString(), QString("value_not_in_choices"));
        QCOMPARE(error.value("node").toString(), QString("save"));
        QCOMPARE(error.value("input").toString(), QString("format"));
        QVERIFY(!QFileInfo::exists(out + "/shot.jpg"));
    }

    // --- file roots (13.6) ---

    // A load -> save workflow at `file`; the save writes `name` into `folder`.
    QString rootsFlow(const QString &file, const QString &source, const QString &folder, const QString &name) {
        write(file, QByteArray("{\"fugue\": 1, \"graph\": {\"nodes\": {"
                               "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"") +
                        source.toUtf8() + "\"}},"
                        "\"save\": {\"type\": \"fugue/save-image\", \"version\": \"1.0.0\", \"inputs\": {\"image\": {\"from\": \"load.image\"},"
                        " \"name\": \"" + name.toUtf8() + "\", \"embed\": \"none\", \"folder\": \"" + folder.toUtf8() + "\"}}}}}");
        return file;
    }

    QString rootsSource() {
        FileHashes hashes(path("hashes-roots.json"));
        const vips::VImage pixels = (vips::VImage::black(5, 4, vips::VImage::option()->set("bands", 3)) + 70).cast(VIPS_FORMAT_UCHAR);
        return MediaHandle::fromImage(pixels).value().saved(path("in-roots"), "src", hashes).value().path();
    }

    static QJsonObject firstError(const Outcome &ran) {
        return ran.lines().last().value("errors").toArray().at(0).toObject();
    }

    // Writing under the directory `fugue run` was started in works.
    void aSaveUnderTheWorkingDirectoryIsWritten() {
        const QString flow = rootsFlow(path("roots-ok.fugue"), rootsSource(), path("work/out"), "inside");
        QVERIFY(QDir().mkpath(path("work")));
        const Outcome ran = fugue({"run", flow, "--wait", "--json"}, path("work"));
        QVERIFY2(ran.exitCode == 0, ran.out.constData());
        QVERIFY(QFileInfo::exists(path("work/out/inside.png")));
    }

    // Outside the write roots is permission_denied on the folder, naming the roots; nothing
    // is written.
    void aSaveOutsideTheWriteRootsIsRefused() {
        QTemporaryDir elsewhere;
        QVERIFY(elsewhere.isValid());
        const QString flow = rootsFlow(path("roots-out.fugue"), rootsSource(), elsewhere.filePath("out"), "outside");
        QVERIFY(QDir().mkpath(path("work2")));
        const Outcome ran = fugue({"run", flow, "--wait", "--json"}, path("work2"));
        QCOMPARE(ran.exitCode, 1);
        const QJsonObject error = firstError(ran);
        QCOMPARE(error.value("code").toString(), QString("permission_denied"));
        QCOMPARE(error.value("node").toString(), QString("save"));
        QCOMPARE(error.value("input").toString(), QString("folder"));
        QVERIFY2(error.value("hint").toString().contains(path("work2")), qPrintable(error.value("hint").toString()));
        QVERIFY(!QFileInfo::exists(elsewhere.filePath("out")));
    }

    // `..` and a symbolic link cannot lead out of the roots.
    void dotDotAndSymlinksCannotEscapeTheRoots() {
        QTemporaryDir elsewhere;
        QVERIFY(elsewhere.isValid());
        QVERIFY(QDir().mkpath(path("work3")));
        const QString source = rootsSource();
        const Outcome up = fugue({"run", rootsFlow(path("roots-up.fugue"), source, path("work3") + "/../../" +
                                                       QFileInfo(elsewhere.path()).fileName(), "up"),
                                  "--wait", "--json"},
                                 path("work3"));
        QCOMPARE(up.exitCode, 1);
        QCOMPARE(firstError(up).value("code").toString(), QString("permission_denied"));
        QVERIFY(QFile::link(elsewhere.path(), path("work3/link")));
        const Outcome linked = fugue({"run", rootsFlow(path("roots-link.fugue"), source, path("work3/link/sub"), "linked"),
                                      "--wait", "--json"},
                                     path("work3"));
        QCOMPARE(linked.exitCode, 1);
        QCOMPARE(firstError(linked).value("code").toString(), QString("permission_denied"));
        QVERIFY(QDir(elsewhere.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
        // A relative folder stays under the output folder.
        const Outcome relative = fugue({"run", rootsFlow(path("roots-rel.fugue"), source, "../../escape", "rel"),
                                        "--wait", "--json"},
                                       path("work3"));
        QCOMPARE(relative.exitCode, 1);
        QCOMPARE(firstError(relative).value("code").toString(), QString("permission_denied"));
        QVERIFY(!QFileInfo::exists(path("escape")));
    }

    // `..` after a link goes up from where the link leads, as the system resolves it.
    void dotDotAfterALinkCannotEscapeTheRoots() {
        QTemporaryDir elsewhere;
        QVERIFY(elsewhere.isValid());
        QVERIFY(QDir().mkpath(elsewhere.filePath("deep")));
        const QString source = rootsSource();

        // Write: <cwd>/wlink/../wout is elsewhere/wout, not <cwd>/wout.
        QVERIFY(QDir().mkpath(path("work4")));
        QVERIFY(QFile::link(elsewhere.filePath("deep"), path("work4/wlink")));
        const Outcome wrote = fugue({"run", rootsFlow(path("roots-wlink.fugue"), source, path("work4/wlink/../wout"), "esc"),
                                     "--wait", "--json"},
                                    path("work4"));
        QCOMPARE(wrote.exitCode, 1);
        QCOMPARE(firstError(wrote).value("code").toString(), QString("permission_denied"));
        QCOMPARE(firstError(wrote).value("input").toString(), QString("folder"));
        QVERIFY(!QFileInfo::exists(elsewhere.filePath("wout")));
        QVERIFY(!QFileInfo::exists(path("work4/wout")));

        // Read: ~/rlink/../x.png is elsewhere/x.png. The same bytes are first loaded from inside
        // home, so a cached result is waiting: the check must not depend on a cache miss.
        const Outcome inside = fugue({"run", rootsFlow(path("roots-in.fugue"), source, path("out-rlink"), "in"),
                                      "--wait", "--json"});
        QVERIFY2(inside.exitCode == 0, inside.out.constData());
        QVERIFY(QFile::copy(source, elsewhere.filePath("x.png")));
        QVERIFY(QFile::link(elsewhere.filePath("deep"), path("rlink")));
        const Outcome read = fugue({"run", rootsFlow(path("roots-rlink.fugue"), "~/rlink/../x.png", path("out-rlink"), "in"),
                                    "--wait", "--json"});
        QCOMPARE(read.exitCode, 1);
        QCOMPARE(firstError(read).value("code").toString(), QString("permission_denied"));
        QCOMPARE(firstError(read).value("node").toString(), QString("load"));
        QCOMPARE(firstError(read).value("input").toString(), QString("file"));
    }

    // A relative `file` path is the workflow's folder's, wherever `fugue run` was started
    // (Appendix A); the roots still judge where it leads after `..` and links.
    void aRelativeFileIsReadFromBesideTheWorkflow() {
        QVERIFY(QDir().mkpath(path("shared/inputs")));
        QVERIFY(QFile::copy(rootsSource(), path("shared/inputs/src.png")));
        QVERIFY(QDir().mkpath(path("work5")));
        const QString flow = rootsFlow(path("shared/flow.fugue"), "inputs/src.png", path("work5/out"), "relative");
        const Outcome ran = fugue({"run", flow, "--wait", "--json"}, path("work5"));
        QVERIFY2(ran.exitCode == 0, ran.out.constData());
        QVERIFY(QFileInfo::exists(path("work5/out/relative.png")));
        // `fugue validate` takes it from the same folder: the run's result is the cached one.
        const Outcome valid = fugue({"validate", flow, "--json"}, path("work5"));
        QVERIFY2(valid.exitCode == 0, valid.out.constData());
        QCOMPARE(valid.data().value("nodes").toObject().value("load").toObject().value("state").toString(), QString("cached"));

        QTemporaryDir elsewhere;
        QVERIFY(elsewhere.isValid());
        QVERIFY(QFile::copy(rootsSource(), elsewhere.filePath("src.png")));
        QVERIFY(QFile::link(elsewhere.path(), path("shared/away")));
        for (const QString &escape : {QStringLiteral("away/src.png"), QStringLiteral("inputs/../away/src.png")}) {
            const Outcome out = fugue({"run", rootsFlow(path("shared/escape.fugue"), escape, path("work5/out"), "escape"),
                                       "--wait", "--json"},
                                      path("work5"));
            QCOMPARE(out.exitCode, 1);
            QCOMPARE(firstError(out).value("code").toString(), QString("permission_denied"));
            QCOMPARE(firstError(out).value("input").toString(), QString("file"));
        }
    }

    // Load Image reads only under the home folder.
    void aLoadOutsideTheReadRootsIsRefused() {
        QTemporaryDir elsewhere;
        QVERIFY(elsewhere.isValid());
        const QString outside = elsewhere.filePath("src.png");
        QVERIFY(QFile::copy(rootsSource(), outside));
        const Outcome ran = fugue({"run", rootsFlow(path("roots-read.fugue"), outside, path("out-read"), "read"),
                                   "--wait", "--json"});
        QCOMPARE(ran.exitCode, 1);
        const QJsonObject error = firstError(ran);
        QCOMPARE(error.value("code").toString(), QString("permission_denied"));
        QCOMPARE(error.value("node").toString(), QString("load"));
        QCOMPARE(error.value("input").toString(), QString("file"));
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
              "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.0.0\", \"inputs\": {\"file\": \"" + path("nonexistent/none.png").toUtf8() + "\"}},"
              "\"look\": {\"type\": \"fugue/preview\", \"version\": \"1.0.0\", \"inputs\": {\"value\": {\"from\": \"load.width\"}}}}}}");
        const Outcome ran = fugue({"run", path("missing.fugue"), "--wait", "--json"});
        QCOMPARE(ran.exitCode, 1);
        QVERIFY(ran.out.contains("file_not_found"));
        QVERIFY(ran.out.contains(path("nonexistent/none.png").toUtf8()));
    }
};

FUGUE_TEST_MAIN(TestBasicCli, QCoreApplication)
#include "test_basic_cli.moc"
