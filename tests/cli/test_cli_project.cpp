// `fugue project ...` and the project selection of the real binary (spec 1.4.2, epic #204). Seam: the
// `fugue` subprocess with an empty HOME and no XDG or FUGUE_* variables from the test runner, so the
// registry is the test's own ~/.config/fugue/projects.json and a path that ignores a project's root lands
// in the empty HOME where the test sees it. The engine side is tests/engine/test_project.cpp.

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

QStringList tree(const QString &path) {
    QStringList lines;
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        lines << QStringLiteral("%1 %2").arg(QDir(path).relativeFilePath(it.filePath())).arg(it.fileInfo().isDir() ? -1 : it.fileInfo().size());
    }
    lines.sort();
    return lines;
}

} // namespace

class TestCliProject : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QProcessEnvironment m_extra;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    Outcome fugue(const QStringList &args, const QProcessEnvironment &extra = {}) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR",
                                     "FUGUE_HOME", "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME",
                                     "FUGUE_BASE_XDG_STATE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR", "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
            env.remove(QString::fromLatin1(variable));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_HELPER", "");
        env.insert(extra);
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

    void writeFile(const QString &file, const QByteArray &bytes) const {
        QDir().mkpath(QFileInfo(file).path());
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(bytes);
    }

    static QByteArray readFile(const QString &file) {
        QFile in(file);
        return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    }

    QString registryPath() const { return path("home/.config/fugue/projects.json"); }

    QString imageWorkflow() const {
        writeFile(path("work/inputs/in.png"),
                  QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="));
        const QString file = path("work/images.fugue");
        writeFile(file, "{\"fugue\": 1, \"graph\": {\"nodes\": {"
                        "\"load\": {\"type\": \"fugue/load-image\", \"version\": \"1.1.0\", \"inputs\": {\"file\": \"" + path("work/inputs/in.png").toUtf8() + "\"}},"
                        "\"save\": {\"type\": \"fugue/save-image\", \"version\": \"1.0.0\", \"inputs\": {\"image\": {\"from\": \"load.image\"}, \"name\": \"proj\"}}}}}");
        return file;
    }

private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(path("home")));
        QVERIFY(QDir().mkpath(path("work")));
    }
    void cleanup() {
        for (const char *folder : {"home", "work", "proj"})
            QDir(path(QString::fromLatin1(folder))).removeRecursively();
    }

    void listWithNothingShowsDefaultAndWritesNothing() {
        const Outcome o = fugue({"project", "list", "--json"});
        QCOMPARE(o.exitCode, 0);
        QVERIFY(o.data().toObject().value("projects").toArray().isEmpty());
        QVERIFY(o.data().toObject().value("default_current").toBool());
        QCOMPARE(tree(path("home")), QStringList());
        const Outcome text = fugue({"project", "list"});
        QVERIFY(text.out.contains("default"));
    }

    void newMakesTheProjectAndRegistersIt() {
        const Outcome o = fugue({"project", "new", "Acme", "--in", path("proj/acme"), "--json"});
        QVERIFY2(o.exitCode == 0, o.out + o.err);
        QCOMPARE(o.data().toObject().value("name").toString(), QString("Acme"));
        QCOMPARE(o.data().toObject().value("path").toString(), path("proj/acme"));
        QCOMPARE(o.data().toObject().value("inherits").toArray().size(), 3); // models, packs, preferences
        const QJsonObject manifest = QJsonDocument::fromJson(readFile(path("proj/acme/project.json"))).object();
        QCOMPARE(manifest.value("format").toInt(), 1);
        QCOMPARE(manifest.value("name").toString(), QString("Acme"));
        QVERIFY(QFileInfo(registryPath()).isFile());
        const Outcome list = fugue({"project", "list", "--json"});
        const QJsonArray projects = list.data().toObject().value("projects").toArray();
        QCOMPARE(projects.size(), 1);
        QCOMPARE(projects.at(0).toObject().value("state").toString(), QString("ok"));
        QVERIFY(!projects.at(0).toObject().value("current").toBool());
    }

    void newWithoutInDefaultsToTheDataFolderAndTakesInherits() {
        const Outcome o = fugue({"project", "new", "Client Work", "--inherits", "models,templates", "--json"});
        QVERIFY2(o.exitCode == 0, o.out + o.err);
        QCOMPARE(o.data().toObject().value("path").toString(), path("home/.local/share/fugue/projects/client-work"));
        QCOMPARE(o.data().toObject().value("inherits").toArray(), (QJsonArray{"models", "templates"}));
        const Outcome none = fugue({"project", "new", "Bare", "--inherits", "none", "--json"});
        QVERIFY(none.data().toObject().value("inherits").toArray().isEmpty());
    }

    void newErrors() {
        QCOMPARE(fugue({"project", "new", "default", "--json"}).errorCode(), QString("project_invalid"));
        QCOMPARE(fugue({"project", "new", "a/b", "--json"}).errorCode(), QString("project_invalid"));
        QCOMPARE(fugue({"project", "new", "Acme", "--in", path("proj/a"), "--json"}).exitCode, 0);
        const Outcome again = fugue({"project", "new", "acme", "--in", path("proj/b"), "--json"});
        QCOMPARE(again.exitCode, 1);
        QCOMPARE(again.errorCode(), QString("project_exists"));
        QVERIFY(!QFileInfo::exists(path("proj/b")));
        QCOMPARE(fugue({"project", "new", "Other", "--in", path("proj/a"), "--json"}).errorCode(), QString("project_exists"));
        QCOMPARE(fugue({"project", "new", "--json"}).errorCode(), QString("usage"));
    }

    void ignoredInheritsAreAWarningNotAFailure() {
        const Outcome o = fugue({"project", "new", "Acme", "--in", path("proj/a"), "--inherits", "models,providers,bogus", "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.data().toObject().value("inherits").toArray(), (QJsonArray{"models"}));
        QCOMPARE(o.data().toObject().value("ignored").toArray(), (QJsonArray{"providers", "bogus"}));
        QCOMPARE(o.json().value("warnings").toArray().size(), 2);
        QCOMPARE(o.json().value("warnings").toArray().at(0).toObject().value("code").toString(), QString("project_inherits_ignored"));
    }

    void showByNameAndCurrent() {
        QCOMPARE(fugue({"project", "new", "Acme", "--in", path("proj/a"), "--json"}).exitCode, 0);
        const Outcome named = fugue({"project", "show", "acme", "--json"});
        QCOMPARE(named.exitCode, 0);
        QCOMPARE(named.data().toObject().value("path").toString(), path("proj/a"));
        QVERIFY(!named.data().toObject().value("current").toBool());
        const Outcome none = fugue({"project", "show", "--json"});
        QCOMPARE(none.data().toObject().value("name").toString(), QString("default"));
        // Inside the project it is the current one, and the registry is still the base one.
        const Outcome inside = fugue({"--project", "Acme", "project", "show", "--json"});
        QVERIFY2(inside.exitCode == 0, inside.out + inside.err);
        QCOMPARE(inside.data().toObject().value("name").toString(), QString("Acme"));
        QVERIFY(inside.data().toObject().value("current").toBool());
        QVERIFY(inside.data().toObject().value("registered").toBool());
        const Outcome list = fugue({"project", "list", "--json"}, [&] { QProcessEnvironment e; e.insert("FUGUE_PROJECT", "Acme"); return e; }());
        QCOMPARE(list.data().toObject().value("projects").toArray().size(), 1);
        QVERIFY(list.data().toObject().value("projects").toArray().at(0).toObject().value("current").toBool());
        QCOMPARE(fugue({"project", "show", "nope", "--json"}).errorCode(), QString("unknown_project"));
    }

    void addRegistersAProjectFromAnotherPlace() {
        writeFile(path("proj/x/project.json"), R"({"format":1,"name":"X"})");
        const Outcome o = fugue({"project", "add", path("proj/x"), "--json"});
        QVERIFY2(o.exitCode == 0, o.out + o.err);
        QCOMPARE(fugue({"project", "list", "--json"}).data().toObject().value("projects").toArray().size(), 1);
        QCOMPARE(fugue({"project", "add", path("proj/none"), "--json"}).errorCode(), QString("project_invalid"));
        writeFile(path("proj/bad/project.json"), "{");
        QCOMPARE(fugue({"project", "add", path("proj/bad"), "--json"}).errorCode(), QString("invalid_json"));
    }

    void aGoneProjectListsAsMissing() {
        QCOMPARE(fugue({"project", "new", "Acme", "--in", path("proj/a"), "--json"}).exitCode, 0);
        QVERIFY(QDir(path("proj/a")).removeRecursively());
        const Outcome list = fugue({"project", "list", "--json"});
        QCOMPARE(list.data().toObject().value("projects").toArray().at(0).toObject().value("state").toString(), QString("missing"));
        QVERIFY(fugue({"project", "list"}).out.contains("[missing]"));
        const Outcome use = fugue({"--project", "a", "runs", "list", "--json"});
        QCOMPARE(use.exitCode, 2);
    }

    void removeOnlyUnregisters() {
        QCOMPARE(fugue({"project", "new", "Acme", "--in", path("proj/a"), "--json"}).exitCode, 0);
        writeFile(path("proj/a/keep.txt"), "x");
        const Outcome o = fugue({"project", "remove", "acme", "--json"});
        QCOMPARE(o.exitCode, 0);
        QVERIFY(fugue({"project", "list", "--json"}).data().toObject().value("projects").toArray().isEmpty());
        QVERIFY(QFileInfo(path("proj/a/keep.txt")).isFile());
        QVERIFY(QFileInfo(path("proj/a/project.json")).isFile());
        QCOMPARE(fugue({"project", "remove", "acme", "--json"}).errorCode(), QString("unknown_project"));
    }

    void inheritsChangesTheList() {
        QCOMPARE(fugue({"project", "new", "Acme", "--in", path("proj/a"), "--json"}).exitCode, 0);
        const Outcome o = fugue({"project", "inherits", "Acme", "templates,prompts", "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.data().toObject().value("inherits").toArray(), (QJsonArray{"templates", "prompts"}));
        const Outcome bad = fugue({"project", "inherits", "Acme", "models,recovery", "--json"});
        QCOMPARE(bad.exitCode, 0);
        QCOMPARE(bad.json().value("warnings").toArray().size(), 1);
        QCOMPARE(fugue({"project", "inherits", "Acme", "none", "--json"}).data().toObject().value("inherits").toArray().size(), 0);
        QCOMPARE(fugue({"project", "inherits", "nope", "models", "--json"}).errorCode(), QString("unknown_project"));
    }

    void homeWithProjectExitsWithTwo() {
        QCOMPARE(fugue({"project", "new", "Acme", "--in", path("proj/a"), "--json"}).exitCode, 0);
        const Outcome o = fugue({"--home", path("proj/h"), "--project", "Acme", "project", "list"});
        QCOMPARE(o.exitCode, 2);
        QVERIFY(o.err.contains("cannot be used together"));
        QVERIFY(!QFileInfo::exists(path("proj/h")));
        QCOMPARE(fugue({"--project", "nope", "project", "list"}).exitCode, 2);
        QCOMPARE(fugue({"project", "list", "--project"}).exitCode, 2);
    }

    void theCommandLineNeverUsesTheLastProject() {
        QCOMPARE(fugue({"project", "new", "Acme", "--in", path("proj/a"), "--json"}).exitCode, 0);
        QJsonObject registry = QJsonDocument::fromJson(readFile(registryPath())).object();
        registry.insert("last", path("proj/a"));
        writeFile(registryPath(), QJsonDocument(registry).toJson());
        const Outcome o = fugue({"project", "show", "--json"});
        QCOMPARE(o.data().toObject().value("name").toString(), QString("default"));
    }

    void aRunUnderProjectWritesOnlyInsideItsRoot() {
        QCOMPARE(fugue({"project", "new", "Acme", "--in", path("proj/a"), "--json"}).exitCode, 0);
        const QString file = imageWorkflow();
        const QStringList homeBefore = tree(path("home")); // the registry, made by `project new`
        const QStringList workBefore = tree(path("work"));
        const Outcome o = fugue({"--project", "Acme", "run", file, "--wait", "--json"});
        QVERIFY2(o.exitCode == 0, o.out + o.err);
        QCOMPARE(tree(path("home")), homeBefore); // nothing leaked into HOME
        QCOMPARE(tree(path("work")), workBefore);
        QVERIFY2(!QDir(path("proj/a/outputs")).entryList({"proj*.png"}).isEmpty(), qPrintable(tree(path("proj/a")).join('\n')));
        QVERIFY(QDir(path("proj/a/cache/fugue/runs")).exists());
        // The variable does the same.
        QProcessEnvironment env;
        env.insert("FUGUE_PROJECT", "acme");
        const Outcome viaVariable = fugue({"run", file, "--wait", "--json"}, env);
        QVERIFY2(viaVariable.exitCode == 0, viaVariable.out + viaVariable.err);
        QCOMPARE(tree(path("home")), homeBefore);
    }

    void aPathSelectsAProjectThatIsNotRegistered() {
        writeFile(path("proj/x/project.json"), R"({"format":1,"name":"X"})");
        const Outcome o = fugue({"--project", path("proj/x"), "project", "show", "--json"});
        QVERIFY2(o.exitCode == 0, o.out + o.err);
        QCOMPARE(o.data().toObject().value("name").toString(), QString("X"));
        QVERIFY(!o.data().toObject().value("registered").toBool());
        QVERIFY(!QFileInfo(registryPath()).exists()); // selecting registers nothing
        QCOMPARE(fugue({"--project", path("proj"), "project", "show"}).exitCode, 2);
    }

    void helpNamesTheCommandsAndTheOption() {
        const Outcome o = fugue({"--help"});
        QVERIFY(o.out.contains("project new"));
        QVERIFY(o.out.contains("--project"));
    }
};

QTEST_GUILESS_MAIN(TestCliProject)
#include "test_cli_project.moc"
