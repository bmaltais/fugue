// `fugue app list|info|run` (#392, spec 4.9). Seam: the real `fugue` binary as a subprocess with a root of its own (FUGUE_HOME),
// an empty HOME, a folder of templates of the test's own (FUGUE_TEMPLATES) and no GPU helper. The app is load-image, invert and
// save-image, so the whole path (the binding, the queue, the worker, the Save node writing into --out) runs for real on the CPU.
// The block's reading and binding are tests/engine/test_app_binding.cpp's; this is the command: what it lists, the codes it
// refuses with, and the files it prints.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QJsonObject error() const { return json().value("errors").toArray().at(0).toObject(); }
    QString code() const { return error().value("code").toString(); }
};

const char *kInvert = R"({"fugue": 1,
 "app": {"format": 1, "title": "Invert", "description": "Inverts a picture.",
         "inputs": [{"role": "image", "targets": ["load.file"], "label": "Picture"},
                    {"name": "name", "targets": ["save.name"], "label": "Name"},
                    {"name": "amount", "targets": ["pad.value"], "min": 1, "max": 5, "default": 2}],
         "outputs": [{"node": "save", "label": "Result"}]},
 "graph": {"nodes": {
   "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": ""}},
   "pad": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 1}},
   "invert": {"type": "fugue/invert", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "invert.image"}, "name": "app"}}}}})";

// The same graph with a node of a pack that is not installed, and no source to get it from.
const char *kNeedsPack = R"({"fugue": 1, "requires": {"packs": {"io.example.tools": {"version": "1.0.0"}}},
 "app": {"format": 1, "title": "Needs a pack", "inputs": [], "outputs": [{"node": "save", "label": "Result"}]},
 "graph": {"nodes": {
   "other": {"type": "io.example.tools/tool", "version": "1.0.0", "inputs": {}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "other.image"}}}}}})";

const char *kPlain = R"({"fugue": 1, "graph": {"nodes": {"n": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 1}}}}})";

} // namespace

class TestCliApp : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    Outcome fugue(const QStringList &args) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "FUGUE_PROJECT",
                                     "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
            env.remove(QString::fromLatin1(variable));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_HOME", path("root"));
        env.insert("FUGUE_TEMPLATES", path("templates"));
        env.insert("FUGUE_HELPER", "");
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(path("work"));
        process.start(FUGUE_BINARY, args);
        Outcome outcome;
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            return outcome;
        }
        outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    void write(const QString &file, const QByteArray &bytes) const {
        QVERIFY(QDir().mkpath(QFileInfo(file).absolutePath()));
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(bytes);
    }

    void template_(const QString &id, const QString &title, const QByteArray &workflow) const {
        write(path("templates/" + id + "/template.json"), "{\"title\": \"" + title.toUtf8() + "\"}");
        write(path("templates/" + id + "/workflow.fugue"), workflow);
    }

    // Under the home folder: the one place a run may read from besides its own (13.6).
    QString picture() const { return path("home/pictures/in.png"); }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(path("home")));
        QVERIFY(QDir().mkpath(path("work")));
        // A 1x1 PNG: a real picture through the real loader.
        write(picture(), QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="));
        template_("invert", "Invert template", kInvert);
        template_("plain", "Plain", kPlain);
        write(path("work/needs-pack.fugue"), kNeedsPack);
        write(path("work/invert.fugue"), kInvert);
    }

    void listShowsTheAppsAndNotThePlainTemplates() {
        const Outcome listed = fugue({"app", "list", "--json"});
        QCOMPARE(listed.exitCode, 0);
        QStringList ids;
        for (const QJsonValue app : listed.data().value("apps").toArray())
            ids << app.toObject().value("id").toString();
        ids.sort();
        QCOMPARE(ids, (QStringList{"invert"}));
        QCOMPARE(listed.data().value("apps").toArray().at(0).toObject().value("title").toString(), QString("Invert"));
    }

    void infoGivesTheInputsAndOutputsOfATemplateOrAFile() {
        for (const QString &ref : {QString("invert"), QString("invert.fugue")}) {
            const Outcome info = fugue({"app", "info", ref, "--json"});
            QCOMPARE(info.exitCode, 0);
            const QJsonObject data = info.data();
            QVERIFY(data.value("ready").toBool());
            QCOMPARE(data.value("title").toString(), QString("Invert"));
            const QJsonArray inputs = data.value("inputs").toArray();
            QCOMPARE(inputs.size(), 3);
            const QJsonObject picture = inputs.at(0).toObject();
            QCOMPARE(picture.value("key").toString(), QString("image"));
            QCOMPARE(picture.value("type").toString(), QString("image"));
            QCOMPARE(picture.value("label").toString(), QString("Picture"));
            QVERIFY(picture.value("required").toBool());
            const QJsonObject name = inputs.at(1).toObject();
            QCOMPARE(name.value("type").toString(), QString("text"));
            QCOMPARE(name.value("default").toString(), QString("app")); // the workflow's own value
            QVERIFY(!name.value("required").toBool());
            const QJsonObject amount = inputs.at(2).toObject();
            QCOMPARE(amount.value("type").toString(), QString("number"));
            QCOMPARE(amount.value("default").toInt(), 2); // the block's default
            QCOMPARE(amount.value("min").toInt(), 1);
            QCOMPARE(amount.value("max").toInt(), 5);
            QCOMPARE(data.value("outputs").toArray().at(0).toObject().value("label").toString(), QString("Result"));
        }
    }

    void infoSaysWhenTheAppIsNotReady() {
        const Outcome info = fugue({"app", "info", "needs-pack.fugue", "--json"});
        QCOMPARE(info.exitCode, 0);
        QVERIFY(!info.data().value("ready").toBool());
        QVERIFY(info.data().contains("needs"));
    }

    void aWorkflowWithNoBlockOrNoSuchAppIsRefused() {
        write(path("work/plain.fugue"), kPlain);
        QCOMPARE(fugue({"app", "info", "plain.fugue", "--json"}).code(), QString("no_app"));
        QCOMPARE(fugue({"app", "info", "plain", "--json"}).code(), QString("no_app")); // a template with no block
        const Outcome none = fugue({"app", "run", "nothing", "--json"});
        QCOMPARE(none.code(), QString("file_not_found"));
        QVERIFY(none.error().value("valid").toArray().contains("invert"));
    }

    void runRefusesWhatItCannotBindBeforeAnythingRuns() {
        QCOMPARE(fugue({"app", "run", "invert", "--json"}).code(), QString("missing_input"));
        const Outcome unknown = fugue({"app", "run", "invert", "--input", "color=red", "--json"});
        QCOMPARE(unknown.code(), QString("unknown_input"));
        QVERIFY(unknown.error().value("valid").toArray().contains("amount"));
        QCOMPARE(fugue({"app", "run", "invert", "--input", "image=" + path("work/none.png"), "--json"}).code(), QString("file_not_found"));
        QCOMPARE(fugue({"app", "run", "invert", "--input", "image=" + picture(), "--input", "amount=lots", "--json"}).code(), QString("usage"));
        QCOMPARE(fugue({"app", "run", "needs-pack.fugue", "--json"}).code(), QString("app_not_ready"));
    }

    void runPrintsTheFilesTheOutputNodeWroteInOut() {
        write(path("work/name.txt"), "mine\n");
        const QString out = path("work/results");
        const Outcome ran = fugue({"app", "run", "invert", "--input", "image=" + picture(), "--input", "name=@" + path("work/name.txt"), "--input", "amount=3",
                                   "--out", out, "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        const QJsonArray files = ran.data().value("files").toArray();
        QCOMPARE(files.size(), 1);
        const QString file = files.at(0).toString();
        QVERIFY2(file.startsWith(out + "/mine"), qPrintable(file));
        QVERIFY(QFile::exists(file));
        const QJsonObject result = ran.data().value("outputs").toArray().at(0).toObject();
        QCOMPARE(result.value("label").toString(), QString("Result"));
        QCOMPARE(result.value("files").toArray().at(0).toString(), file);

        // Without --json the text is the paths, one per line, for a script to read; a file path is as good as an id.
        const Outcome text = fugue({"app", "run", path("work/invert.fugue"), "--input", "image=" + picture(), "--out", out});
        QVERIFY2(text.exitCode == 0, text.out + text.err);
        QVERIFY(QString::fromUtf8(text.out).trimmed().startsWith(out + "/app"));
        // Nothing was written to the home folder.
        QCOMPARE(QDir(path("home")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{"pictures"});
    }
};

QTEST_GUILESS_MAIN(TestCliApp)
#include "test_cli_app.moc"
