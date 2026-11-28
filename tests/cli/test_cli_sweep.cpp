// `fugue run --sweep` (#552, spec 13.1) end to end on the CPU: Load Image and Save Image per cell, then the sheet's own run (Image Grid).
// Seam: the real `fugue` subprocess with an empty HOME (the output folder, the queue and the runs are the test's own). The plan and the
// sheet's workflow are tests/engine/test_sweep.cpp's; this proves the runs, the blank tile of a failed cell and the printed answer.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include "../test_paths.h"

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

class TestCliSweep : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    Outcome fugue(const QStringList &args) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "XDG_PICTURES_DIR", "TMPDIR",
                                     "FUGUE_HOME", "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_RUNTIME_DIR", "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
            env.remove(QString::fromLatin1(variable));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_HELPER", "");
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(path("home/work"));
        process.start(fugueBinary(), args);
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

    // A 1 x 1 PNG (no GUI library here to draw one).
    QString picture(const QString &name) const {
        QFile file(path("home/pics/" + name));
        const QByteArray png = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg==");
        return file.open(QIODevice::WriteOnly) && file.write(png) == png.size() ? file.fileName() : QString();
    }

    // The width and height in a PNG's header.
    static QSize sizeOfPng(const QString &file) {
        QFile handle(file);
        if (!handle.open(QIODevice::ReadOnly))
            return {};
        const QByteArray head = handle.read(24);
        const auto at = [&](int i) { return (uchar(head[i]) << 24) | (uchar(head[i + 1]) << 16) | (uchar(head[i + 2]) << 8) | uchar(head[i + 3]); };
        return head.size() == 24 ? QSize(at(16), at(20)) : QSize();
    }

private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        // The output folder is outside the home folder (a drive of its own): the cells are saved there and the sheet's run must still read
        // them, though the file roots only allow reading under home.
        QVERIFY(QDir().mkpath(path("outside/out")));
        QVERIFY(QDir().mkpath(path("home/.config/fugue")));
        QFile settings(path("home/.config/fugue/settings.json"));
        QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write(QJsonDocument(QJsonObject{{"output_folder", path("outside/out")}}).toJson());
        QVERIFY(QDir().mkpath(path("home/work")));
        QVERIFY(QDir().mkpath(path("home/pics")));
    }
    void cleanup() {
        QDir(path("home")).removeRecursively();
        QDir(path("outside")).removeRecursively();
    }

    // Two axes, four cells: a picture that is there and one that is not, each as png and webp. The two that fail are blank tiles, the sheet
    // is still made, and the answer says which is which.
    void aFailedCellIsABlankTileNotTheEndOfTheSweep() {
        const QString red = picture("red.png");
        QVERIFY(!red.isEmpty());
        QFile flow(path("home/work/flow.fugue"));
        QVERIFY(flow.open(QIODevice::WriteOnly));
        flow.write(R"({"fugue": 1, "graph": {"nodes": {
          "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": ""}},
          "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"image": {"from": "load.image"}, "name": "cell", "folder": "cells"}}}}})");
        flow.close();

        const Outcome run = fugue({"run", "flow.fugue", "--wait", "--to", "save", "--json", "--sweep", "load.file=" + red + "," + path("home/pics/gone.png"),
                                   "--sweep", "save.format=png,webp"});
        QCOMPARE(run.exitCode, 1); // a cell failed, the sheet is made all the same
        QVERIFY2(run.json().value("ok").toBool(), run.out + run.err);
        const QJsonObject data = run.data();
        QVERIFY2(data.value("grid").toString().startsWith(path("outside/out")), run.out);
        const QJsonArray cells = data.value("cells").toArray();
        QCOMPARE(cells.size(), 4);
        QCOMPARE(data.value("failed").toInt(), 2);
        QCOMPARE(cells.at(0).toObject().value("status").toString(), QString("ok"));
        QVERIFY(QFile::exists(cells.at(0).toObject().value("file").toString()));
        QCOMPARE(cells.at(1).toObject().value("status").toString(), QString("failed")); // the first axis runs along a row
        QCOMPARE(cells.at(1).toObject().value("code").toString(), QString("file_not_found"));
        QCOMPARE(cells.at(1).toObject().value("label").toString(), QString("file=gone.png, format=png"));
        QCOMPARE(cells.at(2).toObject().value("status").toString(), QString("ok")); // the webp of the same picture
        QCOMPARE(cells.at(3).toObject().value("status").toString(), QString("failed"));
        const QSize grid = sizeOfPng(data.value("grid").toString());
        QVERIFY(grid.width() > 2 + 3 * 8 && grid.height() > 2 + 3 * 8 + 2 * 10); // two 1 px cells, three gaps and two label lines
        QCOMPARE(run.json().value("warnings").toArray().at(0).toObject().value("severity").toString(), QString("warning"));
    }

    // Nothing is started for a sweep the plan refuses.
    void aRefusedSweepRunsNothing() {
        QFile flow(path("home/work/flow.fugue"));
        QVERIFY(flow.open(QIODevice::WriteOnly));
        flow.write(R"({"fugue": 1, "graph": {"nodes": {"load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": ""}}}}})");
        flow.close();
        const Outcome run = fugue({"run", "flow.fugue", "--wait", "--to", "load", "--json", "--sweep", "load.nope=1,2"});
        QCOMPARE(run.exitCode, 1);
        QCOMPARE(run.errorCode(), QString("unknown_input"));
        const Outcome noWait = fugue({"run", "flow.fugue", "--to", "load", "--json", "--sweep", "load.file=a,b"});
        QCOMPARE(noWait.errorCode(), QString("usage"));
        QVERIFY(fugue({"runs", "list", "--json"}).data().value("runs").toArray().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCliSweep)
#include "test_cli_sweep.moc"
