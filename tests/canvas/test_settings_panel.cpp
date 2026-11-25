// The settings panel in the real window (#51): Ctrl+, opens it, a change made with the mouse
// and keys applies live and is in the file, so a model built from that file has it. The
// files live in a temporary XDG_CONFIG_HOME, never the owner's.

#include <QFile>
#include <QQmlProperty>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_app.h"
#include "../scroll_checks.h"
#include "../window_grab.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/providers.h"

#include <QJsonDocument>
#include "../test_shard.h"

using namespace canvas_test;

namespace {

const QByteArray emptyDoc = R"({"fugue": 1, "graph": {"nodes": {}}})";

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

bool panelOpen(QQuickWindow *window) {
    QObject *panel = window->findChild<QObject *>("settingsPanel");
    return panel && panel->property("opened").toBool();
}

#define NEED(name)                                                                                                     \
    [&] {                                                                                                              \
        QQuickItem *found = nullptr;                                                                                   \
        [&] { QTRY_VERIFY2((found = find(app.window, name)), qPrintable(QStringLiteral("no %1").arg(name))); }();      \
        return found;                                                                                                  \
    }()

// Where `item` is once the layouts have placed it: a list that just changed moves its rows
// when the next frame is polished, so the item must stay put for a few frames first.
QPoint settled(QQuickItem *item) {
    QPoint at = centerOf(item);
    for (int still = 0, i = 0; still < 3 && i < 50; ++i) {
        QTest::qWait(20);
        const QPoint now = centerOf(item);
        still = now == at ? still + 1 : 0;
        at = now;
    }
    return at;
}

// Scrolls the page that holds `item` until all of it shows, as a person scrolls to a control
// below the fold. Where the fold falls depends on the fonts: CI's Noto Sans makes the pages
// taller than the sans of a desktop, so a row that shows here can be under the fold there.
// Says whether it scrolled.
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
    const qreal most = qMax(0.0, view->property("contentHeight").toReal() - view->height());
    view->setProperty("contentY", qBound(0.0, y, most));
    return true;
}

// Clicks an item where it settles, scrolled into view first.
void click(App &app, QQuickItem *item) {
    QPoint at = settled(item);
    if (scrollIntoView(item))
        at = settled(item);
    QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
}

// Types `text` into a field of the panel and presses Enter.
void type(App &app, QQuickItem *field, const QString &text) {
    click(app, field);
    QTest::keySequence(app.window, QKeySequence::SelectAll);
    QTest::keyClick(app.window, Qt::Key_Backspace);
    for (const QChar c : text)
        QTest::keyClick(app.window, c.toLatin1());
    QTest::keyClick(app.window, Qt::Key_Return);
}

} // namespace

class TestSettingsPanel : public QObject {
    Q_OBJECT

    QTemporaryDir m_config;

private slots:
    void initTestCase() {
        qputenv("XDG_CONFIG_HOME", m_config.filePath("config").toUtf8());
    }

    void ctrlCommaOpensItAndEscClosesIt() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QTest::keyClick(app.window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(panelOpen(app.window));
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!panelOpen(app.window));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The window by keys alone (#464, the owner's keyboard rule): opening puts the focus on the current page, Tab walks the page list and
    // then the page's controls in reading order, Space flips a switch, Escape closes and the focus goes back to what had it.
    void theWindowWorksByKeysAlone() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.canvas->forceActiveFocus();
        QTRY_VERIFY(app.canvas->hasActiveFocus());
        QQuickItem *before = app.window->activeFocusItem();
        QVERIFY(!app.canvas->property("hoverMagnifier").toBool());
        QTest::keyClick(app.window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(panelOpen(app.window));
        QTRY_VERIFY(NEED("settingsPage-general")->hasActiveFocus());
        // Down moves in the page list and opens that page; Up comes back.
        QTest::keyClick(app.window, Qt::Key_Down);
        QTRY_COMPARE(app.window->findChild<QObject *>("settingsPanel")->property("page").toString(), QStringLiteral("keymap"));
        NEED("settingsPage-keymap")->forceActiveFocus();
        QTest::keyClick(app.window, Qt::Key_Up);
        QTRY_COMPARE(app.window->findChild<QObject *>("settingsPanel")->property("page").toString(), QStringLiteral("general"));
        // Tab: out of the list into the page, the kit's ring on each, until the magnify switch; Space flips it.
        QStringList order;
        for (int i = 0; i < 60; ++i) {
            QTest::keyClick(app.window, Qt::Key_Tab);
            QQuickItem *focused = app.window->activeFocusItem();
            QVERIFY(focused);
            QQuickItem *named = focused;
            while (named && named->objectName().isEmpty())
                named = named->parentItem();
            order << (named ? named->objectName() : QString());
            if (named && named->objectName() == "hoverMagnifier")
                break;
        }
        QVERIFY2(order.last() == "hoverMagnifier", qPrintable(order.join(',')));
        QVERIFY2(order.indexOf("navigation-legacy") < 0 || order.indexOf("navigation-legacy") < order.size() - 1, qPrintable(order.join(',')));
        QTest::keyClick(app.window, Qt::Key_Space);
        QTRY_VERIFY(app.canvas->property("hoverMagnifier").toBool());
        QVERIFY(fugue::SettingsModel(app.settings.path()).hoverMagnifier());
        // Escape closes it and the focus is where it was.
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!panelOpen(app.window));
        QTRY_COMPARE(app.window->activeFocusItem(), before);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Settings > Cache (#141): the command "Clear image preview cache" opens the page with the
    // question for the previews; nothing goes before the yes, and a no keeps everything.
    void clearingTheImagePreviewsAsksFirst() {
        const QString file = fugue::thumbnailsDir() + "/probe-512.png";
        QDir().mkpath(fugue::thumbnailsDir());
        QFile probe(file);
        QVERIFY(probe.open(QIODevice::WriteOnly));
        probe.write(QByteArray(1000, 'x'));
        probe.close();
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("cache.clear-previews");
        QTRY_VERIFY(panelOpen(app.window));
        NEED("cacheQuestion");
        QTRY_VERIFY(NEED("cacheQuestionDetail")->property("text").toString().contains("Nothing is lost for good"));
        // The safe answer is the highlighted one and comes first; Clear is the danger tint, last (the cohesion audit's D8).
        QVERIFY(NEED("cacheNo")->property("highlighted").toBool());
        QVERIFY(!NEED("cacheYes")->property("highlighted").toBool());
        QVERIFY(NEED("cacheYes")->property("danger").toBool());
        QVERIFY(NEED("cacheNo")->mapToScene({0, 0}).x() < NEED("cacheYes")->mapToScene({0, 0}).x());
        click(app, NEED("cacheNo"));
        QTRY_VERIFY(!find(app.window, "cacheQuestion"));
        QVERIFY(QFile::exists(file));
        // Asked again, the yes clears it, and the page says how much.
        app.commands.trigger("cache.clear-previews");
        click(app, NEED("cacheYes"));
        QTRY_VERIFY(!QFile::exists(file));
        QTRY_VERIFY(NEED("cacheMessage")->property("text").toString().contains("Cleared"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Clear all (spec 10.6): one button beside the total, off while there is nothing to clear;
    // it asks first, with the total and what is lost, and the yes clears every kind.
    void clearAllAsksFirstAndClearsEveryKind() {
        QDir(fugue::thumbnailsDir()).removeRecursively();
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("cache.clear-previews");
        QTRY_VERIFY(panelOpen(app.window));
        NEED("cacheQuestion"); // the previews' question, shown by the command
        click(app, NEED("cacheNo"));
        QQuickItem *clearAll = NEED("cacheClearAll");
        QTRY_VERIFY(!clearAll->property("enabled").toBool()); // nothing there yet (and not while measuring)
        QVERIFY(QFile::exists(fugue::thumbnailsDir()) || QDir().mkpath(fugue::thumbnailsDir()));
        const QString file = fugue::thumbnailsDir() + "/probe-all-512.png";
        QFile probe(file);
        QVERIFY(probe.open(QIODevice::WriteOnly));
        probe.write(QByteArray(1000, 'x'));
        probe.close();
        // Something to clear: measured again when the page is made again.
        app.commands.trigger("cache.clear-previews");
        click(app, NEED("cacheNo"));
        QQuickItem *button = NEED("cacheClearAll");
        QTRY_VERIFY(button->property("enabled").toBool());
        QVERIFY(button->height() >= 28);
        click(app, button);
        NEED("cacheQuestion");
        const QString detail = NEED("cacheQuestionDetail")->property("text").toString();
        QVERIFY(detail.contains("run again"));
        QVERIFY(detail.contains("history of past runs"));
        QVERIFY(detail.contains("Pictures on the canvas"));
        // A no keeps everything.
        click(app, NEED("cacheNo"));
        QTRY_VERIFY(!find(app.window, "cacheQuestion"));
        QVERIFY(QFile::exists(file));
        // The yes clears it, and the page says how much.
        click(app, NEED("cacheClearAll"));
        click(app, NEED("cacheYes"));
        QTRY_VERIFY(!QFile::exists(file));
        QTRY_VERIFY(NEED("cacheMessage")->property("text").toString().contains("everything cached"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theCacheLimitsAreChoicesThatPersist() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("cache.clear-previews");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("cacheResults-5"));
        QTRY_COMPARE(fugue::SettingsModel(app.settings.path()).cacheResultsGb(), 5.0);
        click(app, NEED("cacheThumbnails-0"));
        QTRY_COMPARE(fugue::SettingsModel(app.settings.path()).cacheThumbnailsGb(), 0.0);
        click(app, NEED("runsKept-50")); // below the fold of the page: click() scrolls to it
        QTRY_COMPARE(fugue::SettingsModel(app.settings.path()).runsKept(), 50);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void navigationChangesLiveAndPersists() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QCOMPARE(app.canvas->property("navigation").toString(), QStringLiteral("legacy"));
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("navigation-standard"));
        QTRY_COMPARE(app.canvas->property("navigation").toString(), QStringLiteral("standard"));
        // A model made from the file, as the next start makes it, has it.
        fugue::SettingsModel again(app.settings.path());
        QCOMPARE(again.navigation(), QStringLiteral("standard"));
        click(app, NEED("navigation-legacy"));
        QTRY_COMPARE(app.canvas->property("navigation").toString(), QStringLiteral("legacy"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // General > Magnify small nodes on hover (#123): off by default, the switch turns it on and
    // off live and the file keeps it.
    void theHoverMagnifierSwitchChangesLiveAndPersists() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QVERIFY(!app.canvas->property("hoverMagnifier").toBool());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        QQuickItem *toggle = NEED("hoverMagnifier");
        QVERIFY(!toggle->property("on").toBool());
        click(app, toggle);
        QTRY_VERIFY(app.canvas->property("hoverMagnifier").toBool());
        QVERIFY(fugue::SettingsModel(app.settings.path()).hoverMagnifier());
        click(app, toggle);
        QTRY_VERIFY(!app.canvas->property("hoverMagnifier").toBool());
        QVERIFY(!fugue::SettingsModel(app.settings.path()).hoverMagnifier());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // General > Reopen tabs at start (#212): on by default; the switch turns it off and on and the file keeps it.
    void theRestoreTabsSwitchPersists() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        QQuickItem *toggle = NEED("restoreTabs");
        QVERIFY(toggle->property("on").toBool());
        click(app, toggle);
        QTRY_VERIFY(!fugue::SettingsModel(app.settings.path()).restoreTabs());
        click(app, toggle);
        QTRY_VERIFY(fugue::SettingsModel(app.settings.path()).restoreTabs());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Theme > Look > Follow Omarchy's rounding (#467, replacing General > Corner radius, #63): the Corners slider starts from Omarchy's
    // rounding (none here: square) instead of 8 px; the canvas redraws at once and the file keeps it. The old General row is gone.
    void theCornersFollowOmarchyOptionChangesLiveAndPersists() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        const auto corner = [&] { return app.theme.look()->property("corner").toInt(); };
        QCOMPARE(corner(), 8);
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        QVERIFY(!find(app.window, "cornerValue")); // the old General row
        click(app, NEED("settingsPage-theme"));
        QQuickItem *toggle = NEED("lookCornersOmarchy");
        QVERIFY(!toggle->property("checked").toBool());
        click(app, toggle);
        QTRY_COMPARE(corner(), 0);
        QTRY_VERIFY(fugue::SettingsModel(app.settings.path()).lookCornersOmarchy());
        click(app, toggle);
        QTRY_COMPARE(corner(), 8);
        QTRY_VERIFY(!fugue::SettingsModel(app.settings.path()).lookCornersOmarchy());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The performance budget (docs/canvas-look.md 20.17, #467): Settings, settled on its Theme page (sliders, switches, choosers), swaps
    // no frame, in the Calm and the Flat look.
    void anIdleSettingsWindowDoesNotRedraw_data() {
        QTest::addColumn<QString>("preset");
        QTest::newRow("calm") << "calm";
        QTest::newRow("flat") << "flat";
    }
    void anIdleSettingsWindowDoesNotRedraw() {
        QFETCH(QString, preset);
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QCOMPARE(app.settings.applyLookPreset(preset), QString());
        QElapsedTimer opened;
        opened.start();
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-theme"));
        qInfo() << "Settings' first frame after open," << preset << ":" << opened.elapsed() << "ms";
        app.window->contentItem()->forceActiveFocus();
        QTest::mouseMove(app.window, QPoint(5, 5));
        int frames = 0;
        const auto connection = connect(app.window, &QQuickWindow::frameSwapped, this, [&frames] { ++frames; });
        bool quiet = false;
        for (int i = 0; i < 20 && !quiet; ++i) {
            frames = 0;
            QTest::qWait(500);
            quiet = frames == 0;
        }
        QVERIFY2(quiet, "Settings never stopped drawing");
        QTest::qWait(1000);
        disconnect(connection);
        QCOMPARE(frames, 0);
    }

    // The page list is alphabetical by title, opens on General, and Up and Down walk it in that order.
    void thePagesAreAlphabeticalAndOpenOnGeneral() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        QObject *panel = app.window->findChild<QObject *>("settingsPanel");
        QCOMPARE(panel->property("page").toString(), QStringLiteral("general"));
        QStringList ids;
        for (const QVariant &page : panel->property("pages").toList())
            ids << page.toMap().value("id").toString();
        QCOMPARE(ids, (QStringList{"cache", "folders", "general", "keymap", "packs", "providers", "run", "theme"}));
        QQuickItem *cache = NEED("settingsPage-cache");
        QVERIFY(cache->mapToScene(QPointF(0, 0)).y() < NEED("settingsPage-folders")->mapToScene(QPointF(0, 0)).y());
        click(app, NEED("settingsPage-general"));
        NEED("settingsPage-general")->forceActiveFocus();
        QTest::keyClick(app.window, Qt::Key_Down);
        QTRY_COMPARE(panel->property("page").toString(), QStringLiteral("keymap"));
        NEED("settingsPage-keymap")->forceActiveFocus();
        QTest::keyClick(app.window, Qt::Key_Up);
        QTRY_COMPARE(panel->property("page").toString(), QStringLiteral("general"));
    }

    void aTypedFolderIsCheckedAndPersists() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-folders"));
        QQuickItem *output = NEED("output_folder");
        type(app, output, "not/absolute");
        QTRY_VERIFY(find(app.window, "settingsError"));
        QVERIFY(fugue::SettingsModel(app.settings.path()).outputFolderIsDefault());
        type(app, output, "~/Art/Fugue");
        QTRY_COMPARE(app.settings.outputFolder(), QStringLiteral("~/Art/Fugue"));
        QCOMPARE(fugue::SettingsModel(app.settings.path()).outputFolder(), QStringLiteral("~/Art/Fugue"));
        type(app, NEED("model_paths-add"), "/srv/models");
        QTRY_COMPARE(fugue::SettingsModel(app.settings.path()).modelPaths(), QStringList{"/srv/models"});
        click(app, NEED("model_paths-remove-0"));
        QTRY_VERIFY(fugue::SettingsModel(app.settings.path()).modelPaths().isEmpty());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Choose… opens the folder dialog. It once called the callback instead of the dialog (a line starting
    // with "(" continued the one above), which warned "Property 'open' of object false is not a function".
    void chooseOpensTheFolderDialog() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-folders"));
        click(app, NEED("output_folder-choose"));
        QTRY_VERIFY(app.window->findChild<QObject *>("settingsFolderDialog"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #86: an extra model folder says what it holds: what its files say, or a kind set here.
    void aModelFolderShowsWhatItHoldsAndTakesAKind() {
        QTemporaryDir loras;
        const QByteArray json = "{\"x.lora_A.weight\": {\"dtype\": \"F16\", \"shape\": [4, 4], \"data_offsets\": [0, 0]}}";
        QByteArray header;
        for (int i = 0; i < 8; ++i)
            header.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
        for (const char *name : {"a.safetensors", "b.safetensors"}) {
            QFile file(loras.filePath(QString::fromLatin1(name)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(header + json);
        }
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-folders"));
        type(app, NEED("model_paths-add"), loras.path());
        QTRY_COMPARE(fugue::SettingsModel(app.settings.path()).modelPaths(), QStringList{loras.path()});
        QTRY_COMPARE(NEED("model_paths-found-0")->property("text").toString(), QStringLiteral("Found: LoRAs"));
        click(app, NEED("model_paths-holds-0-fugue/vae"));
        QTRY_COMPARE(fugue::SettingsModel(app.settings.path()).modelFolderKind(loras.path()), QStringLiteral("fugue/vae"));
        QTRY_VERIFY(!find(app.window, "model_paths-found-0")); // a set kind needs no finding: hidden
        click(app, NEED("model_paths-holds-0-"));
        QTRY_VERIFY(fugue::SettingsModel(app.settings.path()).modelFolderKind(loras.path()).isEmpty());
        click(app, NEED("model_paths-holds-0-layout"));
        QTRY_COMPARE(fugue::SettingsModel(app.settings.path()).modelFolderKind(loras.path()), QStringLiteral("layout"));
        click(app, NEED("model_paths-remove-0")); // its kind goes with it
        QTRY_VERIFY(fugue::SettingsModel(app.settings.path()).modelPaths().isEmpty());
        QTRY_VERIFY(fugue::SettingsModel(app.settings.path()).modelFolderKind(loras.path()).isEmpty());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void runOptionsReachNewNodes() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-run"));
        click(app, NEED("newSeed-fixed"));
        click(app, NEED("embed-none"));
        QTRY_COMPARE(fugue::SettingsModel(app.settings.path()).embedWorkflow(), QStringLiteral("none"));
        QCOMPARE(fugue::SettingsModel(app.settings.path()).newSeed(), QStringLiteral("fixed"));
        const QString seeded = app.model.addNode("fugue/generate-image", 0, 0);
        QCOMPARE(app.model.document().graph.nodes.value(seeded).inputs.value("seed").kind(),
                 fugue::InputValue::Kind::Literal);
        const QString saver = app.model.addNode("fugue/save-image", 0, 0);
        QCOMPARE(app.model.document().graph.nodes.value(saver).inputs.value("embed").literal(), QJsonValue("none"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void providersPageShowsDefaultsAndKeepsKeysHidden() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        const QString providers = app.settings.providersPath();
        QVERIFY(providers.startsWith(app.dir.path()));
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-providers"));
        NEED("providerRow-0");
        QVERIFY(!find(app.window, "providerKey-0")); // the rows are shut: one line each
        click(app, NEED("providerRow-0"));
        NEED("providerDefault-0");
        QVERIFY(!QFile::exists(providers));
        type(app, NEED("providerKey-0"), "sk-hidden-123");
        QTRY_VERIFY(QFile::exists(providers));
        QCOMPARE(fugue::loadProviderInstances(providers).value().first().key, QStringLiteral("sk-hidden-123"));
        QTRY_VERIFY(!find(app.window, "providerDefault-0")); // saved now
        NEED("providerKeyClear-0");
        // The field never shows the key, and nothing in the panel's model holds it. (The
        // rows are made again from the new list, so the field is found again.)
        const QList<QObject *> inputs = NEED("providerKey-0")->findChildren<QObject *>();
        for (QObject *child : inputs)
            QVERIFY(!child->property("text").toString().contains("sk-hidden"));
        QVERIFY(!QJsonDocument::fromVariant(app.settings.providers()).toJson().contains("sk-hidden"));
        type(app, NEED("providerAddress-0"), "nonsense address");
        QTRY_VERIFY(find(app.window, "settingsError"));
        QCOMPARE(fugue::loadProviderInstances(providers).value().first().address, QString());
        click(app, NEED("providerEnabled-0"));
        QTRY_COMPARE(fugue::loadProviderInstances(providers).value().first().enabled, false);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The search and the catalogue, by keyboard alone: typing filters the list and opens the catalogue (a query nothing answers says so and
    // offers to clear it), Down goes to the first match, Enter on its Add adds fal.ai, its row opens, the focus lands in its key field,
    // and what is typed there shows as dots.
    void theSearchFiltersAndAddingAServiceFocusesTheMaskedKey() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-providers"));
        QVERIFY(!find(app.window, "providerAddService")); // the dropdown is gone: the catalogue is a row
        QQuickItem *search = NEED("providerSearch");
        QVERIFY(!find(app.window, "providerPreset-fal")); // the catalogue starts shut
        QQuickItem *input = search->findChild<QQuickItem *>("providerSearchInput");
        QVERIFY(input);
        QMetaObject::invokeMethod(search, "focusField");
        input->setProperty("text", "xyz");
        QTRY_VERIFY(find(app.window, "providerNoMatch"));
        QVERIFY(find(app.window, "providerClearSearch"));
        QVERIFY(!find(app.window, "providerCatalogue")); // nothing anywhere: the catalogue steps aside
        click(app, NEED("providerClearSearch"));
        QTRY_VERIFY(find(app.window, "providerRow-0"));
        QVERIFY(!find(app.window, "providerNoMatch"));
        QMetaObject::invokeMethod(search, "focusField");
        for (const QChar c : QStringLiteral("fal"))
            QTest::keyClick(app.window, c.toLatin1());
        QTRY_VERIFY(find(app.window, "providerPreset-fal")); // typing opens the catalogue and filters it
        QVERIFY(!find(app.window, "providerPreset-openai"));
        QVERIFY(find(app.window, "providerNoMatch")); // none of the services on the list match
        QVERIFY(!find(app.window, "providerRow-0")); // and the list is filtered too
        QTest::keyClick(app.window, Qt::Key_Down);
        QQuickItem *chip = NEED("providerPresetAdd-fal");
        QTRY_VERIFY(app.window->activeFocusItem() == chip);
        const int before = app.settings.providers().size();
        QTest::keyClick(app.window, Qt::Key_Return);
        const int fal = app.settings.providers().size() - 1;
        QCOMPARE(fal, before);
        QCOMPARE(app.settings.providers().last().toMap().value("name").toString(), QStringLiteral("fal.ai"));
        QQuickItem *key = NEED(QStringLiteral("providerKey-%1").arg(fal));
        QTRY_VERIFY(app.window->activeFocusItem() && key->isAncestorOf(app.window->activeFocusItem()));
        QVERIFY(find(app.window, QStringLiteral("providerPrivacy-%1").arg(fal)));
        QCOMPARE(input->property("text").toString(), QString()); // the search cleared
        QTRY_VERIFY(!find(app.window, "providerPreset-fal")); // and the catalogue shut
        for (const QChar c : QStringLiteral("sk-abc"))
            QTest::keyClick(app.window, c.toLatin1());
        QCOMPARE(app.window->activeFocusItem()->property("echoMode").toInt(), 2 /* TextInput.Password */);
        // With a filter on, moving is off and Enter in the search never adds.
        QMetaObject::invokeMethod(search, "focusField");
        input->setProperty("text", "fal");
        QTRY_VERIFY(!find(app.window, "providerRow-0"));
        QTest::keyClick(app.window, Qt::Key_Return);
        QCOMPARE(app.settings.providers().size(), before + 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The rows by keys alone, and the two fixed bugs. Down from the search, End, Enter opens, Down goes on to "Add a service", Alt+Up moves
    // the service (its open row and the focus go with it, Advanced too: the page keeps its state by name), Tab goes to its switch and into
    // its body, Escape closes the row with the focus on its header and then closes Settings. A switch flipped by keyboard keeps the
    // focus although the rows are made again.
    void theRowsWorkByKeysAndKeepTheirStateByName() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QCOMPARE(app.settings.addPreset("fal"), QString());
        const int fal = app.settings.providers().size() - 1;
        QVERIFY(fal >= 1);
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-providers"));
        const auto header = [&](int n) { return NEED(QStringLiteral("providerRow-%1").arg(n))->property("header").value<QQuickItem *>(); };
        QVERIFY(!header(fal)->activeFocusOnTab()); // roving: only the first row's header is a Tab stop
        QVERIFY(header(0)->activeFocusOnTab());
        // The bug of the keyboard switch: Space flips it, the rows are made again, the focus is still on it.
        NEED("providerEnabled-0")->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(app.window, Qt::Key_Space);
        QTRY_COMPARE(fugue::loadProviderInstances(app.settings.providersPath()).value().first().enabled, false);
        QTRY_VERIFY(app.window->activeFocusItem() == find(app.window, "providerEnabled-0"));
        QTest::keyClick(app.window, Qt::Key_Space); // and again: it still has it
        QTRY_COMPARE(fugue::loadProviderInstances(app.settings.providersPath()).value().first().enabled, true);
        QTRY_VERIFY(app.window->activeFocusItem() == find(app.window, "providerEnabled-0"));
        // Down, End, Enter.
        QMetaObject::invokeMethod(NEED("providerSearch"), "focusField");
        QTest::keyClick(app.window, Qt::Key_Down);
        QTRY_VERIFY(app.window->activeFocusItem() == header(0));
        QTest::keyClick(app.window, Qt::Key_End);
        QTRY_VERIFY(app.window->activeFocusItem() == header(fal));
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(find(app.window, QStringLiteral("providerBody-%1").arg(fal)));
        QTest::keyClick(app.window, Qt::Key_Down); // Down from the last header goes on to "Add a service"
        QTRY_VERIFY(app.window->activeFocusItem() == NEED("providerCatalogue")->property("header").value<QQuickItem *>());
        QTest::keyClick(app.window, Qt::Key_Up);
        QTRY_VERIFY(app.window->activeFocusItem() == header(fal));
        // The bug of Advanced by place: open for fal.ai, then move fal.ai up.
        QTest::qWait(300); // the body grows over 160 ms
        test_support::grab(app.window); // a grab polishes the layout: the chip is where it will be clicked
        QVERIFY(!find(app.window, QStringLiteral("providerName-%1").arg(fal))); // a cloud service keeps its name under Advanced
        click(app, NEED(QStringLiteral("providerAdvanced-%1").arg(fal)));
        NEED(QStringLiteral("providerName-%1").arg(fal));
        header(fal)->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(app.window, Qt::Key_Up, Qt::AltModifier);
        QTRY_COMPARE(app.settings.providers().at(fal - 1).toMap().value("name").toString(), QStringLiteral("fal.ai"));
        QTRY_VERIFY(app.window->activeFocusItem() == header(fal - 1)); // the focus stayed with the service
        QVERIFY(find(app.window, QStringLiteral("providerBody-%1").arg(fal - 1))); // and so did its open row
        NEED(QStringLiteral("providerName-%1").arg(fal - 1)); // and its Advanced
        QVERIFY(!find(app.window, QStringLiteral("providerBody-%1").arg(fal)));
        // Opening the row that took its old place closes this one (one at a time).
        header(fal)->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!find(app.window, QStringLiteral("providerBody-%1").arg(fal - 1)));
        NEED(QStringLiteral("providerBody-%1").arg(fal));
        // Back to fal.ai: Tab to its switch, then into its body; Escape closes the row, then Settings.
        header(fal - 1)->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(find(app.window, QStringLiteral("providerBody-%1").arg(fal - 1)));
        QTest::keyClick(app.window, Qt::Key_Tab);
        QCOMPARE(app.window->activeFocusItem(), NEED(QStringLiteral("providerEnabled-%1").arg(fal - 1)));
        QTest::keyClick(app.window, Qt::Key_Tab);
        QTRY_VERIFY(NEED(QStringLiteral("providerKey-%1").arg(fal - 1))->isAncestorOf(app.window->activeFocusItem()));
        QTest::keyClick(app.window, Qt::Key_Escape); // one layer per press: the row, with the focus on its header
        QTRY_VERIFY(!find(app.window, QStringLiteral("providerBody-%1").arg(fal - 1)));
        QTRY_VERIFY(app.window->activeFocusItem() == header(fal - 1));
        QVERIFY(panelOpen(app.window));
        // Removing a service with no key stored needs no question; the focus goes to the next header (the previous, when it was the last).
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(find(app.window, QStringLiteral("providerBody-%1").arg(fal - 1)));
        QTest::qWait(300);
        test_support::grab(app.window);
        click(app, NEED(QStringLiteral("providerRemove-%1").arg(fal - 1)));
        QTRY_COMPARE(app.settings.providers().size(), fal);
        QTRY_VERIFY(app.window->activeFocusItem() == header(fal - 1));
        QTest::keyClick(app.window, Qt::Key_Escape); // closed: Escape is the panel's
        QTRY_VERIFY(!panelOpen(app.window));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void keymapPageOpensTheFileAndShowsItsProblems() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        const QString keymap = fugue::CommandRegistry::keymapPath();
        QVERIFY(keymap.startsWith(m_config.path()));
        QFile::remove(keymap);
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-keymap"));
        click(app, NEED("keymapOpen"));
        QTRY_COMPARE(app.openedUrls.size(), 1);
        QCOMPARE(app.openedUrls.first(), QUrl::fromLocalFile(keymap));
        QVERIFY(QFile::exists(keymap)); // an empty diff to start from
        QFile file(keymap);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"add": [{"command": "no.such.command", "key": "F7"}]})");
        file.close();
        click(app, NEED("keymapReload"));
        QTRY_VERIFY(!app.commands.problems().isEmpty());
        QFile::remove(keymap);
        app.commands.loadKeymap(keymap);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void everyPageOpensWithoutWarnings() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        for (const QString &page : QStringList{"general", "folders", "run", "providers", "keymap", "theme"}) {
            click(app, NEED("settingsPage-" + page));
            QTest::qWait(20);
        }
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A page taller than the window shows the bar over its right edge: no text runs under it, and the
    // wheel and the thumb scroll the page. At least one page overflows in a short window.
    void aTallPageKeepsItsContentClearOfTheScrollBar() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.window->resize(app.window->width(), 420);
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        int overflowing = 0;
        for (const QString &index : QStringList{"general", "folders", "run", "providers", "keymap", "theme"}) {
            click(app, NEED("settingsPage-" + index));
            QTest::qWait(60);
            QQuickItem *page = nullptr;
            for (QQuickItem *flick : app.window->contentItem()->findChildren<QQuickItem *>())
                if (flick->property("contentHeight").isValid() && scroll_checks::barOf(flick) && flick->isVisible())
                    page = flick;
            QVERIFY(page);
            QCOMPARE(page->property("contentY").toReal(), 0.0); // the page before was scrolled; this one starts at its top
            QQuickItem *bar = scroll_checks::barOf(page);
            if (!bar->isVisible())
                continue;
            ++overflowing;
            const QString under = scroll_checks::contentUnderBar(page, bar);
            QVERIFY2(under.isEmpty(), qPrintable(QStringLiteral("page %1: %2").arg(index, under)));
            const QString failed = scroll_checks::scrollsByWheelAndThumb(app.window, page);
            QVERIFY2(failed.isEmpty(), qPrintable(QStringLiteral("page %1: %2").arg(index, failed)));
            page->setProperty("contentY", page->property("contentHeight").toReal() - page->height()); // left at its foot
        }
        QVERIFY(overflowing > 0);
    }

    // FUGUE_SETTINGS_SHOTS=<dir> [FUGUE_SHOT_THEME=<omarchy theme>] [FUGUE_SHOT_LOOK="preset=flat"] [QT_QUICK_BACKEND=rhi]: pages as PNGs, to look at,
    // at the two sizes of the owner's checks (one theme and look per process).
    void screenshots() {
        const QString out = qEnvironmentVariable("FUGUE_SETTINGS_SHOTS");
        if (out.isEmpty())
            QSKIP("Set FUGUE_SETTINGS_SHOTS to a folder to write the pages there.");
        QDir().mkpath(out);
        App app(load(emptyDoc), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        QVERIFY(app.open());
        for (const QString &part : qEnvironmentVariable("FUGUE_SHOT_LOOK").split(';', Qt::SkipEmptyParts)) {
            const QStringList kv = part.split('=');
            QVERIFY(kv.size() == 2);
            QCOMPARE(kv[0] == "preset" ? app.settings.applyLookPreset(kv[1]) : app.settings.set(kv[0], kv[1].toDouble()), QString());
        }
        QCOMPARE(app.settings.addPath("model_paths", "~/ComfyUI/models"), QString());
        QCOMPARE(app.settings.addProvider("omarchy"), QString());
        QCOMPARE(app.settings.setProviderField(1, "key", "k"), QString());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        for (const QSize size : {QSize(1888, 971), QSize(900, 700)}) {
            app.window->resize(size);
            for (const QString &page : QStringList{"general", "folders", "run", "providers", "keymap", "theme", "packs", "cache"}) {
                click(app, NEED("settingsPage-" + page));
                QTest::qWait(300);
                QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/settings-%2-%3x%4.png").arg(out, page).arg(size.width()).arg(size.height())));
            }
        }
    }

    // FUGUE_SETTINGS_SHOTS=<dir> (as screenshots()): the Providers page as a person meets it: the collapsed list (the default install and a fal.ai
    // row), one row open, the search "flux", the catalogue open, a search nothing answers, and the list in a narrow window.
    void providerShots() {
        const QString out = qEnvironmentVariable("FUGUE_SETTINGS_SHOTS");
        if (out.isEmpty())
            QSKIP("Set FUGUE_SETTINGS_SHOTS to a folder to write the pages there.");
        QDir().mkpath(out);
        App app(load(emptyDoc), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        QVERIFY(app.open());
        for (const QString &part : qEnvironmentVariable("FUGUE_SHOT_LOOK").split(';', Qt::SkipEmptyParts)) {
            const QStringList kv = part.split('=');
            QVERIFY(kv.size() == 2);
            QCOMPARE(kv[0] == "preset" ? app.settings.applyLookPreset(kv[1]) : app.settings.set(kv[0], kv[1].toDouble()), QString());
        }
        const QString tag = qEnvironmentVariable("FUGUE_SHOT_TAG", QStringLiteral("dark"));
        const auto shot = [&](const QString &name) {
            QTest::qWait(500);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/providers-%2-%3.png").arg(out, name, tag)));
        };
        QCOMPARE(app.settings.addPreset("fal"), QString());
        const int fal = app.settings.providers().size() - 1;
        app.window->resize(1300, 850);
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-providers"));
        shot("1-list");
        click(app, NEED(QStringLiteral("providerRow-%1").arg(fal)));
        shot("2-open");
        click(app, NEED(QStringLiteral("providerRow-%1").arg(fal))->property("header").value<QQuickItem *>()); // shut again (the header: the row is tall now)
        QMetaObject::invokeMethod(NEED("providerSearch"), "focusField");
        for (const QChar c : QStringLiteral("flux"))
            QTest::keyClick(app.window, c.toLatin1());
        shot("3-search");
        QQuickItem *input = NEED("providerSearch")->findChild<QQuickItem *>("providerSearchInput");
        input->setProperty("text", "");
        QTRY_VERIFY(find(app.window, "providerPreset-fal")); // typing opened the catalogue, and clearing the search left it open
        shot("4-catalogue");
        input->setProperty("text", "xyz");
        QTRY_VERIFY(find(app.window, "providerClearSearch"));
        shot("5-nomatch");
        input->setProperty("text", "");
        QTRY_VERIFY(find(app.window, "providerRow-0"));
        app.window->resize(520, 760);
        shot("6-narrow");
    }
};

FUGUE_TEST_MAIN(TestSettingsPanel, QGuiApplication)
#include "test_settings_panel.moc"
