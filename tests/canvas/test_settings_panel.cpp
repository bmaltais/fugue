// The settings panel in the real window (#51): Ctrl+, opens it, a change made with the mouse
// and keys applies live and is in the file, so a model built from that file has it. The
// files live in a temporary XDG_CONFIG_HOME, never the owner's.

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_app.h"
#include "../scroll_checks.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/providers.h"

#include <QJsonDocument>

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

    // General > Corner radius (#63): fixed at 8 px by default, a step at a time from 0 to 16, or
    // Omarchy's rounding (none here: square); the canvas redraws at once and the file keeps it.
    void theCornerRadiusChangesLiveAndPersists() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        const auto corner = [&] { return app.theme.look()->property("corner").toInt(); };
        QCOMPARE(corner(), 8);
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        QCOMPARE(NEED("cornerValue")->property("text").toString(), QStringLiteral("8 px"));
        click(app, NEED("cornerMore"));
        QTRY_COMPARE(corner(), 9);
        click(app, NEED("cornerLess"));
        click(app, NEED("cornerLess"));
        QTRY_COMPARE(corner(), 7);
        QCOMPARE(fugue::SettingsModel(app.settings.path()).cornerRadius(), 7);
        click(app, NEED("corner-omarchy"));
        QTRY_COMPARE(corner(), 0);
        QTRY_VERIFY(!find(app.window, "cornerValue"));
        click(app, NEED("corner-fixed"));
        QTRY_COMPARE(corner(), 8);
        // Held to the range at its ends.
        for (int i = 0; i < 10; ++i)
            click(app, NEED("cornerMore"));
        QTRY_COMPARE(corner(), 16);
        QVERIFY(!NEED("cornerMore")->isEnabled());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aTypedFolderIsCheckedAndPersists() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-1"));
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
        click(app, NEED("settingsPage-1"));
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
        click(app, NEED("settingsPage-1"));
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
        click(app, NEED("settingsPage-2"));
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
        click(app, NEED("settingsPage-3"));
        NEED("providerDefault-0");
        QVERIFY(!QFile::exists(providers));
        type(app, NEED("providerKey-0"), "sk-hidden-123");
        QTRY_VERIFY(QFile::exists(providers));
        QCOMPARE(fugue::loadProviderInstances(providers).value().first().key, QStringLiteral("sk-hidden-123"));
        QTRY_VERIFY(!find(app.window, "providerDefault-0")); // saved now
        NEED("providerKeyClear-0");
        // The field never shows the key, and nothing in the panel's model holds it. (The
        // cards are made again from the new list, so the field is found again.)
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

    void keymapPageOpensTheFileAndShowsItsProblems() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        const QString keymap = fugue::CommandRegistry::keymapPath();
        QVERIFY(keymap.startsWith(m_config.path()));
        QFile::remove(keymap);
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        click(app, NEED("settingsPage-4"));
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
        for (int page = 0; page < 6; ++page) {
            click(app, NEED(QStringLiteral("settingsPage-%1").arg(page)));
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
        for (int index = 0; index < 6; ++index) {
            click(app, NEED(QStringLiteral("settingsPage-%1").arg(index)));
            QTest::qWait(60);
            QQuickItem *page = nullptr;
            for (QQuickItem *flick : app.window->contentItem()->findChildren<QQuickItem *>())
                if (flick->property("contentHeight").isValid() && scroll_checks::barOf(flick) && flick->isVisible())
                    page = flick;
            QVERIFY(page);
            QQuickItem *bar = scroll_checks::barOf(page);
            if (!bar->isVisible())
                continue;
            ++overflowing;
            const QString under = scroll_checks::contentUnderBar(page, bar);
            QVERIFY2(under.isEmpty(), qPrintable(QStringLiteral("page %1: %2").arg(index).arg(under)));
            const QString failed = scroll_checks::scrollsByWheelAndThumb(app.window, page);
            QVERIFY2(failed.isEmpty(), qPrintable(QStringLiteral("page %1: %2").arg(index).arg(failed)));
        }
        QVERIFY(overflowing > 0);
    }

    // FUGUE_SETTINGS_SHOTS=<dir> QT_QUICK_BACKEND=rhi: each page as a PNG, to look at.
    void screenshots() {
        const QString out = qEnvironmentVariable("FUGUE_SETTINGS_SHOTS");
        if (out.isEmpty())
            QSKIP("Set FUGUE_SETTINGS_SHOTS to a folder to write the pages there.");
        App app(load(emptyDoc), {}, Look{"ethereal", 8});
        QVERIFY(app.open());
        QCOMPARE(app.settings.addPath("model_paths", "~/ComfyUI/models"), QString());
        QCOMPARE(app.settings.addProvider("omarchy"), QString());
        QCOMPARE(app.settings.setProviderField(1, "key", "k"), QString());
        app.commands.trigger("app.settings");
        QTRY_VERIFY(panelOpen(app.window));
        for (int page = 0; page < 6; ++page) {
            click(app, NEED(QStringLiteral("settingsPage-%1").arg(page)));
            QTest::qWait(300);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/settings-%2.png").arg(out).arg(page)));
        }
    }
};

QTEST_MAIN(TestSettingsPanel)
#include "test_settings_panel.moc"
