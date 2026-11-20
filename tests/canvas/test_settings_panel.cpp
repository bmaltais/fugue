// The settings panel in the real window (#51): Ctrl+, opens it, a change made with the mouse
// and keys applies live and is in the file, so a model built from that file has it. The
// files live in a temporary XDG_CONFIG_HOME, never the owner's.

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_app.h"
#include "engine/json.h"
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

// Clicks an item once the layouts have placed it: a list that just changed moves its rows
// when the next frame is polished, so the item must stay put for a few frames first.
void click(App &app, QQuickItem *item) {
    QPoint at = centerOf(item);
    for (int still = 0, i = 0; still < 3 && i < 50; ++i) {
        QTest::qWait(20);
        const QPoint now = centerOf(item);
        still = now == at ? still + 1 : 0;
        at = now;
    }
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
