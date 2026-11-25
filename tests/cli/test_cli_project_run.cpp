// Two roots, one GPU (spec 1.4.2, epic #204 step 2 slice 3), and the Default left alone (slice 4).
//
// Each root has its own queue and engine lock, so without more two project windows would run at the
// same time and put two helpers on one GPU. A run takes a content-free lock in the real runtime
// folder (shared by every root) after its engine lock; a run in a second root waits for the one in the
// first. Seam: the real CLI, queue and worker (fugue_with_fakes, with its `hold` kernel that waits on a
// gate file), two roots with project.json, one shared XDG_RUNTIME_DIR. Nothing sleeps for a fixed time
// except the short look that a waiting run stays waiting.
//
// The guard at the end: `fugue settings list`, `model list` and a run inside a project that inherits
// everything change nothing under the Default's config, data and state folders.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <csignal>

#include "engine/run_folder.h"

using namespace fugue;

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
};

QJsonObject holdManifest() {
    return QJsonObject{{"manifest", 1},
                       {"id", "fugue/hold"},
                       {"version", "1.0.0"},
                       {"name", "hold"},
                       {"runtime", QJsonObject{{"kind", "builtin"}}},
                       {"inputs", QJsonArray()},
                       {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}}}},
                       {"sink", true}};
}

} // namespace

class TestCliProjectRun : public QObject {
    Q_OBJECT

    QTemporaryDir *m_dir = nullptr;

    QString path(const QString &name) const { return m_dir->filePath(name); }
    QString root(int n) const { return path(QStringLiteral("root%1").arg(n)); }
    QString runsRoot(int n) const { return root(n) + "/cache/fugue/runs"; }
    QString queueDir(int n) const { return root(n) + "/state/fugue/queue"; }
    QString gate() const { return path("gate"); }

    void write(const QString &file, const QByteArray &bytes) const {
        QDir().mkpath(QFileInfo(file).path());
        QFile handle(file);
        QVERIFY2(handle.open(QIODevice::WriteOnly), qPrintable(file));
        handle.write(bytes);
    }

    // The Default's own folders, and a shared runtime folder; `home` empty is the Default session.
    Outcome fugue(const QStringList &args, const QString &home = {}) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "FUGUE_HOME", "FUGUE_PROJECT",
                                     "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_RUNTIME_DIR"})
            env.remove(QString::fromLatin1(variable));
        env.insert("XDG_CONFIG_HOME", path("base/config"));
        env.insert("XDG_DATA_HOME", path("base/data"));
        env.insert("XDG_STATE_HOME", path("base/state"));
        env.insert("XDG_CACHE_HOME", path("base/cache"));
        env.insert("XDG_RUNTIME_DIR", path("runtime"));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_NODE_MANIFESTS", path("manifests"));
        env.insert("FUGUE_TEST_GATE", gate());
        env.insert("FUGUE_HELPER", "");
        if (!home.isEmpty())
            env.insert("FUGUE_HOME", home);
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(FUGUE_FAKE_BINARY, args);
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

    QString start(int n) const {
        const Outcome o = fugue({"run", path("hold.fugue"), "--json"}, root(n));
        if (o.exitCode != 0)
            qWarning("run failed: %s %s", o.out.constData(), o.err.constData());
        return o.data().value("run").toString();
    }

    QJsonObject stateOf(int n, const QString &id) const {
        const auto folder = RunFolder::open(runsRoot(n), id);
        return folder.ok() ? folder->state() : QJsonObject();
    }
    QString statusOf(int n, const QString &id) const { return stateOf(n, id).value("status").toString(); }

    // The queue entry of `id` in root n: its file name and the pid its worker recorded.
    QString entryOf(int n, const QString &id, qint64 *pid = nullptr) const {
        for (const QString &name : QDir(queueDir(n)).entryList(QDir::Files | QDir::NoDotAndDotDot)) {
            if (!name.endsWith(id))
                continue;
            QFile file(queueDir(n) + "/" + name);
            if (pid && file.open(QIODevice::ReadOnly))
                *pid = qint64(QJsonDocument::fromJson(file.readAll()).object().value("pid").toDouble());
            return name;
        }
        return {};
    }

    bool anyWorkerAlive() const {
        for (const QString &pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile file("/proc/" + pid + "/cmdline");
            if (!pid.at(0).isDigit() || !file.open(QIODevice::ReadOnly))
                continue;
            if (!file.readAll().split('\0').contains("__worker")) // an argument of its own, not part of some text
                continue;
            // Only this test's own workers: another run on the machine (an agent's, a second suite) is not ours.
            QFile environ("/proc/" + pid + "/environ");
            if (environ.open(QIODevice::ReadOnly) && environ.readAll().contains(m_dir->path().toUtf8()))
                return true;
        }
        return false;
    }

    static QStringList tree(const QString &base) {
        QStringList lines;
        for (const char *folder : {"config", "data", "state"}) {
            QDirIterator it(base + "/" + folder, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                QFile file(it.filePath());
                const QByteArray content = it.fileInfo().isFile() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
                lines << QStringLiteral("%1 %2 %3").arg(it.filePath()).arg(it.fileInfo().isDir() ? -1 : it.fileInfo().size()).arg(QString::fromLatin1(content));
            }
        }
        lines.sort();
        return lines;
    }

private slots:
    void init() {
        delete m_dir;
        m_dir = new QTemporaryDir;
        QVERIFY(m_dir->isValid());
        write(path("manifests/hold/node.json"), QJsonDocument(holdManifest()).toJson());
        write(path("hold.fugue"),
              R"({"fugue": 1, "graph": {"nodes": {"h": {"type": "fugue/hold", "version": "1.0.0", "inputs": {}}}}})");
        for (int n : {1, 2})
            write(root(n) + "/project.json", R"({"format":1,"name":"P","inherits":["models","preferences"]})");
        QDir().mkpath(path("runtime"));
        QDir().mkpath(path("home"));
    }

    void cleanup() {
        // Let any worker finish before the folders go away.
        QVERIFY2(QFile(gate()).open(QIODevice::WriteOnly), "gate");
        QTRY_VERIFY_WITH_TIMEOUT(!anyWorkerAlive(), 30000);
        delete m_dir;
        m_dir = nullptr;
    }

    void aRunInASecondRootWaitsForTheRunInTheFirst() {
        const QString a = start(1);
        QVERIFY(!a.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(1, a), QString("running"), 15000);
        const QString b = start(2);
        QVERIFY(!b.isEmpty());
        // Its worker is up and waiting: it recorded its pid, and its entry is not active.
        qint64 pid = 0;
        QTRY_VERIFY_WITH_TIMEOUT(!entryOf(2, b, &pid).isEmpty() && pid > 0, 15000);
        QTest::qWait(400); // a look that it stays waiting: it would have started by now
        QCOMPARE(statusOf(2, b), QString("queued"));
        QVERIFY(!entryOf(2, b).startsWith("0-"));
        QVERIFY(entryOf(1, a).startsWith("0-")); // the first one is the active one of its own root

        QVERIFY(QFile(gate()).open(QIODevice::WriteOnly)); // the hold kernels let go
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(1, a), QString("ok"), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(2, b), QString("ok"), 20000);
        const QString endedA = stateOf(1, a).value("ended").toString();
        const QString startedB = stateOf(2, b).value("started").toString();
        QVERIFY2(!endedA.isEmpty() && !startedB.isEmpty() && startedB >= endedA,
                 qPrintable(QStringLiteral("a ended %1, b started %2").arg(endedA, startedB)));
    }

    void aRunInTheSameRootAndOneInTheDefaultSessionShareTheGpuToo() {
        const QString a = start(1);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(1, a), QString("running"), 15000);
        // The Default session (no root) has its own queue folder under the base state.
        const Outcome o = fugue({"run", path("hold.fugue"), "--json"});
        const QString d = o.data().value("run").toString();
        QVERIFY(!d.isEmpty());
        qint64 pid = 0;
        const QString queueDefault = path("base/state/fugue/queue");
        QTRY_VERIFY_WITH_TIMEOUT(!QDir(queueDefault).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(), 15000);
        QTest::qWait(300);
        const auto defaultRun = RunFolder::open(path("base/cache/fugue/runs"), d);
        QVERIFY(defaultRun.ok());
        QCOMPARE(defaultRun->state().value("status").toString(), QString("queued"));
        (void)pid;
        QVERIFY(QFile(gate()).open(QIODevice::WriteOnly));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(1, a), QString("ok"), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(RunFolder::open(path("base/cache/fugue/runs"), d)->state().value("status").toString(), QString("ok"), 20000);
    }

    void aKilledHoldersLockDoesNotBlockTheNextRoot() {
        const QString a = start(1);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(1, a), QString("running"), 15000);
        const QString b = start(2);
        qint64 pid = 0;
        QTRY_VERIFY_WITH_TIMEOUT(!entryOf(2, b, &pid).isEmpty() && pid > 0, 15000);
        // Kill the first run's worker hard: no cleanup, only the system lets its locks go.
        qint64 workerA = 0;
        QVERIFY(!entryOf(1, a, &workerA).isEmpty() && workerA > 0);
        QVERIFY(::kill(pid_t(workerA), SIGKILL) == 0);
        QVERIFY(QFile(gate()).open(QIODevice::WriteOnly));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(2, b), QString("ok"), 20000);
    }

    void theDefaultIsUnchangedByASettingsListAModelListAndARunInAProject() {
        write(path("base/config/fugue/settings.json"),
              R"({"new_seed": "fixed", "model_paths": ["/srv/models-d"], "output_folder": "/srv/out-d"})");
        write(path("base/data/fugue/models/loras/x.safetensors"), "x");
        write(path("base/state/fugue/trust.json"), "{}");
        const QStringList before = tree(path("base"));
        QVERIFY(before.size() > 3);

        const Outcome settings = fugue({"settings", "list", "--json"}, root(1));
        QCOMPARE(settings.exitCode, 0);
        QMap<QString, QJsonObject> byKey;
        for (const QJsonValue &entry : settings.data().value("settings").toArray())
            byKey.insert(entry.toObject().value("key").toString(), entry.toObject());
        QCOMPARE(byKey.value("new_seed").value("value").toString(), QString("fixed"));
        QCOMPARE(byKey.value("new_seed").value("origin").toString(), QString("inherited from Default"));
        QCOMPARE(byKey.value("navigation").value("origin").toString(), QString("built-in"));
        QVERIFY(byKey.value("output_folder").value("value").isNull()); // never inherited
        QCOMPARE(byKey.value("output_folder").value("origin").toString(), QString("built-in"));
        QVERIFY(byKey.value("model_paths").value("inherited_entries").toArray().contains("/srv/models-d"));
        QVERIFY(QString::fromUtf8(fugue({"settings", "list"}, root(1)).out).contains("new_seed = fixed  (inherited from Default)"));
        // The Default session reads its own file: every set key is its own.
        const Outcome plain = fugue({"settings", "list", "--json"});
        for (const QJsonValue &entry : plain.data().value("settings").toArray())
            if (entry.toObject().value("key").toString() == "new_seed")
                QCOMPARE(entry.toObject().value("origin").toString(), QString("own"));

        const Outcome models = fugue({"model", "list", "--json"}, root(1));
        QCOMPARE(models.exitCode, 0);
        QStringList origins;
        for (const QJsonValue &root : models.data().value("roots").toArray())
            origins << root.toObject().value("origin").toString() + ":" + root.toObject().value("path").toString();
        QVERIFY2(origins.contains("inherited from Default:" + path("base/data/fugue/models")), qPrintable(origins.join('\n')));
        QVERIFY(origins.contains("inherited from Default:/srv/models-d"));
        QVERIFY(origins.contains("own:" + root(1) + "/data/fugue/models"));
        // --into an inherited root is refused, naming the Default project.
        const Outcome into = fugue({"model", "get", path("hold.fugue"), "--into", path("base/data/fugue/models"), "--json"}, root(1));
        QCOMPARE(into.exitCode, 1);
        QVERIFY2(into.out.contains("read-only"), into.out);

        const QString id = start(1);
        QVERIFY(QFile(gate()).open(QIODevice::WriteOnly));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(1, id), QString("ok"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!anyWorkerAlive(), 20000);

        QCOMPARE(tree(path("base")), before);
    }
};

QTEST_GUILESS_MAIN(TestCliProjectRun)
#include "test_cli_project_run.moc"
