// `fugue pack list`, `pack remove`, `template list` and a run inside a project that inherits packs and
// templates (spec 1.4.2, epic #204 step 2 slice 5), as the real binary. The Default is HOME's own
// ~/.config, ~/.local/share and ~/.local/state; its folders must be as they were after every command of
// the project. The engine side is tests/engine/test_project_packs.cpp.

#include "../test_paths.h"
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

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonValue data() const { return json().value("data"); }
    QString errorCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
};

} // namespace

class TestCliProjectPacks : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    Outcome fugue(const QStringList &args, const QString &home = {}) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR",
                                     "FUGUE_HOME", "FUGUE_PROJECT", "FUGUE_TEMPLATES", "FUGUE_BASE_XDG_CONFIG_HOME",
                                     "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR",
                                     "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
            env.remove(QString::fromLatin1(variable));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_HELPER", "");
        env.insert("FUGUE_TEMPLATES", path("no-bundled"));
        if (!home.isEmpty())
            env.insert("FUGUE_HOME", home);
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(path("work"));
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

    void write(const QString &file, const QByteArray &bytes) const {
        QDir().mkpath(QFileInfo(file).path());
        QFile out(file);
        QVERIFY2(out.open(QIODevice::WriteOnly), qPrintable(file));
        out.write(bytes);
    }

    // What the Default holds: its config, data and state, and its Documents folder, with the content.
    QStringList defaults() const {
        QStringList lines;
        for (const QString &folder : {path("home/.config"), path("home/.local"), path("home/Documents")}) {
            QDirIterator it(folder, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                QFile file(it.filePath());
                const QByteArray content = it.fileInfo().isFile() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
                lines << QStringLiteral("%1 %2 %3").arg(it.filePath()).arg(it.fileInfo().isDir() ? -1 : it.fileInfo().size()).arg(QString::fromLatin1(content.toHex()));
            }
        }
        lines.sort();
        return lines;
    }

    void makeDefault() const {
        const QString pack = path("home/.local/share/fugue/packs/io.example.tools");
        write(pack + "/pack.json", R"({"manifest":1,"id":"io.example.tools","version":"1.0.0","name":"Example tools","nodes":["say"]})");
        write(pack + "/say/node.json",
              R"({"manifest":1,"id":"io.example.tools/say","version":"1.0.0","name":"Say","runtime":{"kind":"command","entry":"./run.sh"},
                  "inputs":[],"outputs":[{"name":"text","type":"text"}]})");
        write(pack + "/say/run.sh", "#!/bin/bash\ncat > /dev/null\necho '{\"outputs\": {\"text\": \"hello\"}}'\n");
        QVERIFY(QFile::setPermissions(pack + "/say/run.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(path("home/.local/state/fugue/trust.json"), R"({"packs":{"io.example.tools":["1.0.0"]}})");
        write(path("home/Documents/Fugue/templates/note/template.json"), R"({"title":"Note"})");
        write(path("home/Documents/Fugue/templates/note/workflow.fugue"),
              R"({"fugue":1,"graph":{"nodes":{"t":{"type":"fugue/text","version":"1.0.0","inputs":{"text":"a fox"}}}}})");
    }

    QString makeProject(const QString &inherits) const {
        const Outcome made = fugue({"project", "new", "Acme", "--in", path("proj/acme"), "--inherits", inherits, "--json"});
        return made.exitCode == 0 ? path("proj/acme") : QString();
    }

private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(path("home")));
        QVERIFY(QDir().mkpath(path("work")));
        makeDefault();
    }
    void cleanup() {
        for (const char *folder : {"home", "work", "proj"})
            QDir(path(QString::fromLatin1(folder))).removeRecursively();
    }

    void packListShowsTheOriginAndTheWarningOfInheritedPacks() {
        const QString project = makeProject("packs");
        QVERIFY(!project.isEmpty());
        const QStringList before = defaults();
        const Outcome json = fugue({"pack", "list", "--json"}, project);
        QVERIFY2(json.exitCode == 0, json.out + json.err);
        const QJsonArray packs = json.data().toObject().value("packs").toArray();
        QCOMPARE(packs.size(), 1);
        const QJsonObject pack = packs.at(0).toObject();
        QCOMPARE(pack.value("id").toString(), QString("io.example.tools"));
        QCOMPARE(pack.value("origin").toString(), QString("inherited from Default"));
        QVERIFY(pack.value("inherited").toBool());
        QVERIFY(pack.value("trusted").toBool()); // the Default's decision, read-only
        QVERIFY(pack.value("warning").toString().contains("Default project's folders"));
        const Outcome text = fugue({"pack", "list"}, project);
        QVERIFY(text.out.contains("[inherited from Default]"));
        QVERIFY(text.out.contains("Warning: Inherited from Default"));
        // The Default session lists the same pack as its own, with no warning.
        const Outcome plain = fugue({"pack", "list"});
        QVERIFY(plain.out.contains("io.example.tools 1.0.0  trusted"));
        QVERIFY(!plain.out.contains("inherited"));
        QCOMPARE(defaults(), before);
    }

    void aProjectThatDoesNotInheritPacksSeesNone() {
        const QString project = makeProject("models");
        const Outcome text = fugue({"pack", "list"}, project);
        QCOMPARE(text.exitCode, 0);
        QVERIFY(text.out.contains("No packs installed."));
    }

    void removingAnInheritedPackIsRefusedAndNamesTheDefault() {
        const QString project = makeProject("packs");
        const QStringList before = defaults();
        const Outcome removed = fugue({"pack", "remove", "io.example.tools", "--json"}, project);
        QVERIFY(removed.exitCode != 0);
        QCOMPARE(removed.errorCode(), QString("usage"));
        QVERIFY(removed.out.contains("Default project"));
        QCOMPARE(defaults(), before);
    }

    void templateListShowsTheOriginOfInheritedTemplates() {
        const QString project = makeProject("templates");
        const QString closed = path("proj/closed");
        QVERIFY(fugue({"project", "new", "Closed", "--in", closed, "--inherits", "none", "--json"}).exitCode == 0);
        const QStringList before = defaults();
        const Outcome json = fugue({"template", "list", "--json"}, project);
        QVERIFY2(json.exitCode == 0, json.out + json.err);
        const QJsonArray templates = json.data().toObject().value("templates").toArray();
        QCOMPARE(templates.size(), 1);
        QCOMPARE(templates.at(0).toObject().value("origin").toString(), QString("inherited from Default"));
        QVERIFY(fugue({"template", "list"}, project).out.contains("[inherited from Default]"));
        QVERIFY(fugue({"template", "list"}, closed).out.trimmed().isEmpty());
        QCOMPARE(defaults(), before);
    }

    void aRunUsingAnInheritedTrustedPackWorksAndLeavesTheDefaultAsItWas() {
        const QString project = makeProject("packs");
        write(path("work/say.fugue"), R"({"fugue":1,"graph":{"nodes":{"s":{"type":"io.example.tools/say","version":"1.0.0","inputs":{}},
                                         "p":{"type":"fugue/preview","version":"1.0.0","inputs":{"value":{"from":"s.text"}}}}}})");
        const QStringList before = defaults();
        const Outcome run = fugue({"run", path("work/say.fugue"), "--wait", "--json"}, project);
        QVERIFY2(run.exitCode == 0, run.out + run.err);
        QVERIFY(run.out.contains("hello"));
        QCOMPARE(defaults(), before); // the project's run, results and queue went under its own root
    }
};

QTEST_GUILESS_MAIN(TestCliProjectPacks)
#include "test_cli_project_packs.moc"
