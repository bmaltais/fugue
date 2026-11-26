// The projects dialog's model (spec 1.4.2, epic #204 step 2 slice 6): the list with the Default first and the
// current project marked, making, adding, renaming and unregistering a project, and switching: a new process
// through an injected launcher (a fake that records what it was asked to start; the real window is never
// started), with the base environment restored, and the window closed afterwards, or kept when it has
// unsaved work. Seam: settings/project_model.h in a real project root (setFugueHome) beside a Default made of
// temporary folders. The environment is the process's, so each test saves and restores what it sets.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/fugue_home.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "settings/project_model.h"

using namespace fugue;

namespace {

const char *const kVariables[] = {"FUGUE_HOME", "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME",
                                  "FUGUE_BASE_XDG_STATE_HOME", "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR",
                                  "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR",
                                  "TMPDIR", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "CUDA_CACHE_PATH",
                                  "__GL_SHADER_DISK_CACHE_PATH", "PIPEWIRE_RUNTIME_DIR", "PULSE_RUNTIME_PATH"};

struct Fake {
    QList<LaunchSpec> launched;
    bool works = true;
    int closed = 0;
    bool unsaved = false;
};

QVariantMap rowNamed(const ProjectModel &model, const QString &name) {
    for (const QVariant &row : model.projects())
        if (row.toMap().value("name").toString() == name)
            return row.toMap();
    return {};
}

} // namespace

class TestProjectModel : public QObject {
    Q_OBJECT

    QHash<QByteArray, QByteArray> m_saved;
    QSet<QByteArray> m_unset;
    QTemporaryDir m_dir;
    Fake m_fake;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    // A model that starts nothing: its launcher records, its window hooks count.
    std::unique_ptr<ProjectModel> make() {
        auto model = std::make_unique<ProjectModel>([this](const LaunchSpec &spec) {
            m_fake.launched << spec;
            return m_fake.works;
        });
        model->setWindow([this] { return m_fake.unsaved; }, [this] { ++m_fake.closed; });
        return model;
    }

    void enterProject(const QString &name, const QStringList &inherits) {
        const Result<ProjectEntry> made = createProject(name, path("p/" + name.toLower()), inherits);
        QVERIFY2(made.ok(), "project not made");
        QVERIFY(setFugueHome(made->root, nullptr));
    }

private slots:
    void init() {
        m_fake = {};
        for (const char *variable : kVariables) {
            if (qEnvironmentVariableIsSet(variable))
                m_saved.insert(variable, qgetenv(variable));
            else
                m_unset.insert(variable);
            qunsetenv(variable);
        }
        qputenv("HOME", path("home").toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", path("base/config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", path("base/data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", path("base/state").toLocal8Bit());
        qputenv("XDG_CACHE_HOME", path("base/cache").toLocal8Bit());
        qputenv("XDG_RUNTIME_DIR", path("base/run").toLocal8Bit());
        QDir().mkpath(path("base/run"));
        QDir().mkpath(path("home"));
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
        QDir(path("home")).removeRecursively();
    }

    // The list

    void theDefaultIsFirstAndCurrentAndHasNoChip() {
        const auto model = make();
        QCOMPARE(model->projects().size(), 1);
        const QVariantMap row = model->projects().first().toMap();
        QVERIFY(row.value("isDefault").toBool());
        QVERIFY(row.value("current").toBool());
        QCOMPARE(row.value("name").toString(), QString("Default project"));
        QVERIFY(model->currentIsDefault());
        QCOMPARE(model->indicator(), QString("Default project"));
        QVERIFY(!model->indicatorShown());
        QVERIFY(model->create("Acme", QString(), {}).value("error").toString().isEmpty());
        QVERIFY(!model->indicatorShown()); // other projects change nothing: no chip reads as the Default
    }

    void aProjectGetsAFolderOfItsOwnInsideTheChosenOneAndTheFolderIsMade() {
        const auto model = make();
        QCOMPARE(model->projectPath("Acme Corp", QString()), path("base/data/fugue/projects/acme-corp"));
        const QString parent = path("clients/new");
        QCOMPARE(model->projectPath("Acme Corp", parent), path("clients/new/acme-corp"));
        QCOMPARE(model->projectPath("Acme Corp", QUrl::fromLocalFile(parent)), path("clients/new/acme-corp"));
        QVERIFY(!QFileInfo::exists(parent)); // nothing is made by asking
        const QVariantMap made = model->create("Acme Corp", model->projectPath("Acme Corp", parent), model->defaultInherits());
        QVERIFY2(made.value("error").toString().isEmpty(), qPrintable(made.value("error").toString()));
        QCOMPARE(made.value("root").toString(), path("clients/new/acme-corp"));
        QVERIFY(QFileInfo::exists(path("clients/new/acme-corp/project.json")));
        QCOMPARE(model->projectPath("Acme Corp", parent), path("clients/new/acme-corp-2")); // a taken folder is not reused
    }

    void aNewProjectInheritsWhatTheDefaultsListSaysAndGoesToTheDefaultPlace() {
        const auto model = make();
        QCOMPARE(model->defaultInherits(), (QStringList{"models", "packs", "preferences"}));
        QCOMPARE(model->defaultLocation("Acme Corp"), path("base/data/fugue/projects/acme-corp"));
        const QVariantMap made = model->create("Acme", QString(), model->defaultInherits());
        QVERIFY2(made.value("error").toString().isEmpty(), qPrintable(made.value("error").toString()));
        QCOMPARE(made.value("root").toString(), path("base/data/fugue/projects/acme"));
        const Result<ProjectManifest> manifest = readProject(made.value("root").toString());
        QVERIFY(manifest.ok());
        QCOMPARE(manifest->inherits, (QStringList{"models", "packs", "preferences"}));
        const QVariantMap row = rowNamed(*model, "Acme");
        QCOMPARE(row.value("state").toString(), QString("ok"));
        QVERIFY(!row.value("current").toBool());
        QCOMPARE(row.value("inherits").toStringList(), (QStringList{"models", "packs", "preferences"}));
    }

    void aChosenFolderIsTheRootAndAFolderUrlWorks() {
        const auto model = make();
        const QVariantMap made = model->create("Client", QUrl::fromLocalFile(path("clients/one")), {"models"});
        QVERIFY2(made.value("error").toString().isEmpty(), qPrintable(made.value("error").toString()));
        QCOMPARE(made.value("root").toString(), path("clients/one"));
        QVERIFY(QFileInfo::exists(path("clients/one/project.json")));
    }

    void badAndTakenNamesAreRefusedWithASentence() {
        const auto model = make();
        QVERIFY(model->create("Acme", QString(), {}).value("error").toString().isEmpty());
        const QString taken = model->create("acme", QString(), {}).value("error").toString();
        QVERIFY(taken.contains("registered already"));
        QVERIFY(!taken.contains("fugue project")); // no command line hints in a window
        QVERIFY(model->create("a/b", QString(), {}).value("error").toString().contains("not a name"));
        QVERIFY(model->create("Default", QString(), {}).value("error").toString().contains("not a name"));
        QCOMPARE(model->projects().size(), 2);
    }

    void aGoneOrBrokenProjectListsAsMissingOrInvalidAndStaysListed() {
        const auto model = make();
        const QString gone = model->create("Gone", QString(), {}).value("root").toString();
        const QString broken = model->create("Broken", QString(), {}).value("root").toString();
        QVERIFY(QFile::remove(gone + "/project.json"));
        QVERIFY(QFile::remove(broken + "/project.json"));
        QVERIFY(QFile(broken + "/project.json").open(QIODevice::WriteOnly));
        model->refresh();
        QCOMPARE(model->projects().size(), 3);
        const QVariantMap missing = model->projects().at(1).toMap();
        QCOMPARE(missing.value("state").toString(), QString("missing"));
        QVERIFY(missing.value("stateText").toString().contains("Missing"));
        const QVariantMap invalid = model->projects().at(2).toMap();
        QCOMPARE(invalid.value("state").toString(), QString("invalid"));
        QVERIFY(!invalid.value("stateText").toString().isEmpty());
    }

    void openFolderRegistersAnExistingProjectAndRefusesAFolderWithoutOne() {
        ProjectManifest manifest;
        manifest.name = "Elsewhere";
        QVERIFY(QDir().mkpath(path("drive/elsewhere")));
        QVERIFY(writeProject(path("drive/elsewhere"), manifest).isEmpty());
        const auto model = make();
        QVERIFY(model->addFolder(QUrl::fromLocalFile(path("drive/elsewhere"))).isEmpty());
        QCOMPARE(rowNamed(*model, "Elsewhere").value("path").toString(), path("drive/elsewhere"));
        QVERIFY(model->addFolder(path("home")).contains("no project.json"));
        QVERIFY(!model->addFolder(QString()).isEmpty());
    }

    void removeOnlyUnregistersAndTheFilesStay() {
        const auto model = make();
        const QString root = model->create("Acme", QString(), {}).value("root").toString();
        QVERIFY(QDir().mkpath(root + "/outputs"));
        QVERIFY(QFile(root + "/outputs/keep.png").open(QIODevice::WriteOnly));
        QVERIFY(model->remove(root).isEmpty());
        QVERIFY(rowNamed(*model, "Acme").isEmpty());
        QVERIFY(QFileInfo::exists(root + "/project.json"));
        QVERIFY(QFileInfo::exists(root + "/outputs/keep.png"));
        QVERIFY(!model->remove(QString()).isEmpty()); // the Default is not in the list
        QVERIFY(!model->remove(root).isEmpty());      // not registered any more
    }

    void aProjectStartedFromItsFolderIsListedAsCurrentEvenUnregistered() {
        ProjectManifest manifest;
        manifest.name = "Loose";
        QVERIFY(QDir().mkpath(path("loose")));
        QVERIFY(writeProject(path("loose"), manifest).isEmpty());
        QVERIFY(setFugueHome(path("loose"), nullptr));
        const auto model = make();
        QVERIFY(!model->currentIsDefault());
        QCOMPARE(model->currentName(), QString("Loose"));
        QCOMPARE(model->indicator(), QString("Loose"));
        QVERIFY(model->indicatorShown());
        const QVariantMap row = rowNamed(*model, "Loose");
        QVERIFY(row.value("current").toBool());
        QVERIFY(!row.value("registered").toBool());
        QVERIFY(!model->projects().first().toMap().value("current").toBool()); // the Default is not
        QVERIFY(model->projects().first().toMap().value("isDefault").toBool());
    }

    // Project settings

    void theDefaultHasNothingToChange() {
        const auto model = make();
        QVERIFY(model->setCurrent("Anything", {"models"}).contains("Default project"));
    }

    void settingsRenameAndChangeWhatIsInherited() {
        enterProject("Acme", {"models"});
        (void)createProject("Other", QString(), {});
        const auto model = make();
        QCOMPARE(model->currentName(), QString("Acme"));
        QCOMPARE(model->currentInherits(), (QStringList{"models"}));
        QSignalSpy changed(model.get(), &ProjectModel::changed);
        QVERIFY2(model->setCurrent("Acme Corp", {"models", "prompts"}).isEmpty(), "set");
        QVERIFY(changed.count() > 0);
        QCOMPARE(model->currentName(), QString("Acme Corp"));
        QCOMPARE(model->currentInherits(), (QStringList{"models", "prompts"}));
        QCOMPARE(sessionInherits(), (QStringList{"models", "prompts"}));
        QVERIFY(model->setCurrent("other", {}).contains("registered already")); // another project's name
        QCOMPARE(model->currentName(), QString("Acme Corp"));
        QVERIFY(model->setCurrent("a.b", {}).contains("not a name"));
    }

    // Switching

    void switchingStartsANewProcessWithTheBaseEnvironmentAndThenClosesThisWindow() {
        const auto model = make();
        const QString root = model->create("Acme", QString(), {}).value("root").toString();
        // This window runs in another project: its environment is redirected, with the base saved.
        enterProject("Current", {});
        qputenv("FUGUE_PROJECT", "x");
        qputenv("CUDA_CACHE_PATH", "/redirected");
        model->refresh();
        QVERIFY(model->switchTo(root).isEmpty());
        QCOMPARE(m_fake.launched.size(), 1);
        QCOMPARE(m_fake.closed, 1);
        const LaunchSpec spec = m_fake.launched.first();
        QCOMPARE(spec.program, QCoreApplication::applicationFilePath());
        QCOMPARE(spec.arguments, (QStringList{"--project", root}));
        const QProcessEnvironment env = spec.environment;
        QCOMPARE(env.value("XDG_CONFIG_HOME"), path("base/config"));
        QCOMPARE(env.value("XDG_DATA_HOME"), path("base/data"));
        QCOMPARE(env.value("XDG_STATE_HOME"), path("base/state"));
        QCOMPARE(env.value("XDG_CACHE_HOME"), path("base/cache"));
        QCOMPARE(env.value("XDG_RUNTIME_DIR"), path("base/run"));
        QVERIFY(!env.contains("FUGUE_HOME"));
        QVERIFY(!env.contains("FUGUE_PROJECT"));
        QVERIFY(!env.contains("FUGUE_BASE_XDG_CONFIG_HOME"));
        QVERIFY(!env.contains("TMPDIR"));
        QVERIFY(!env.contains("CUDA_CACHE_PATH"));
    }

    void switchingToTheDefaultNamesItSoTheLastProjectIsNotReopened() {
        enterProject("Current", {});
        const auto model = make();
        QVERIFY(model->switchTo(QString()).isEmpty());
        QCOMPARE(m_fake.launched.first().arguments, (QStringList{"--project", "default"}));
        QCOMPARE(m_fake.closed, 1);
    }

    void switchingToTheCurrentProjectIsRefusedAndStartsNothing() {
        const auto model = make();
        QVERIFY(model->switchTo(QString()).contains("already"));
        enterProject("Acme", {});
        model->refresh();
        QVERIFY(model->switchTo(model->currentRoot()).contains("already"));
        QVERIFY(m_fake.launched.isEmpty());
        QCOMPARE(m_fake.closed, 0);
    }

    void aMissingProjectCannotBeSwitchedToAndNothingStarts() {
        const auto model = make();
        const QString root = model->create("Gone", QString(), {}).value("root").toString();
        QVERIFY(QFile::remove(root + "/project.json"));
        QVERIFY(!model->switchTo(root).isEmpty());
        QVERIFY(!model->openInNewWindow(root).isEmpty());
        QVERIFY(m_fake.launched.isEmpty());
        QCOMPARE(m_fake.closed, 0);
    }

    void aLauncherThatFailsKeepsThisWindowOpen() {
        const auto model = make();
        const QString root = model->create("Acme", QString(), {}).value("root").toString();
        m_fake.works = false;
        QVERIFY(model->switchTo(root).contains("could not start"));
        QCOMPARE(m_fake.closed, 0);
    }

    void unsavedWorkKeepsTheWindowAndOffersANewWindow() {
        const auto model = make();
        const QString root = model->create("Acme", QString(), {}).value("root").toString();
        m_fake.unsaved = true;
        QVERIFY(model->unsavedWork());
        QSignalSpy blocked(model.get(), &ProjectModel::switchBlocked);
        QVERIFY(model->switchTo(root).isEmpty());
        QCOMPARE(blocked.count(), 1);
        QCOMPARE(blocked.first().at(0).toString(), root);
        QCOMPARE(blocked.first().at(1).toString(), QString("Acme"));
        QVERIFY(m_fake.launched.isEmpty()); // nothing started, nothing closed
        QCOMPARE(m_fake.closed, 0);
        QVERIFY(model->openInNewWindow(root).isEmpty());
        QCOMPARE(m_fake.launched.size(), 1);
        QCOMPARE(m_fake.closed, 0); // this window stays
    }

    void baseEnvironmentLeavesTheDefaultSessionAsItIs() {
        QProcessEnvironment env;
        env.insert("XDG_CONFIG_HOME", "/home/u/.config");
        env.insert("TMPDIR", "/tmp/mine");
        const QProcessEnvironment out = baseEnvironment(env);
        QCOMPARE(out.value("XDG_CONFIG_HOME"), QString("/home/u/.config"));
        QCOMPARE(out.value("TMPDIR"), QString("/tmp/mine")); // not redirected, so not taken away
    }

    void renamingAProjectNeedsAFreeNameAndKeepsItsFolder() {
        const Result<ProjectEntry> a = createProject("A", QString(), {});
        const Result<ProjectEntry> b = createProject("B", QString(), {});
        QVERIFY(a.ok() && b.ok());
        QVERIFY(!renameProject(b->root, "a").ok());
        const Result<ProjectEntry> renamed = renameProject(b->root, "C");
        QVERIFY(renamed.ok());
        QCOMPARE(renamed->name(), QString("C"));
        QCOMPARE(renamed->root, b->root);
        QVERIFY(!renameProject(b->root, "default").ok());
    }
};

QTEST_GUILESS_MAIN(TestProjectModel)
#include "test_project_model.moc"
