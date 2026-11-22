// Projects in the real window (spec 1.4.2, epic #204 step 2 slice 6), offscreen: the title and the project
// chip, the Projects dialog (list, New, Project settings, Remove from list, the unsaved-work question), and
// the 'Inherited from Default' tags of the settings panel, the model and pack folders and the Packs page.
// Switching goes through a fake launcher that records what it would start: no window is ever launched, and
// no real pointer is moved. The folders (the Default's, the projects', the settings) are temporary; with
// FUGUE_SHOT_DIR set the last test saves the window as pictures.

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QQuickStyle>
#include <QtTest>

#include "../disk_pack.h"
#include "canvas_app.h"
#include "engine/fugue_home.h"
#include "engine/json.h"
#include "engine/settings.h"
#include "engine/project.h"
#include "settings/project_model.h"

using namespace canvas_test;
using namespace fugue;

namespace {

const QByteArray emptyDoc = R"({"fugue": 1, "graph": {"nodes": {}}})";

const char *const kVariables[] = {"FUGUE_HOME", "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME",
                                  "FUGUE_BASE_XDG_STATE_HOME", "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR",
                                  "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR",
                                  "TMPDIR", "CUDA_CACHE_PATH", "__GL_SHADER_DISK_CACHE_PATH"};

QQuickItem *visibleItem(QQuickItem *from, const QString &name) {
    for (QQuickItem *child : from->childItems()) {
        if (!child->isVisible())
            continue;
        if (child->objectName() == name)
            return child;
        if (QQuickItem *found = visibleItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *find(QQuickWindow *window, const QString &name) {
    QQuickItem *root = window->contentItem();
    while (root->parentItem())
        root = root->parentItem();
    return visibleItem(root, name);
}

QPoint centerOf(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

bool open(QQuickWindow *window, const QString &name) {
    QObject *popup = window->findChild<QObject *>(name);
    return popup && popup->property("opened").toBool();
}

#define NEED(name)                                                                                                     \
    [&] {                                                                                                              \
        QQuickItem *found = nullptr;                                                                                   \
        [&] { QTRY_VERIFY2((found = find(app.window, name)), qPrintable(QStringLiteral("no %1").arg(name))); }();      \
        return found;                                                                                                  \
    }()

// Scrolls the flickable that holds `item` until all of it shows.
bool scrollIntoView(QQuickItem *item) {
    QQuickItem *view = item->parentItem();
    while (view && !view->inherits("QQuickFlickable"))
        view = view->parentItem();
    if (!view)
        return false;
    const QRectF in = item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
    qreal y = view->property("contentY").toReal();
    if (in.bottom() > view->height())
        y += in.bottom() - view->height();
    else if (in.top() < 0)
        y += in.top();
    else
        return false;
    view->setProperty("contentY", qBound(0.0, y, qMax(0.0, view->property("contentHeight").toReal() - view->height())));
    return true;
}

// Clicks an item once the layouts have placed it (looked up again by name after every wait: a list that
// changes rebuilds its rows).
void click(App &app, QQuickItem *item) {
    const QString name = item->objectName();
    QPoint at = centerOf(item);
    int still = 0;
    for (int i = 0; still < 3 && i < 500; ++i) {
        QTest::qWait(20);
        QQuickItem *now = find(app.window, name);
        if (!now) {
            still = 0;
            continue;
        }
        if (scrollIntoView(now))
            still = 0;
        const QPoint here = centerOf(now);
        still = here == at ? still + 1 : 0;
        at = here;
    }
    QVERIFY2(still >= 3, qPrintable(QStringLiteral("%1 never settled").arg(name)));
    QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
}

void type(App &app, QQuickItem *field, const QString &text) {
    click(app, field);
    QTest::keySequence(app.window, QKeySequence::SelectAll);
    QTest::keyClick(app.window, Qt::Key_Backspace);
    for (const QChar c : text)
        QTest::keyClick(app.window, c.toLatin1());
}

QString textOf(QQuickItem *item) { return item->property("text").toString(); }

} // namespace

class TestProjectsWindow : public QObject {
    Q_OBJECT

    QHash<QByteArray, QByteArray> m_saved;
    QSet<QByteArray> m_unset;
    QTemporaryDir m_dir;
    QList<LaunchSpec> m_launched;
    int m_closed = 0;

    QString path(const QString &name) const { return m_dir.filePath(name); }
    QString baseConfig() const { return path("base/config/fugue"); }
    QString baseData() const { return path("base/data/fugue"); }

    // The model with a fake launcher and window hooks: nothing is started or closed.
    std::unique_ptr<ProjectModel> makeModel(App &app) {
        auto model = std::make_unique<ProjectModel>([this](const LaunchSpec &spec) {
            m_launched << spec;
            return true;
        });
        model->setWindow([&app] { return app.workflows.unsaved(); }, [this] { ++m_closed; });
        model->registerCommands(app.commands);
        model->bindTitle(app.workflows);
        app.projects = model.get();
        return model;
    }

    void enterProject(const QString &name, const QStringList &inherits) {
        const Result<ProjectEntry> made = createProject(name, path("p/" + name.toLower()), inherits);
        QVERIFY2(made.ok(), "project not made");
        QVERIFY(setFugueHome(made->root, nullptr));
    }

    void writeDefaultSettings() {
        QDir().mkpath(baseConfig());
        QFile file(baseConfig() + "/settings.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(writeJson(QJsonObject{{"navigation", "standard"},
                                         {"corner_radius", 12},
                                         {"model_paths", QJsonArray{path("shared-models")}}}));
    }

    void shot(App &app, const QString &name) {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (!dir.isEmpty()) {
            QTest::qWait(300);
            test_support::grab(app.window).save(QString::fromUtf8(dir) + "/" + name + ".png");
        }
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void init() {
        m_launched.clear();
        m_closed = 0;
        for (const char *variable : kVariables) {
            if (qEnvironmentVariableIsSet(variable))
                m_saved.insert(variable, qgetenv(variable));
            else
                m_unset.insert(variable);
            qunsetenv(variable);
        }
        qputenv("XDG_CONFIG_HOME", path("base/config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", path("base/data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", path("base/state").toLocal8Bit());
        qputenv("XDG_CACHE_HOME", path("base/cache").toLocal8Bit());
        qputenv("XDG_RUNTIME_DIR", path("base/run").toLocal8Bit());
        QDir().mkpath(path("base/run"));
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
        QDir(path("shared-models")).removeRecursively();
    }

    // The title and the chip

    void theDefaultHasAnUnnamedTitleAndNoChipUntilThereIsAProject() {
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        QCOMPARE(app.window->title(), QString("Untitled — Fugue"));
        QVERIFY(!find(app.window, "projectChip"));
        QVERIFY(model->create("Acme", QString(), {}).value("error").toString().isEmpty());
        QQuickItem *chip = NEED("projectChip");
        QCOMPARE(textOf(chip), QString("Default project"));
        QCOMPARE(app.window->title(), QString("Untitled — Fugue"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aProjectNamesTheTitleAndTheChipAndTheChipOpensTheDialog() {
        enterProject("Acme", {"models"});
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        QCOMPARE(app.window->title(), QString("Untitled — Acme — Fugue"));
        QCOMPARE(textOf(NEED("projectChip")), QString("Acme"));
        click(app, NEED("projectChip"));
        QTRY_VERIFY(open(app.window, "projectsDialog"));
        QVERIFY(find(app.window, "projectCurrent-1")); // the Default is row 0, Acme row 1, marked current
        QVERIFY(!find(app.window, "projectCurrent-0"));
        QVERIFY(!find(app.window, "projectSwitch-1")); // no switch to where you are
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void renamingTheProjectChangesTheTitle() {
        enterProject("Acme", {});
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        QVERIFY(model->setCurrent("Acme Corp", {}).isEmpty());
        QCOMPARE(app.window->title(), QString("Untitled — Acme Corp — Fugue"));
        QTRY_COMPARE(textOf(NEED("projectChip")), QString("Acme Corp"));
    }

    // The dialog

    void newMakesAProjectListsItAndSwitchStartsANewProcessThenClosesTheWindow() {
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("file.projects"));
        QTRY_VERIFY(open(app.window, "projectsDialog"));
        QVERIFY(find(app.window, "projectCurrent-0")); // the Default, current
        click(app, NEED("projectNew"));
        type(app, NEED("newProjectName"), "acme");
        click(app, NEED("newProjectCreate"));
        QTRY_VERIFY(find(app.window, "project-1"));
        QVERIFY(!find(app.window, "projectsError"));
        QVERIFY(QFileInfo::exists(baseData() + "/projects/acme/project.json"));
        click(app, NEED("projectSwitch-1"));
        QTRY_COMPARE(m_closed, 1);
        QCOMPARE(m_launched.size(), 1);
        QCOMPARE(m_launched.first().arguments, (QStringList{"--project", baseData() + "/projects/acme"}));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aRefusedNewShowsWhyAndMakesNothing() {
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        app.commands.trigger("file.projects");
        QTRY_VERIFY(open(app.window, "projectsDialog"));
        click(app, NEED("projectNew"));
        type(app, NEED("newProjectName"), "a.b");
        click(app, NEED("newProjectCreate"));
        QVERIFY(textOf(NEED("projectsError")).contains("not a name"));
        QCOMPARE(model->projects().size(), 1);
    }

    void removeFromListAsksSaysTheFilesStayAndOnlyUnregisters() {
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        const QString root = model->create("Acme", QString(), {}).value("root").toString();
        QVERIFY(QFile(root + "/note.txt").open(QIODevice::WriteOnly));
        QVERIFY(app.open());
        app.commands.trigger("file.projects");
        QTRY_VERIFY(open(app.window, "projectsDialog"));
        QVERIFY(!find(app.window, "projectRemove-0")); // nothing to remove on the Default
        click(app, NEED("projectRemove-1"));
        QVERIFY(textOf(NEED("projectRemoveQuestion-1")).contains("folder and files are kept"));
        click(app, NEED("projectRemoveNo-1")); // Keep
        QCOMPARE(model->projects().size(), 2);
        click(app, NEED("projectRemove-1"));
        click(app, NEED("projectRemoveYes-1"));
        QTRY_COMPARE(model->projects().size(), 1);
        QVERIFY(QFileInfo::exists(root + "/project.json"));
        QVERIFY(QFileInfo::exists(root + "/note.txt"));
    }

    void projectSettingsOfTheDefaultOffersNothingToChange() {
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        app.commands.trigger("file.projects");
        QTRY_VERIFY(open(app.window, "projectsDialog"));
        click(app, NEED("projectSettings"));
        QVERIFY(textOf(NEED("defaultProjectNote")).contains("Default project"));
        QVERIFY(!find(app.window, "projectName"));
        QVERIFY(!find(app.window, "projectSettingsSave"));
        QVERIFY(!find(app.window, "projectInherits-models"));
        click(app, NEED("projectSettingsBack"));
        QVERIFY(find(app.window, "projectNew"));
    }

    void projectSettingsRenameAndTickWhatIsInherited() {
        enterProject("Acme", {"models"});
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        app.commands.trigger("file.projects");
        QTRY_VERIFY(open(app.window, "projectsDialog"));
        click(app, NEED("projectSettings"));
        type(app, NEED("projectName"), "client");
        click(app, NEED("projectInherits-packs"));
        click(app, NEED("projectInherits-models")); // off
        click(app, NEED("projectSettingsSave"));
        QTRY_COMPARE(model->currentName(), QString("client"));
        QCOMPARE(model->currentInherits(), (QStringList{"packs"}));
        QVERIFY(!find(app.window, "projectsError"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void unsavedWorkKeepsTheWindowAndOffersANewWindow() {
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        const QString root = model->create("Acme", QString(), {}).value("root").toString();
        QVERIFY(app.open());
        app.model.addNode("fugue/text", 100, 100);
        QVERIFY(app.workflows.unsaved());
        app.commands.trigger("file.projects");
        QTRY_VERIFY(open(app.window, "projectsDialog"));
        click(app, NEED("projectSwitch-1"));
        QVERIFY(textOf(NEED("projectBlockedText")).contains("unsaved"));
        QVERIFY(m_launched.isEmpty());
        QCOMPARE(m_closed, 0);
        click(app, NEED("projectBlockedNewWindow"));
        QTRY_COMPARE(m_launched.size(), 1);
        QCOMPARE(m_launched.first().arguments, (QStringList{"--project", root}));
        QCOMPARE(m_closed, 0); // this window is still open
        QVERIFY(open(app.window, "projectsDialog"));
    }

    // The tags

    void inheritedSettingsShowTheTagAndUseDefaultsValueGivesTheProjectsOwnUp() {
        writeDefaultSettings();
        enterProject("Acme", {"preferences", "models"});
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(open(app.window, "settingsPanel"));
        QQuickItem *tag = NEED("origin-navigation");
        QCOMPARE(textOf(tag), QString("Inherited from Default"));
        QVERIFY(!find(app.window, "useDefault-navigation"));
        QCOMPARE(app.settings.navigation(), QString("standard"));
        // A choice makes it the project's own: the tag goes, the way back appears.
        click(app, NEED("navigation-legacy"));
        QTRY_VERIFY(find(app.window, "useDefault-navigation"));
        QVERIFY(!find(app.window, "origin-navigation"));
        QCOMPARE(app.settings.navigation(), QString("legacy"));
        QCOMPARE(readJsonObject(fugue::settingsPath()).value().value("navigation").toString(), QString("legacy"));
        click(app, NEED("useDefault-navigation"));
        QTRY_VERIFY(find(app.window, "origin-navigation"));
        QCOMPARE(app.settings.navigation(), QString("standard"));
        QVERIFY(!QFile(baseConfig() + "/settings.json").readAll().contains("legacy")); // the Default is untouched
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aSettingOfAProjectThatInheritsNothingHasNoTag() {
        writeDefaultSettings();
        enterProject("Closed", {});
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(open(app.window, "settingsPanel"));
        QTRY_VERIFY(find(app.window, "navigation-legacy"));
        QVERIFY(!find(app.window, "origin-navigation"));
        QCOMPARE(app.settings.navigation(), QString("legacy"));
    }

    void inheritedModelFolderRowsAreReadOnlyAndRemoveGivesTheRefusal() {
        writeDefaultSettings();
        enterProject("Acme", {"models"});
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(open(app.window, "settingsPanel"));
        click(app, NEED("settingsPage-1"));
        // The Default's own models folder first, then its model_paths.
        QVERIFY(NEED("model_paths-inherited-0"));
        QVERIFY(NEED("model_paths-inherited-1"));
        QCOMPARE(textOf(NEED("model_paths-inherited-tag-1")), QString("Inherited from Default"));
        click(app, NEED("model_paths-inherited-remove-1"));
        QVERIFY(textOf(NEED("settingsError")).contains("inherits from the Default"));
        QCOMPARE(app.settings.inheritedModelPaths().size(), 2); // still there
        QVERIFY(!find(app.window, "model_paths-remove-0"));       // no own folder to remove
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void inheritedPacksShowTheTagAndTheWarningAndRemoveGivesTheRefusal() {
        QVERIFY(pack_fixture::writeSayPack(baseData() + "/packs", "default"));
        enterProject("Acme", {"packs"});
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(open(app.window, "settingsPanel"));
        click(app, NEED("settingsPage-6"));
        QCOMPARE(textOf(NEED("packInherited-io.example.tools")), QString("Inherited from Default"));
        QVERIFY(textOf(NEED("packWarning-io.example.tools")).contains("Default"));
        click(app, NEED("packRemove-io.example.tools"));
        QTRY_VERIFY(find(app.window, "packsFailure"));
        QVERIFY(textOf(NEED("packsFailure")).contains("inherits from the Default"));
        QVERIFY(QFileInfo::exists(baseData() + "/packs/io.example.tools/pack.json")); // the Default's pack is untouched
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_projects_window savesThePicturesOfTheWindow

    void savesThePicturesOfTheWindow() {
        if (qgetenv("FUGUE_SHOT_DIR").isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the projects pictures");
        writeDefaultSettings();
        QVERIFY(pack_fixture::writeSayPack(baseData() + "/packs", "default"));
        (void)createProject("Gone", QString(), {});
        (void)createProject("Beta", QString(), {"models"});
        QVERIFY(QFile::remove(baseData() + "/projects/gone/project.json"));
        enterProject("Acme", {"preferences", "models", "packs"});
        std::unique_ptr<ProjectModel> model; // made before the window, so it outlives its QML
        App app(load(emptyDoc));
        model = makeModel(app);
        QVERIFY(app.open());
        app.window->resize(960, 708);
        app.commands.trigger("file.projects");
        QTRY_VERIFY(open(app.window, "projectsDialog"));
        shot(app, "projects-list");
        click(app, NEED("projectNew"));
        type(app, NEED("newProjectName"), "client");
        shot(app, "projects-new");
        click(app, NEED("newProjectBack"));
        click(app, NEED("projectSettings"));
        shot(app, "projects-settings");
        QMetaObject::invokeMethod(app.window->findChild<QObject *>("projectsDialog"), "close");
        QTRY_VERIFY(!open(app.window, "projectsDialog"));
        app.commands.trigger("app.settings");
        QTRY_VERIFY(open(app.window, "settingsPanel"));
        shot(app, "projects-settings-general");
        click(app, NEED("settingsPage-1"));
        shot(app, "projects-settings-folders");
        click(app, NEED("settingsPage-6"));
        shot(app, "projects-settings-packs");
    }
};

QTEST_MAIN(TestProjectsWindow)
#include "test_projects_window.moc"
