// The first-run model folder sheet in the real window, by keys alone (#619, the owner's keyboard rule): it opens with the focus on
// Skip, nothing is searched until Search, the focus follows to the primary when looking ends, Space flips a switch, Up and Down move
// between switches, Enter applies (a switch takes no Enter), Escape skips. Fake installs in a scratch home; the real finder.
//
// FUGUE_SHOT_DIR=<folder> also takes the pictures of the documentation: the consent screen, three installs found, nothing found and
// a slow drive, at 680 and 480 px, in the window's own (dark) theme.

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_app.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/settings.h"
#include "../window_grab.h"

using namespace canvas_test;

namespace {

const QByteArray emptyDoc = R"({"fugue": 1, "graph": {"nodes": {}}})";

void touchFile(const QString &path, qint64 size = 4) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("abc");
    if (size > 3)
        file.resize(size); // sparse: the size shows, nothing is stored
}

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

constexpr qint64 GB = 1000LL * 1000 * 1000;

} // namespace

class TestModelSetupSheet : public QObject {
    Q_OBJECT

    QTemporaryDir m_tree;
    QByteArray m_home;
    QString m_mnt;
    fugue::FinderEnv env() {
        fugue::FinderEnv e;
        e.home = QDir::homePath();
        e.driveParents = {m_mnt};
        e.ownDataDir = e.home + "/.local/share/fugue";
        return e;
    }
    void comfy(const QString &root) {
        QDir().mkpath(root + "/custom_nodes");
        touchFile(root + "/models/checkpoints/flux1-dev-fp8.safetensors", 17 * GB);
        touchFile(root + "/models/checkpoints/sdxl-base-1.0.safetensors", 7 * GB);
        for (int i = 0; i < 24; ++i)
            touchFile(root + QString("/models/loras/style-%1.safetensors").arg(i, 2, 10, QChar('0')), 150 * 1000 * 1000);
        touchFile(root + "/models/vae/ae.safetensors", 335 * 1000 * 1000);
        touchFile(root + "/models/text_encoders/t5xxl_fp8.safetensors", 5 * GB);
    }
    void a1111(const QString &root) {
        touchFile(root + "/webui.py");
        touchFile(root + "/models/Stable-diffusion/dreamshaper-8.safetensors", 2 * GB);
        for (int i = 0; i < 6; ++i)
            touchFile(root + QString("/models/Lora/detail-%1.safetensors").arg(i), 140 * 1000 * 1000);
        touchFile(root + "/models/VAE/sdxl-vae.safetensors", 335 * 1000 * 1000);
    }
    void invoke(const QString &root) {
        touchFile(root + "/invokeai.yaml");
        for (int i = 0; i < 9; ++i)
            touchFile(root + QString("/models/sdxl/main/model-%1.safetensors").arg(i), 6 * GB);
    }

    // The sheet, found once the window has made it.
    QObject *sheet(App &app) {
        QObject *found = nullptr;
        [&] { QTRY_VERIFY2((found = app.window->findChild<QObject *>("modelSetupSheet")), "no sheet"); }();
        return found;
    }
    QQuickItem *control(App &app, const QString &name) {
        QQuickItem *item = nullptr;
        [&] { QTRY_VERIFY2((item = find(app.window, name)), qPrintable(name)); }();
        return item;
    }
    QString shotDir() const { return qEnvironmentVariable("FUGUE_SHOT_DIR"); }
    void shot(App &app, QObject *sheetObject, const QString &name, int windowWidth) {
        QTest::qWait(250); // the sheet's own animations
        QImage image = test_support::grab(app.window);
        QVERIFY(!image.isNull());
        const QQuickItem *content = qvariant_cast<QQuickItem *>(sheetObject->property("contentItem"));
        QVERIFY(content);
        const QPointF at = content->mapToScene(QPointF(0, 0));
        const QRect crop = QRect(int(at.x()) - 24, int(at.y()) - 24, int(content->width()) + 48, int(content->height()) + 48).intersected(image.rect());
        QVERIFY(image.copy(crop).save(shotDir() + QStringLiteral("/%1-%2.png").arg(name).arg(windowWidth > 600 ? 680 : 480)));
    }

private slots:
    void initTestCase() {
        m_home = qgetenv("HOME");
        QVERIFY(m_tree.isValid());
        // The scratch home: the finder and the settings both see this one, never the owner's.
        qputenv("HOME", m_tree.filePath("home").toLocal8Bit());
        m_mnt = m_tree.filePath("mnt");
        QDir().mkpath(QDir::homePath());
        QDir().mkpath(m_mnt);
    }
    void cleanupTestCase() { qputenv("HOME", m_home); }
    void init() {
        QDir(QDir::homePath()).removeRecursively();
        QDir(m_mnt).removeRecursively();
        QDir().mkpath(QDir::homePath());
        QDir().mkpath(m_mnt);
    }

    void keysAloneWalkTheSheet() {
        comfy(QDir::homePath() + "/ComfyUI");
        a1111(m_mnt + "/d/stable-diffusion-webui");
        App app(load(emptyDoc));
        app.settings.modelSetup()->setEnv(env());
        QVERIFY(app.open());
        fugue::ModelSetup &setup = *app.settings.modelSetup();
        QVERIFY(setup.maybeOffer());
        QObject *popup = sheet(app);
        QTRY_VERIFY(popup->property("visible").toBool());
        // Consent: the focus is on Skip, and nothing was searched yet.
        QQuickItem *skip = control(app, "modelSetupSkip");
        QTRY_VERIFY(skip->hasActiveFocus());
        QCOMPARE(setup.phase(), QStringLiteral("consent"));
        QVERIFY(find(app.window, "modelSetupPlace-home"));
        // Tab reaches Search; Enter runs it; the sheet looks, then shows the results with the focus on the primary.
        QTest::keyClick(app.window, Qt::Key_Tab);
        QQuickItem *primary = control(app, "modelSetupPrimary");
        QTRY_VERIFY(primary->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_COMPARE(setup.phase(), QStringLiteral("results"));
        QCOMPARE(setup.rows().size(), 2);
        QTRY_VERIFY(find(app.window, "modelSetupToggle-0"));
        QTRY_VERIFY(primary->hasActiveFocus());
        // A switch: Space flips it, Down moves to the next, Enter on a switch runs the primary.
        QQuickItem *first = control(app, "modelSetupToggle-0");
        first->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(first->hasActiveFocus());
        QVERIFY(setup.rows().first().toMap().value("on").toBool());
        QTest::keyClick(app.window, Qt::Key_Space);
        QVERIFY(!setup.rows().first().toMap().value("on").toBool());
        QTest::keyClick(app.window, Qt::Key_Space);
        QVERIFY(setup.rows().first().toMap().value("on").toBool());
        QTest::keyClick(app.window, Qt::Key_Down);
        QQuickItem *second = control(app, "modelSetupToggle-1");
        QTRY_VERIFY(second->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Up);
        QTRY_VERIFY(first->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!setup.isOpen());
        QCOMPARE(fugue::readSettings(app.settings.path()).settings.modelPaths.size(), 2);
        // Escape on a rerun skips and writes nothing more.
        const QByteArray before = [&] { QFile f(app.settings.path()); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        setup.show(false);
        QTRY_VERIFY(popup->property("visible").toBool());
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!setup.isOpen());
        QFile after(app.settings.path());
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), before);
    }

    // The focus scrolls the list: arrow and Tab moves past what the list shows bring the focused switch into view, in the consent
    // list and in a long list of results.
    void theFocusedSwitchScrollsIntoView() {
        // Every usual install folder exists: a long consent list.
        for (const QString &name : {"ComfyUI/models", "ComfyUI-Shared", "ComfyUI-Installs", "StabilityMatrix/Data/Models", "StabilityMatrix/Models",
                                    "stable-diffusion-webui/models", "stable-diffusion-webui-forge/models", "SwarmUI/Models", "invokeai/models"})
            QDir().mkpath(QDir::homePath() + "/" + name);
        App app(load(emptyDoc));
        fugue::ModelSetup &setup = *app.settings.modelSetup();
        setup.setEnv(env());
        QVERIFY(app.open());
        app.window->resize(900, 480);
        setup.setFinder([](const fugue::ModelSetup::FinderJob &, const std::function<void(const fugue::FoundFolder &)> &found) {
            for (int i = 0; i < 20; ++i) {
                fugue::FoundFolder folder;
                folder.tool = "ComfyUI";
                folder.path = QString("/models-%1").arg(i, 2, 10, QChar('0'));
                folder.models = 100 - i;
                folder.size = 1000000 * (100 - i);
                found(folder);
            }
            return fugue::FindResult();
        }, true);
        setup.show(true);
        QObject *popup = sheet(app);
        QTRY_VERIFY(popup->property("visible").toBool());
        QQuickItem *list = control(app, "modelSetupList");
        const auto visibleIn = [&](QQuickItem *item) {
            const qreal top = item->mapToItem(list, QPointF(0, 0)).y();
            return top >= 0 && top + item->height() <= list->height() + 1;
        };
        // The consent list: the last place's switch, focused, is in view.
        QQuickItem *last = control(app, "modelSetupPlaceToggle-drive:" + m_mnt);
        QVERIFY2(!visibleIn(last) || list->property("contentY").toReal() >= 0, "the list is long enough to need a scroll");
        last->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(visibleIn(last));
        QVERIFY(list->property("contentY").toReal() > 0);
        // The results: Down along twenty rows keeps the focused switch in view.
        setup.search();
        QTRY_COMPARE(setup.phase(), QStringLiteral("results"));
        QVERIFY(popup->setProperty("showAll", true));
        QCOMPARE(popup->property("showAll").toBool(), true);
        QCOMPARE(setup.rows().size(), 20);
        QQuickItem *first = control(app, "modelSetupToggle-0");
        first->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(first->hasActiveFocus());
        for (int i = 1; i < 20; ++i) {
            QTest::keyClick(app.window, Qt::Key_Down);
            QQuickItem *row = control(app, QString("modelSetupToggle-%1").arg(i));
            QTRY_VERIFY(row->hasActiveFocus());
            QTRY_VERIFY2(visibleIn(row), qPrintable(QString("row %1").arg(i)));
        }
        QTest::keyClick(app.window, Qt::Key_Up);
        QTRY_VERIFY(visibleIn(control(app, "modelSetupToggle-18")));
    }

    // The pictures of the documentation.
    void pictures() {
        if (shotDir().isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to take the pictures");
        QDir().mkpath(shotDir());
        for (const int width : {1000, 528}) {
            for (const QString &which : {"consent", "found", "nothing", "slow"}) {
                init();
                if (which != "nothing") {
                    comfy(QDir::homePath() + "/ComfyUI");
                    if (which == "found") {
                        a1111(QDir::homePath() + "/stable-diffusion-webui");
                        invoke(QDir::homePath() + "/invokeai");
                    }
                    if (which == "slow") {
                        // A big collection on a slow drive, as the entry cap sees it.
                        for (int i = 0; i < 90; ++i)
                            touchFile(QDir::homePath() + "/ComfyUI/models/loras" + QString("/more-%1.safetensors").arg(i, 2, 10, QChar('0')), 120 * 1000 * 1000);
                    }
                }
                if (which == "consent") {
                    a1111(QDir::homePath() + "/stable-diffusion-webui");
                    QDir().mkpath(m_mnt + "/d");
                }
                App app(load(emptyDoc));
                fugue::FinderEnv e = env();
                if (which == "consent" && QFileInfo::exists("/mnt"))
                    e.driveParents = {QStringLiteral("/mnt")}; // only stat'd to be listed: the consent screen searches nothing
                if (which == "slow")
                    e.countCapEntries = 60;
                app.settings.modelSetup()->setEnv(e);
                QVERIFY(app.open());
                app.window->resize(width, 900);
                fugue::ModelSetup &setup = *app.settings.modelSetup();
                setup.setFreeSpace([](const QString &) { return qint64(1979) * GB; });
                setup.show(true);
                QObject *popup = sheet(app);
                QTRY_VERIFY(popup->property("visible").toBool());
                if (which != "consent") {
                    setup.search();
                    QTRY_VERIFY(!setup.looking());
                    QTest::qWait(100);
                }
                shot(app, popup, which, width);
                setup.skip();
            }
        }
    }
};

QTEST_MAIN(TestModelSetupSheet)
#include "test_model_setup_sheet.moc"
