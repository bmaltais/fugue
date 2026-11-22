// Projects (spec 1.4.2, epic #204): project.json, the registry in the BASE config folder, the base
// folders setFugueHome saves, and the selection order of applyFugueHome. Seam: engine/project.h and
// engine/fugue_home.h. The environment is the process's, so each test saves and restores what it sets.
// The commands are tests/cli/test_cli_project.cpp.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <vector>

#include "engine/fugue_home.h"
#include "engine/paths.h"
#include "engine/project.h"

using namespace fugue;

namespace {

const char *const kVariables[] = {"FUGUE_HOME",       "FUGUE_PROJECT",   "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME",
                                  "FUGUE_BASE_XDG_STATE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME",     "XDG_STATE_HOME",
                                  "XDG_CACHE_HOME",   "XDG_RUNTIME_DIR", "TMPDIR",                     "WAYLAND_DISPLAY",
                                  "DBUS_SESSION_BUS_ADDRESS", "CUDA_CACHE_PATH", "__GL_SHADER_DISK_CACHE_PATH",
                                  "PIPEWIRE_RUNTIME_DIR", "PULSE_RUNTIME_PATH"};

struct Args {
    std::vector<QByteArray> words;
    std::vector<char *> pointers;
    int argc = 0;
    explicit Args(const QList<QByteArray> &list) : words(list.begin(), list.end()) {
        for (QByteArray &word : words)
            pointers.push_back(word.data());
        pointers.push_back(nullptr);
        argc = int(words.size());
    }
    QStringList left() const {
        QStringList out;
        for (int i = 0; i < argc; ++i)
            out << QString::fromLocal8Bit(pointers[size_t(i)]);
        return out;
    }
};

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

} // namespace

class TestProject : public QObject {
    Q_OBJECT

    QHash<QByteArray, QByteArray> m_saved;
    QSet<QByteArray> m_unset;
    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

private slots:
    void init() {
        for (const char *variable : kVariables) {
            if (qEnvironmentVariableIsSet(variable))
                m_saved.insert(variable, qgetenv(variable));
            else
                m_unset.insert(variable);
            qunsetenv(variable);
        }
        // The ordinary session of this test: its own XDG folders.
        qputenv("XDG_CONFIG_HOME", path("base/config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", path("base/data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", path("base/state").toLocal8Bit());
    }
    void cleanup() {
        for (auto it = m_saved.begin(); it != m_saved.end(); ++it)
            qputenv(it.key().constData(), it.value());
        for (const QByteArray &variable : std::as_const(m_unset))
            qunsetenv(variable.constData());
        m_saved.clear();
        m_unset.clear();
        QDir(path("base")).removeRecursively();
        QDir(path("p")).removeRecursively();
    }

    // project.json

    void aManifestIsParsed() {
        const auto m = parseProjectManifest(R"({"format":1,"name":"Acme","inherits":["models","packs"],"note":"x"})", "project.json");
        QVERIFY(m.ok());
        QCOMPARE(m->name, QString("Acme"));
        QCOMPARE(m->inherits, (QStringList{"models", "packs"}));
        QVERIFY(m->ignored.isEmpty());
        QVERIFY(m.warnings().isEmpty());
    }

    void inheritsMayBeLeftOut() {
        const auto m = parseProjectManifest(R"({"format":1,"name":"Acme"})", "project.json");
        QVERIFY(m.ok());
        QVERIFY(m->inherits.isEmpty());
    }

    void unknownAndForbiddenItemsAreIgnoredAndReported() {
        const auto m = parseProjectManifest(R"({"format":1,"name":"Acme","inherits":["models","providers","keys","colour","models","outputs"]})", "p");
        QVERIFY(m.ok()); // not fatal
        QCOMPARE(m->inherits, (QStringList{"models"}));
        QCOMPARE(m->ignored, (QStringList{"providers", "keys", "colour", "outputs"}));
        QCOMPARE(m.warnings().size(), 4);
        for (const Error &warning : m.warnings()) {
            QCOMPARE(warning.code, QString("project_inherits_ignored"));
            QCOMPARE(warning.severity, Error::Severity::Warning);
        }
        QVERIFY(m.warnings().at(0).message.contains("never be inherited"));
        QVERIFY(m.warnings().at(2).message.contains("not something"));
    }

    void everyNeverInheritableItemIsRefusedAndNoneIsInTheVocabulary() {
        for (const QString &item : neverInheritableItems())
            QVERIFY2(!inheritableItems().contains(item), qPrintable(item));
        QVERIFY(neverInheritableItems().contains("providers"));
        QVERIFY(neverInheritableItems().contains("recovery"));
        QCOMPARE(inheritableItems(), (QStringList{"models", "packs", "templates", "prompts", "preferences"}));
    }

    void badManifestsAreRefused() {
        QCOMPARE(parseProjectManifest("not json", "p").error().code, QString("invalid_json"));
        QCOMPARE(parseProjectManifest("[]", "p").error().code, QString("project_invalid"));
        QCOMPARE(parseProjectManifest(R"({"format":2,"name":"A"})", "p").error().code, QString("unsupported_format_version"));
        QCOMPARE(parseProjectManifest(R"({"name":"A"})", "p").error().code, QString("project_invalid"));
        QCOMPARE(parseProjectManifest(R"({"format":1})", "p").error().code, QString("project_invalid"));
        QCOMPARE(parseProjectManifest(R"({"format":1,"name":"a/b"})", "p").error().code, QString("project_invalid"));
        QCOMPARE(parseProjectManifest(R"({"format":1,"name":"Default"})", "p").error().code, QString("project_invalid"));
        QCOMPARE(parseProjectManifest(R"({"format":1,"name":"A","inherits":"models"})", "p").error().code, QString("project_invalid"));
        QCOMPARE(parseProjectManifest(R"({"format":1,"name":"A","inherits":[1]})", "p").error().code, QString("project_invalid"));
    }

    void namesAndSlugs() {
        QVERIFY(projectNameProblem("Acme").isEmpty());
        QVERIFY(projectNameProblem("Client work 2").isEmpty());
        for (const char *bad : {"", " ", " Acme", "a/b", "a\\b", "v1.2", ".hidden", "..", "default", "DEFAULT", "Default"})
            QVERIFY2(!projectNameProblem(bad).isEmpty(), bad);
        QVERIFY(!projectNameProblem(QString(65, 'a')).isEmpty());
        QCOMPARE(projectSlug("Acme Corp!"), QString("acme-corp"));
        QCOMPARE(projectSlug("  --A  B--"), QString("a-b"));
        QCOMPARE(projectSlug("日本語"), QString("project"));
    }

    void writingKeepsKeysThisVersionDoesNotKnow() {
        QVERIFY(QDir().mkpath(path("p/a")));
        write(path("p/a/project.json"), R"({"format":1,"name":"A","future":{"x":1}})");
        auto m = readProject(path("p/a"));
        QVERIFY(m.ok());
        m->inherits = {"templates"};
        QVERIFY(!hasErrors(writeProject(path("p/a"), *m)));
        const auto again = readProject(path("p/a"));
        QCOMPARE(again->inherits, (QStringList{"templates"}));
        QCOMPARE(again->raw.value("future").toObject().value("x").toInt(), 1);
    }

    // Base folders

    void baseDirsAreTheOrdinarySessionsWithoutARoot() {
        QCOMPARE(baseConfigDir(), configDir());
        QCOMPARE(baseDataDir(), dataDir());
        QCOMPARE(baseStateDir(), stateDir());
    }

    void baseDirsSurviveASecondSetFugueHome() {
        const QString config = configDir(), data = dataDir(), state = stateDir();
        QVERIFY(setFugueHome(path("p/one"), nullptr));
        QCOMPARE(configDir(), path("p/one/config/fugue"));
        QCOMPARE(baseConfigDir(), config);
        QCOMPARE(baseDataDir(), data);
        QCOMPARE(baseStateDir(), state);
        QCOMPARE(qgetenv("FUGUE_BASE_XDG_CONFIG_HOME"), path("base/config").toLocal8Bit());
        QVERIFY(setFugueHome(path("p/two"), nullptr)); // never overwritten
        QCOMPARE(configDir(), path("p/two/config/fugue"));
        QCOMPARE(baseConfigDir(), config);
        QCOMPARE(baseDataDir(), data);
        QCOMPARE(baseStateDir(), state);
    }

    void aFailedSetFugueHomeSavesNothing() {
        QVERIFY(!setFugueHome("relative", nullptr));
        QVERIFY(!qEnvironmentVariableIsSet("FUGUE_BASE_XDG_CONFIG_HOME"));
    }

    // The registry

    void noRegistryIsAnEmptyOneAndNothingIsWrittenToRead() {
        const auto registry = readProjectRegistry();
        QVERIFY(registry.ok());
        QVERIFY(registry->projects.isEmpty());
        QVERIFY(!projectRegistryExists());
    }

    void theRegistryRoundTrips() {
        ProjectRegistry registry;
        registry.projects = {path("p/a"), path("p/b")};
        registry.last = path("p/b");
        QVERIFY(!hasErrors(writeProjectRegistry(registry)));
        QCOMPARE(projectRegistryPath(), path("base/config/fugue/projects.json"));
        const auto again = readProjectRegistry();
        QVERIFY(again.ok());
        QCOMPARE(again->projects, registry.projects);
        QCOMPARE(again->last, registry.last);
    }

    void aBrokenRegistryIsAnErrorAndNeverReplaced() {
        write(projectRegistryPath(), "{oops");
        QCOMPARE(readProjectRegistry().error().code, QString("invalid_json"));
        QVERIFY(!createProject("A", path("p/a"), {}).ok());
        QCOMPARE(read(projectRegistryPath()), QByteArray("{oops"));
    }

    void theRegistryIsAlwaysTheBaseOneEvenInsideAProject() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        QVERIFY(setFugueHome(path("p/a"), nullptr));
        QCOMPARE(projectRegistryPath(), path("base/config/fugue/projects.json"));
        const auto listed = listProjects();
        QVERIFY(listed.ok());
        QCOMPARE(listed->size(), 1);
        QVERIFY(QFileInfo(path("p/a/config/fugue/projects.json")).exists() == false);
    }

    // Operations

    void newMakesTheFileAndRegisters() {
        const auto made = createProject("Acme", path("p/acme"), {"models", "preferences"});
        QVERIFY(made.ok());
        QCOMPARE(made->root, path("p/acme"));
        QVERIFY(made->registered);
        QVERIFY(QFileInfo(path("p/acme/project.json")).isFile());
        const auto listed = listProjects();
        QCOMPARE(listed->size(), 1);
        QCOMPARE(listed->at(0).name(), QString("Acme"));
        QCOMPARE(listed->at(0).manifest.inherits, (QStringList{"models", "preferences"}));
        QCOMPARE(listed->at(0).state, ProjectState::Ok);
    }

    void theDefaultLocationIsTheBaseDataFolderAndCollisionsGetANumber() {
        const auto one = createProject("Client Work", {}, {});
        QVERIFY(one.ok());
        QCOMPARE(one->root, path("base/data/fugue/projects/client-work"));
        // A folder with that slug is there already (another name, or a stray folder).
        QVERIFY(QDir().mkpath(path("base/data/fugue/projects/client-work-2")));
        const auto two = createProject("client work", {}, {});
        QVERIFY(!two.ok()); // the NAME is taken, case-insensitively
        QCOMPARE(two.error().code, QString("project_exists"));
        const auto three = createProject("Client-Work", {}, {});
        QVERIFY(three.ok());
        QCOMPARE(three->root, path("base/data/fugue/projects/client-work-3"));
    }

    void defaultIsReservedAndBadNamesAreRefused() {
        QCOMPARE(createProject("default", path("p/d"), {}).error().code, QString("project_invalid"));
        QCOMPARE(createProject("Default", path("p/d"), {}).error().code, QString("project_invalid"));
        QCOMPARE(createProject("a/b", path("p/d"), {}).error().code, QString("project_invalid"));
        QVERIFY(!QFileInfo::exists(path("p/d")));
        QVERIFY(!projectRegistryExists());
    }

    void newRefusesAFolderThatIsAProjectAlready() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        QCOMPARE(createProject("B", path("p/a"), {}).error().code, QString("project_exists"));
        QCOMPARE(listProjects()->size(), 1);
    }

    void newReportsIgnoredInheritsAsWarnings() {
        const auto made = createProject("A", path("p/a"), {"models", "providers"});
        QVERIFY(made.ok());
        QCOMPARE(made.warnings().size(), 1);
        QCOMPARE(made->manifest.inherits, (QStringList{"models"}));
    }

    void addRegistersAnExistingProjectOnceAndNeedsAProjectJson() {
        write(path("p/x/project.json"), R"({"format":1,"name":"X"})");
        QVERIFY(addProject(path("p/x")).ok());
        QVERIFY(addProject(path("p/x")).ok()); // again is fine
        QCOMPARE(listProjects()->size(), 1);
        QCOMPARE(addProject(path("p/nothing")).error().code, QString("project_invalid"));
        QVERIFY(QDir().mkpath(path("p/empty")));
        QCOMPARE(addProject(path("p/empty")).error().code, QString("project_invalid"));
        write(path("p/y/project.json"), R"({"format":1,"name":"x"})");
        QCOMPARE(addProject(path("p/y")).error().code, QString("project_exists")); // same name as X
        write(path("p/z/project.json"), "{");
        QCOMPARE(addProject(path("p/z")).error().code, QString("invalid_json"));
    }

    void aGonePathListsAsMissingAndStaysRegistered() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        QVERIFY(QDir(path("p/a")).removeRecursively());
        const auto listed = listProjects();
        QVERIFY(listed.ok());
        QCOMPARE(listed->size(), 1);
        QCOMPARE(listed->at(0).state, ProjectState::Missing);
        QCOMPARE(listed->at(0).name(), QString("a")); // the folder's name: the manifest is gone
        QCOMPARE(resolveProjectRoot(path("p/a")).error().code, QString("file_not_found"));
        QVERIFY(findProject(path("p/a")).ok()); // by its path, it is found
    }

    void aBrokenProjectJsonListsAsInvalid() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        write(path("p/a/project.json"), "{");
        QCOMPARE(listProjects()->at(0).state, ProjectState::Invalid);
        QVERIFY(!resolveProjectRoot(path("p/a")).ok());
    }

    void removeOnlyUnregisters() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        write(path("p/a/keep.txt"), "x");
        const auto removed = removeProject("a"); // case-insensitive
        QVERIFY(removed.ok());
        QVERIFY(listProjects()->isEmpty());
        QVERIFY(QFileInfo(path("p/a/keep.txt")).isFile());
        QVERIFY(QFileInfo(path("p/a/project.json")).isFile());
        QCOMPARE(removeProject("a").error().code, QString("unknown_project"));
    }

    void findByNameOrPathAndAmbiguity() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        QCOMPARE(findProject("A")->root, path("p/a"));
        QCOMPARE(findProject(path("p/a"))->root, path("p/a"));
        QCOMPARE(findProject("nope").error().code, QString("unknown_project"));
        QCOMPARE(findProject(path("p/none")).error().code, QString("unknown_project"));
        // Two registered folders that came to share a name (a project.json edited by hand).
        write(path("p/b/project.json"), R"({"format":1,"name":"B"})");
        QVERIFY(addProject(path("p/b")).ok());
        write(path("p/b/project.json"), R"({"format":1,"name":"A"})");
        QCOMPARE(findProject("a").error().code, QString("usage"));
        QCOMPARE(findProject(path("p/b"))->name(), QString("A")); // a path is exact
    }

    void inheritsIsReplacedAndReported() {
        QVERIFY(createProject("A", path("p/a"), {"models"}).ok());
        const auto changed = setProjectInherits("A", {"packs", "recovery", "templates"});
        QVERIFY(changed.ok());
        QCOMPARE(changed->manifest.inherits, (QStringList{"packs", "templates"}));
        QCOMPARE(changed.warnings().size(), 1);
        QCOMPARE(readProject(path("p/a"))->inherits, (QStringList{"packs", "templates"}));
        QVERIFY(setProjectInherits("A", {}).ok());
        QVERIFY(readProject(path("p/a"))->inherits.isEmpty());
    }

    // Selection

    void projectOptionSelectsByNameAndByPath() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        {
            Args args({"fugue", "--project", "a", "runs", "list"});
            QString error;
            QVERIFY2(applyFugueHome(args.argc, args.pointers.data(), &error), qPrintable(error));
            QCOMPARE(args.left(), (QStringList{"fugue", "runs", "list"}));
            QCOMPARE(fugueHome(), path("p/a"));
            QCOMPARE(configDir(), path("p/a/config/fugue"));
        }
        cleanup();
        init();
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        Args args({"fugue", "runs", QByteArray("--project=") + path("p/a").toLocal8Bit()});
        QString error;
        QVERIFY2(applyFugueHome(args.argc, args.pointers.data(), &error), qPrintable(error));
        QCOMPARE(args.left(), (QStringList{"fugue", "runs"}));
        QCOMPARE(fugueHome(), path("p/a"));
    }

    void homeWithProjectIsAnError() {
        Args args({"fugue", "--home", "/x", "--project", "a", "runs", "list"});
        QString error;
        QVERIFY(!applyFugueHome(args.argc, args.pointers.data(), &error));
        QVERIFY(error.contains("--home and --project"));
        QVERIFY(fugueHome().isEmpty());
    }

    void unknownProjectAndMissingValueAreErrors() {
        {
            Args args({"fugue", "--project", "nope", "runs"});
            QString error;
            QVERIFY(!applyFugueHome(args.argc, args.pointers.data(), &error));
            QVERIFY(error.contains("no project"));
        }
        {
            Args args({"fugue", "--project"});
            QString error;
            QVERIFY(!applyFugueHome(args.argc, args.pointers.data(), &error));
            QVERIFY(error.contains("needs a value"));
        }
        QVERIFY(fugueHome().isEmpty());
    }

    void aPathWithoutAProjectJsonIsNotAProject() {
        QVERIFY(QDir().mkpath(path("p/bare")));
        Args args({"fugue", "--project", path("p/bare").toLocal8Bit(), "runs"});
        QString error;
        QVERIFY(!applyFugueHome(args.argc, args.pointers.data(), &error));
        QVERIFY(fugueHome().isEmpty());
    }

    void precedenceHomeOptionThenProjectOptionThenEnvironment() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        QVERIFY(createProject("B", path("p/b"), {}).ok());
        QVERIFY(createProject("C", path("p/c"), {}).ok());
        // --project beats FUGUE_HOME and FUGUE_PROJECT
        qputenv("FUGUE_HOME", path("p/c").toLocal8Bit());
        qputenv("FUGUE_PROJECT", "c");
        Args first({"fugue", "--project", "a"});
        QString error;
        QVERIFY2(applyFugueHome(first.argc, first.pointers.data(), &error), qPrintable(error));
        QCOMPARE(fugueHome(), path("p/a"));
        // --home beats the variables
        qputenv("FUGUE_HOME", path("p/c").toLocal8Bit());
        Args second({"fugue", "--home", path("p/b").toLocal8Bit()});
        QVERIFY2(applyFugueHome(second.argc, second.pointers.data(), &error), qPrintable(error));
        QCOMPARE(fugueHome(), path("p/b"));
        // FUGUE_HOME beats FUGUE_PROJECT
        qputenv("FUGUE_HOME", path("p/c").toLocal8Bit());
        qputenv("FUGUE_PROJECT", "a");
        Args third({"fugue"});
        QVERIFY2(applyFugueHome(third.argc, third.pointers.data(), &error), qPrintable(error));
        QCOMPARE(fugueHome(), path("p/c"));
    }

    void fugueProjectSelectsWhenNothingElseDoes() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        qputenv("FUGUE_PROJECT", "A");
        Args args({"fugue", "runs"});
        QString error;
        QVERIFY2(applyFugueHome(args.argc, args.pointers.data(), &error), qPrintable(error));
        QCOMPARE(fugueHome(), path("p/a"));
    }

    void defaultByNameMeansNoRootAndNoLastUsed() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        ProjectRegistry registry = *readProjectRegistry();
        registry.last = path("p/a");
        QVERIFY(!hasErrors(writeProjectRegistry(registry)));
        Args args({"fugue", "--project", "default"});
        QString error;
        QVERIFY2(applyFugueHome(args.argc, args.pointers.data(), &error), qPrintable(error));
        QVERIFY(fugueHome().isEmpty());
        QVERIFY(applyLastUsedProject(&error)); // an explicit Default is not replaced by the last used
        QVERIFY(fugueHome().isEmpty());
    }

    void defaultFromInsideAProjectIsRefused() {
        qputenv("FUGUE_HOME", path("p/a").toLocal8Bit());
        Args args({"fugue", "--project", "default"});
        QString error;
        QVERIFY(!applyFugueHome(args.argc, args.pointers.data(), &error));
        QVERIFY(error.contains("inside a project"));
    }

    // Last used: the window only

    void lastUsedOpensTheLastProjectWhenNothingSelected() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        ProjectRegistry registry = *readProjectRegistry();
        registry.last = path("p/a");
        QVERIFY(!hasErrors(writeProjectRegistry(registry)));
        // The command line's selection never looks at it.
        Args args({"fugue", "runs"});
        QString error;
        QVERIFY(applyFugueHome(args.argc, args.pointers.data(), &error));
        QVERIFY(fugueHome().isEmpty());
        // The window then asks for it.
        QVERIFY2(applyLastUsedProject(&error), qPrintable(error));
        QCOMPARE(fugueHome(), path("p/a"));
    }

    void aGoneLastUsedProjectMeansDefault() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        ProjectRegistry registry = *readProjectRegistry();
        registry.last = path("p/a");
        QVERIFY(!hasErrors(writeProjectRegistry(registry)));
        QDir(path("p/a")).removeRecursively();
        QString error;
        QVERIFY(applyLastUsedProject(&error));
        QVERIFY(fugueHome().isEmpty());
        QVERIFY(lastUsedProjectRoot().isEmpty());
    }

    void recordingTheLastUsedProject() {
        // Default with no registry: nothing is created.
        recordLastUsedProject();
        QVERIFY(!projectRegistryExists());
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        write(path("p/bare/x"), "");
        QVERIFY(setFugueHome(path("p/a"), nullptr));
        recordLastUsedProject();
        QCOMPARE(readProjectRegistry()->last, path("p/a"));
        // An anonymous --home leaves it alone.
        QVERIFY(setFugueHome(path("p/bare"), nullptr));
        recordLastUsedProject();
        QCOMPARE(readProjectRegistry()->last, path("p/a"));
        // Default forgets it.
        qunsetenv("FUGUE_HOME");
        recordLastUsedProject();
        QVERIFY(readProjectRegistry()->last.isEmpty());
        QVERIFY(readProjectRegistry()->projects.contains(path("p/a")));
    }

    void removingTheLastUsedProjectForgetsIt() {
        QVERIFY(createProject("A", path("p/a"), {}).ok());
        ProjectRegistry registry = *readProjectRegistry();
        registry.last = path("p/a");
        QVERIFY(!hasErrors(writeProjectRegistry(registry)));
        QVERIFY(removeProject("A").ok());
        QVERIFY(readProjectRegistry()->last.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestProject)
#include "test_project.moc"
