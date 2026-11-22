// `fugue canvas new|import|export|log|show` (spec 13.1, 14.5, epic #206). Seam: the `fugue` subprocess
// with an empty HOME, so the store is the test's own ~/.local/share/fugue/canvases, and under
// `--home` the project's own. The store itself is tests/engine/test_canvas_store.cpp.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/paint_document.h"
#include "engine/paint_file.h"

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QString errorCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
};

} // namespace

class TestCliCanvas : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    Outcome fugue(const QStringList &args) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "FUGUE_HOME",
                                     "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_RUNTIME_DIR", "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
            env.remove(QString::fromLatin1(variable));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_HELPER", "");
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(path("work"));
        process.start(FUGUE_BINARY, args);
        Outcome outcome;
        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished();
            return outcome;
        }
        outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    static QByteArray readFile(const QString &file) {
        QFile in(file);
        return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    }

    // A small canvas as an .ora file in the work folder.
    QString oraFile(const QString &name) const {
        fugue::mask::PaintDocument doc(300, 200);
        doc.addLayer(fugue::mask::LayerKind::Paint, "Paint");
        doc.brush.color = fugue::mask::Rgba{200, 30, 30, 255};
        doc.fillRect(20, 20, 100, 100);
        doc.addLayer(fugue::mask::LayerKind::Mask, "Mask");
        doc.fillRect(0, 0, 50, 50);
        const QString file = path("work/" + name);
        if (fugue::hasErrors(fugue::mask::writePaintFile(doc, file)))
            return {};
        return file;
    }

private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(path("home")));
        QVERIFY(QDir().mkpath(path("work")));
    }
    void cleanup() {
        QDir(path("home")).removeRecursively();
        QDir(path("work")).removeRecursively();
    }

    void newMakesACanvasWithOneRevision() {
        const Outcome made = fugue({"canvas", "new", "--name", "Sketch", "--size", "640x480", "--json"});
        QVERIFY2(made.exitCode == 0, made.out + made.err);
        const QString id = made.data().value("canvas").toString();
        QVERIFY(id.startsWith("canvas-"));
        QCOMPARE(made.data().value("width").toInt(), 640);
        QVERIFY(QFileInfo(path("home/.local/share/fugue/canvases/canvases/" + id + "/head")).isFile());
        const Outcome shown = fugue({"canvas", "show", id, "--json"});
        QVERIFY2(shown.exitCode == 0, shown.out + shown.err);
        QCOMPARE(shown.data().value("name").toString(), QString("Sketch"));
        QVERIFY(shown.data().value("is_head").toBool());
        QCOMPARE(shown.data().value("layers").toArray().size(), 1);
        QCOMPARE(shown.data().value("revision").toString(), made.data().value("head").toString());
        const Outcome log = fugue({"canvas", "log", id, "--json"});
        QCOMPARE(log.data().value("revisions").toArray().size(), 1);
        QCOMPARE(log.data().value("revisions").toArray().at(0).toObject().value("producer").toString(), QString("new"));
    }

    void importThenExportGivesTheSameFile() {
        const QString source = oraFile("in.ora");
        QVERIFY(!source.isEmpty());
        const Outcome imported = fugue({"canvas", "import", source, "--json"});
        QVERIFY2(imported.exitCode == 0, imported.out + imported.err);
        const QString id = imported.data().value("canvas").toString();
        QCOMPARE(imported.data().value("name").toString(), QString("in"));
        const QString out = path("work/out.ora");
        const Outcome exported = fugue({"canvas", "export", id, out, "--json"});
        QVERIFY2(exported.exitCode == 0, exported.out + exported.err);
        QCOMPARE(readFile(out), readFile(source));
        // By revision too, and an export never replaces a file.
        const QString head = imported.data().value("head").toString();
        QCOMPARE(fugue({"canvas", "export", id + "@" + head.mid(0, 15), path("work/rev.ora")}).exitCode, 0);
        QCOMPARE(readFile(path("work/rev.ora")), readFile(source));
        QVERIFY(fugue({"canvas", "export", id, out}).exitCode != 0);
    }

    void theSamePixelsHaveTheSameTreeInTwoCanvases() {
        const QString source = oraFile("in.ora");
        const QString a = fugue({"canvas", "import", source, "--json"}).data().value("canvas").toString();
        const QString b = fugue({"canvas", "import", source, "--json"}).data().value("canvas").toString();
        QVERIFY(a != b);
        QCOMPARE(fugue({"canvas", "show", a, "--json"}).data().value("tree").toString(), fugue({"canvas", "show", b, "--json"}).data().value("tree").toString());
    }

    void aProjectKeepsItsOwnCanvases() {
        QVERIFY(QDir().mkpath(path("proj")));
        const Outcome made = fugue({"--home", path("proj"), "canvas", "new", "--json"});
        QVERIFY2(made.exitCode == 0, made.out + made.err);
        const QString id = made.data().value("canvas").toString();
        QVERIFY(QFileInfo(path("proj/data/fugue/canvases/canvases/" + id + "/canvas.json")).isFile());
        QVERIFY(fugue({"canvas", "show", id, "--json"}).errorCode() == "canvas_not_found"); // the Default session has not got it
        QCOMPARE(fugue({"--home", path("proj"), "canvas", "show", id}).exitCode, 0);
        QDir(path("proj")).removeRecursively();
    }

    void errorsAreCodedAndHelpful() {
        QCOMPARE(fugue({"canvas", "show", "canvas-nope", "--json"}).errorCode(), QString("canvas_not_found"));
        QCOMPARE(fugue({"canvas", "new", "--size", "big", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "new", "--size", "99999x10", "--json"}).errorCode(), QString("input_invalid"));
        QCOMPARE(fugue({"canvas", "log", "canvas-nope", "--json"}).errorCode(), QString("canvas_not_found"));
        QFile bad(path("work/bad.ora"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("nope");
        bad.close();
        QCOMPARE(fugue({"canvas", "import", path("work/bad.ora"), "--json"}).errorCode(), QString("media_unreadable"));
        QVERIFY(fugue({"--help"}).out.contains("canvas new"));
    }
};

QTEST_GUILESS_MAIN(TestCliCanvas)
#include "test_cli_canvas.moc"
