// The Packs page in the real window (#25): installing, trusting and removing a pack with the
// mouse and keys, each only after the question on the page is answered. The packs, the trust
// file and the settings live in temporary XDG folders, never the owner's.

#include <algorithm>
#include <functional>

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_app.h"
#include "engine/json.h"
#include "engine/pack_manager.h"
#include "engine/pack_secrets.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "../test_shard.h"
#include "../window_grab.h"

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

QString textOf(QQuickItem *item) { return item->property("text").toString(); }

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
// when the next frame is polished, so the item must stay put for a few frames first. A refresh
// of the packs list also rebuilds the rows (the item found a moment ago is deleted, #127): the
// item is looked up again by name after every wait, never held across one.
void click(App &app, QQuickItem *item) {
    const QString name = item->objectName();
    QPoint at = centerOf(item);
    int still = 0;
    for (int i = 0; still < 5 && i < 500; ++i) {
        QTest::qWait(10);
        QQuickItem *now = find(app.window, name);
        if (!now) {
            still = 0; // gone for now: wait for the rebuilt row
            continue;
        }
        const QPoint here = centerOf(now);
        still = here == at ? still + 1 : 0;
        at = here;
    }
    QVERIFY2(still >= 3, qPrintable(QStringLiteral("%1 never settled").arg(name)));
    QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
}

// Clicks the header of the disclosure row `row` once it has stayed put for a few frames; looked up again by name each frame (the
// header has no name of its own, and a change of the list makes the rows again).
void clickHeader(App &app, const QString &row) {
    QPoint at;
    int still = 0;
    for (int i = 0; still < 5 && i < 500; ++i) {
        QTest::qWait(10);
        QQuickItem *item = find(app.window, row);
        if (!item) {
            still = 0;
            continue;
        }
        const QPoint here = centerOf(item->property("header").value<QQuickItem *>());
        still = here == at ? still + 1 : 0;
        at = here;
    }
    QVERIFY2(still >= 3, qPrintable(QStringLiteral("%1 never settled").arg(row)));
    QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
}

// True once the disclosure row `name` is open and its body has finished growing (a click on its controls before that lands on the clip).
bool openAndGrown(QQuickWindow *window, const QString &name) {
    QQuickItem *row = find(window, name);
    return row && row->property("expanded").toBool() && !row->property("animating").toBool()
           && qAbs(row->property("shown").toReal() - row->property("bodyHeight").toReal()) < 0.5;
}

// Opens the row named `row` (a KitDisclosureRow) unless `part`, something in its body, shows: a closed row's body is not visible.
void openFor(App &app, const QString &row, const QString &part) {
    if (find(app.window, part))
        return;
    clickHeader(app, row);
    QTRY_VERIFY_WITH_TIMEOUT(find(app.window, part), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(openAndGrown(app.window, row), 20000);
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


class TestPacksPage : public QObject {
    Q_OBJECT

    QTemporaryDir m_xdg;
    QString m_source = QDir::tempPath() + "/fugue-packs-page-source-" + QString::number(QCoreApplication::applicationPid()); // lowercase: typed key by key
    QString packs() const { return m_xdg.filePath("data/fugue/packs"); }
    QString trustFile() const { return m_xdg.filePath("state/fugue/trust.json"); }

    void makePack() const {
        QDir(m_source).removeRecursively();
        QDir().mkpath(m_source + "/tool");
        QFile pack(m_source + "/pack.json");
        QVERIFY(pack.open(QIODevice::WriteOnly));
        pack.write(QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.tools"}, {"version", "1.0.0"}, {"name", "Tools"},
                                             {"nodes", QJsonArray{"tool"}}}).toJson());
        QFile node(m_source + "/tool/node.json");
        QVERIFY(node.open(QIODevice::WriteOnly));
        node.write(QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.tools/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                                             {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                                             {"inputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
                                             {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}}}).toJson());
    }

private slots:
    void initTestCase() {
        qputenv("XDG_CONFIG_HOME", m_xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", m_xdg.filePath("data").toUtf8());
        qputenv("XDG_STATE_HOME", m_xdg.filePath("state").toUtf8());
    }
    void cleanupTestCase() { QDir(m_source).removeRecursively(); }
    void init() { QDir(packs()).removeRecursively(); QFile::remove(trustFile()); makePack(); }

    void saysNoAndNothingIsInstalled() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY_WITH_TIMEOUT(panelOpen(app.window), 20000);
        click(app, NEED("settingsPage-packs"));
        openFor(app, "packAddRow", "packLocation");
        type(app, NEED("packLocation"), m_source);
        click(app, NEED("packAdd"));
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "packQuestion"), 20000);
        QVERIFY(find(app.window, "packQuestionDetail"));
        QTRY_VERIFY_WITH_TIMEOUT(NEED("packNo")->isEnabled(), 20000);
        click(app, NEED("packNo"));
        QTRY_VERIFY_WITH_TIMEOUT(!find(app.window, "packQuestion"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "packsFailure"), 20000);
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void installsTrustsAndRemovesAfterEachQuestion() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.commands.trigger("app.settings");
        QTRY_VERIFY_WITH_TIMEOUT(panelOpen(app.window), 20000);
        click(app, NEED("settingsPage-packs"));

        openFor(app, "packAddRow", "packLocation");
        type(app, NEED("packLocation"), m_source);
        click(app, NEED("packAdd"));
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "packQuestion"), 20000);
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists()); // nothing before the yes
        QTRY_VERIFY_WITH_TIMEOUT(NEED("packYes")->isEnabled(), 20000);
        click(app, NEED("packYes"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(packs() + "/io.example.tools/pack.json").isFile(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "pack-io.example.tools"), 20000);
        // Usable at once (#70): the window's node types have it, with no restart.
        QTRY_VERIFY_WITH_TIMEOUT(app.registry.nodeType("io.example.tools/tool"), 20000);
        QCOMPARE(fugue::TrustStore(trustFile()).isTrusted("io.example.tools", "1.0.0"), false);

        QCOMPARE(textOf(NEED("packState-io.example.tools")), QString("not trusted")); // no "inherited," on an own pack (#541)
        click(app, NEED("packTrust-io.example.tools"));
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "packQuestion"), 20000);
        QVERIFY(!textOf(NEED("packQuestionDetail")).contains("Inherited from Default")); // the warning is for inherited packs only
        QCOMPARE(fugue::TrustStore(trustFile()).isTrusted("io.example.tools", "1.0.0"), false);
        QTRY_VERIFY_WITH_TIMEOUT(NEED("packYes")->isEnabled(), 20000);
        click(app, NEED("packYes"));
        QTRY_COMPARE(fugue::TrustStore(trustFile()).isTrusted("io.example.tools", "1.0.0"), true);

        openFor(app, "pack-io.example.tools", "packRemove-io.example.tools");
        click(app, NEED("packRemove-io.example.tools"));
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "packQuestion"), 20000);
        QVERIFY(QFileInfo(packs() + "/io.example.tools").isDir());
        QTRY_VERIFY_WITH_TIMEOUT(NEED("packYes")->isEnabled(), 20000);
        click(app, NEED("packYes"));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo(packs() + "/io.example.tools").exists(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!find(app.window, "pack-io.example.tools"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!app.registry.nodeType("io.example.tools/tool"), 20000);
        QCOMPARE(fugue::TrustStore(trustFile()).isTrusted("io.example.tools", "1.0.0"), false);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #74: the library's groups are listed on the page, renamed and removed there (removing
    // asks first), and the window's node types follow at once.
    void theLibrarysGroupsAreRenamedAndRemovedOnThePage() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {
            "words": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a fox"}}}},
            "layout": {"nodes": {"words": {"pos": [0, 0]}}}})"));
        QVERIFY(app.open());
        app.model.select({"words"});
        const QString instance = app.model.groups()->makeGroup();
        app.model.select({instance});
        QVERIFY2(app.model.groups()->saveGroupToLibrary(), qPrintable(app.model.groups()->problem()));
        QVERIFY(QFileInfo(packs() + "/local/group/node.json").isFile());
        app.commands.trigger("app.settings");
        QTRY_VERIFY_WITH_TIMEOUT(panelOpen(app.window), 20000);
        click(app, NEED("settingsPage-packs"));
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "libraryGroup-local/group"), 20000);

        click(app, NEED("libraryRename-local/group"));
        type(app, NEED("libraryName-local/group"), "renamed");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(packs() + "/local/renamed/node.json").isFile(), 20000);
        QVERIFY(!QFileInfo(packs() + "/local/group").exists());
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "libraryGroup-local/renamed"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(app.registry.nodeType("local/renamed"), 20000);

        click(app, NEED("libraryRemove-local/renamed"));
        QVERIFY(QFileInfo(packs() + "/local/renamed").isDir()); // nothing before the yes
        click(app, NEED("libraryRemoveYes-local/renamed"));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo(packs() + "/local/renamed").exists(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!find(app.window, "libraryGroup-local/renamed"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!app.registry.nodeType("local/renamed"), 20000);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aStaleAnswerCannotAnswerTheNextQuestion() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        fugue::PacksModel *model = app.settings.packs();
        model->add(m_source, {});
        QTRY_VERIFY_WITH_TIMEOUT(!model->question().isEmpty(), 20000);
        model->answer(true); // too soon: it was meant for nothing on this question
        QVERIFY(!model->question().isEmpty());
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());
        QTRY_VERIFY_WITH_TIMEOUT(model->questionReady(), 20000);
        model->cancel(); // a cancel answers no
        QVERIFY(model->waitIdle());
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());
        QVERIFY(model->failure().contains("not installed"));
    }

    void saidNoToAnUpdateIsNotNothingToUpdate() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        fugue::PacksModel *model = app.settings.packs();
        model->add(m_source, {});
        QTRY_VERIFY_WITH_TIMEOUT(model->questionReady(), 20000);
        model->answer(true);
        QVERIFY(model->waitIdle());
        QVERIFY(QFileInfo(packs() + "/io.example.tools/pack.json").isFile());
        QFile bump(m_source + "/pack.json");
        QVERIFY(bump.open(QIODevice::WriteOnly));
        bump.write(QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.tools"}, {"version", "1.1.0"}, {"name", "Tools"},
                                             {"nodes", QJsonArray{"tool"}}}).toJson());
        bump.close();
        model->update("io.example.tools");
        QTRY_VERIFY_WITH_TIMEOUT(model->questionReady(), 20000);
        QCOMPARE(model->question().value("kind").toString(), QString("update"));
        model->answer(false);
        QVERIFY(model->waitIdle());
        QVERIFY(model->message().contains("said no"));
    }

    // #402: a pack that declares a secret shows it on its card as set or not set, the person types the value and
    // presses Enter (keys only), and the list never carries the value; Clear drops it.
    void aDeclaredSecretIsSetFromTheKeyboardAndShownOnlyAsSetOrNotSet() {
        const QString dir = packs() + "/io.example.keyed";
        QDir().mkpath(dir + "/call");
        QFile pack(dir + "/pack.json");
        QVERIFY(pack.open(QIODevice::WriteOnly));
        pack.write(QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.keyed"}, {"version", "1.0.0"}, {"name", "Keyed"},
                                             {"nodes", QJsonArray{"call"}},
                                             {"settings", QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}, {"label", "Token 1"}, {"origin", "required"}}}}}).toJson());
        pack.close();
        QFile node(dir + "/call/node.json");
        QVERIFY(node.open(QIODevice::WriteOnly));
        node.write(QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.example.keyed/call"}, {"version", "1.0.0"}, {"name", "Call"},
                                             {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                                             {"inputs", QJsonArray{}}, {"outputs", QJsonArray{}}}).toJson());
        node.close();
        QFile::remove(m_xdg.filePath("config/fugue/pack-secrets.json"));

        App app(load(emptyDoc));
        QVERIFY(app.open());
        fugue::PacksModel *model = app.settings.packs();
        const auto secretOf = [model] {
            for (const QVariant &entry : model->packs())
                if (entry.toMap().value("id") == "io.example.keyed")
                    return entry.toMap().value("secrets").toList().value(0).toMap();
            return QVariantMap();
        };
        QCOMPARE(secretOf().value("name").toString(), QString("token_1"));
        QVERIFY(!secretOf().value("set").toBool());

        app.commands.trigger("app.settings");
        QTRY_VERIFY_WITH_TIMEOUT(panelOpen(app.window), 20000);
        click(app, NEED("settingsPage-packs"));
        openFor(app, "pack-io.example.keyed", "packSecret-io.example.keyed-token_1");
        QCOMPARE(find(app.window, "packSecretState-io.example.keyed-token_1")->property("text").toString(), QString("not set"));
        // The pack needs an origin: a value alone is refused, and nothing is stored.
        type(app, NEED("packSecret-io.example.keyed-token_1"), "made-up-key-1234");
        QTRY_VERIFY_WITH_TIMEOUT(model->failure().contains("origin"), 20000);
        QVERIFY(!secretOf().value("set").toBool());
        type(app, NEED("packSecretOnlyFor-io.example.keyed-token_1"), "https://api.example.com");
        type(app, NEED("packSecret-io.example.keyed-token_1"), "made-up-key-1234");
        QTRY_VERIFY_WITH_TIMEOUT(secretOf().value("set").toBool(), 20000);
        QCOMPARE(secretOf().value("origin").toString(), QString("https://api.example.com"));
        QVERIFY(!QString::fromUtf8(QJsonDocument::fromVariant(model->packs()).toJson()).contains("made-up-key-1234"));
        QTRY_COMPARE_WITH_TIMEOUT(find(app.window, "packSecretState-io.example.keyed-token_1")->property("text").toString(), QString("set"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "packSecretOrigin-io.example.keyed-token_1"), 20000); // shown, read-only
        QVERIFY(!find(app.window, "packSecretOnlyFor-io.example.keyed-token_1") || !find(app.window, "packSecretOnlyFor-io.example.keyed-token_1")->isVisible());
        QCOMPARE(QFileInfo(m_xdg.filePath("config/fugue/pack-secrets.json")).permissions() & QFileDevice::ReadOther, QFileDevice::Permissions());

        // The same rules as the command line: one line, no NUL, at most 4096 bytes, an origin that is one.
        QVERIFY(model->clearSecret("io.example.keyed", "token_1"));
        QVERIFY(!model->setSecret("io.example.keyed", "token_1", "two\nlines", "https://api.example.com"));
        QVERIFY(!model->setSecret("io.example.keyed", "token_1", QString(4097, 'a'), "https://api.example.com"));
        QVERIFY(!model->setSecret("io.example.keyed", "token_1", "made-up-key-1234", "https://api.example.com/path"));
        QVERIFY(!model->setSecret("io.example.keyed", "token_1", "made-up-key-1234"));
        QVERIFY(model->setSecret("io.example.keyed", "token_1", "made-up-key-1234", "https://api.example.com"));
        QVERIFY(!model->setSecret("io.example.keyed", "undeclared", "made-up-key-1234")); // only a declared name
        click(app, NEED("packSecretClear-io.example.keyed-token_1"));
        QTRY_VERIFY_WITH_TIMEOUT(!secretOf().value("set").toBool(), 20000);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aPackThatDoesNotLoadShowsWhy() {
        QDir().mkpath(packs() + "/io.example.broken");
        QFile broken(packs() + "/io.example.broken/pack.json");
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{ not json");
        broken.close();
        App app(load(emptyDoc));
        QVERIFY(app.open());
        const QVariantList list = app.settings.packs()->packs();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().toMap().value("enabled").toBool(), false);
        QVERIFY(!list.first().toMap().value("problems").toStringList().isEmpty());
    }

    // Writes a pack of `id` at `version` into `folder` (a name under the packs folder), from `source`, declaring one secret.
    void writeKeyedPack(const QString &folder, const QString &id, const QString &version, const QString &source) const {
        const QString dir = packs() + "/" + folder;
        QDir().mkpath(dir);
        QFile pack(dir + "/pack.json");
        QVERIFY(pack.open(QIODevice::WriteOnly));
        pack.write(QJsonDocument(QJsonObject{{"manifest", 1}, {"id", id}, {"version", version}, {"name", "Dup"}, {"nodes", QJsonArray{}},
                                             {"settings", QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}, {"label", "Token"}, {"origin", "required"}}}}}).toJson());
        pack.close();
        QFile from(dir + "/.fugue-source.json");
        QVERIFY(from.open(QIODevice::WriteOnly));
        from.write(QJsonDocument(QJsonObject{{"kind", "path"}, {"location", source}}).toJson());
    }

    // A secret is bound to the pack's source (#402): with two packs of one id from different sources, the key set on one row
    // is stored for that row's source, never the other's.
    void aSecretIsBoundToThePacksOwnSourceWhenTwoShareAnId() {
        writeKeyedPack("io.example.dup", "io.example.dup", "2.0.0", "/src/two");
        writeKeyedPack("io.example.dup@1", "io.example.dup", "1.0.0", "/src/one");
        App app(load(emptyDoc));
        QVERIFY(app.open());
        fugue::PacksModel *model = app.settings.packs();
        QString two, one;
        for (const QVariant &entry : model->packs()) {
            const QVariantMap pack = entry.toMap();
            if (pack.value("id") == "io.example.dup")
                (pack.value("version") == "2.0.0" ? two : one) = pack.value("path").toString();
        }
        QVERIFY2(!two.isEmpty() && !one.isEmpty() && two != one, "both packs load");
        const fugue::PackSecretStore store;
        const QString keyOne = fugue::packSecretKey("io.example.dup", "/src/one");
        const QString keyTwo = fugue::packSecretKey("io.example.dup", "/src/two");
        QVERIFY(model->setSecret("io.example.dup", "token_1", "made-up-key-1234", "https://api.example.com", one));
        QVERIFY(store.namesSet(keyOne).contains("token_1"));
        QVERIFY(!store.namesSet(keyTwo).contains("token_1"));
        QVERIFY(model->setSecret("io.example.dup", "token_1", "made-up-key-5678", "https://api.example.com", two));
        QVERIFY(store.namesSet(keyTwo).contains("token_1"));
        QVERIFY(model->clearSecret("io.example.dup", "token_1", two));
        QVERIFY(!store.namesSet(keyTwo).contains("token_1"));
        QVERIFY(store.namesSet(keyOne).contains("token_1")); // the other source's key is untouched
        QVERIFY(model->clearSecret("io.example.dup", "token_1", one));
        QVERIFY(!store.namesSet(keyOne).contains("token_1"));
    }

    // Rows are keyed by path: a broken pack (no id) and two packs sharing an id each have their own row, key, tray and tab stop.
    void rowsOfTheSameIdAndABrokenPackAreSeparate() {
        writeKeyedPack("io.example.dup", "io.example.dup", "2.0.0", "/src/two");
        writeKeyedPack("io.example.dup@1", "io.example.dup", "1.0.0", "/src/one");
        QDir().mkpath(packs() + "/io.example.broken");
        QFile broken(packs() + "/io.example.broken/pack.json");
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{ not json");
        broken.close();
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.window->resize(1300, 850);
        app.commands.trigger("app.settings");
        QTRY_VERIFY_WITH_TIMEOUT(panelOpen(app.window), 20000);
        click(app, NEED("settingsPage-packs"));
        const auto rowsNow = [&] { // the visible pack rows, top to bottom
            QList<QQuickItem *> list;
            std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
                for (QQuickItem *child : item->childItems()) {
                    if (child->isVisible() && child->objectName().startsWith("pack-") && child->property("header").isValid())
                        list << child;
                    walk(child);
                }
            };
            QQuickItem *root = app.window->contentItem();
            walk(root);
            std::sort(list.begin(), list.end(), [](QQuickItem *a, QQuickItem *b) { return a->mapToScene(QPointF()).y() < b->mapToScene(QPointF()).y(); });
            return list;
        };
        const auto headerOf = [](QQuickItem *row) { return row->property("header").value<QQuickItem *>(); };
        QTRY_COMPARE_WITH_TIMEOUT(rowsNow().size(), 3, 20000);
        // One header is a Tab stop, and only its row's Trust… chip: Tab does not walk every row.
        int stops = 0;
        for (QQuickItem *row : rowsNow())
            stops += headerOf(row)->property("activeFocusOnTab").toBool();
        QCOMPARE(stops, 1);
        // Arrows, Home and End step through every row, each its own.
        QMetaObject::invokeMethod(rowsNow().first(), "focusHeader");
        QTRY_VERIFY(app.window->activeFocusItem() == headerOf(rowsNow().first()));
        for (int i = 1; i < 3; ++i) {
            QTest::keyClick(app.window, Qt::Key_Down);
            QTRY_VERIFY(app.window->activeFocusItem() == headerOf(rowsNow().at(i)));
        }
        QTest::keyClick(app.window, Qt::Key_Home);
        QTRY_VERIFY(app.window->activeFocusItem() == headerOf(rowsNow().first()));
        QTest::keyClick(app.window, Qt::Key_End);
        QTRY_VERIFY(app.window->activeFocusItem() == headerOf(rowsNow().last()));
        // One tray at a time, even for rows of one id.
        const auto clickRow = [&](int at) { // the header of the row now at `at`, once the layout has settled
            QTest::qWait(300);
            QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(headerOf(rowsNow().at(at))));
        };
        clickRow(0);
        QTRY_VERIFY(rowsNow().at(0)->property("expanded").toBool());
        QTRY_VERIFY(rowsNow().at(1)->property("expanded").toBool() == false);
        clickRow(1);
        QTRY_VERIFY(rowsNow().at(1)->property("expanded").toBool());
        QTRY_VERIFY(!rowsNow().at(0)->property("expanded").toBool());
        int open = 0;
        for (QQuickItem *row : rowsNow())
            open += row->property("expanded").toBool();
        QCOMPARE(open, 1);
        // The search finds the broken pack by where it is.
        QQuickItem *input = NEED("packSearch")->findChild<QQuickItem *>("packSearchInput");
        input->setProperty("text", "example.broken");
        QTRY_COMPARE(rowsNow().size(), 1);
        QVERIFY(rowsNow().first()->objectName().contains("broken"));
        input->setProperty("text", "");
        QTRY_COMPARE(rowsNow().size(), 3);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // FUGUE_SETTINGS_SHOTS=<dir> [FUGUE_SHOT_THEME=<omarchy theme>] [FUGUE_SHOT_TAG=<word>]: the Packs and Folders pages as a person meets them, as PNGs to
    // look at: the list (a trusted pack, one not trusted, one with a key, one that does not load, a library group), a row open, the add row, a
    // search, a narrow window; the Folders list, a row open, the model folders open with one folder.
    void packsAndFoldersShots() {
        const QString out = qEnvironmentVariable("FUGUE_SETTINGS_SHOTS");
        if (out.isEmpty())
            QSKIP("Set FUGUE_SETTINGS_SHOTS to a folder to write the pages there.");
        QDir().mkpath(out);
        const auto write = [](const QString &path, const QJsonObject &json) {
            QDir().mkpath(QFileInfo(path).path());
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(json).toJson());
        };
        for (const QString id : {"io.example.tools", "io.example.draw", "io.example.keyed"})
            write(packs() + "/" + id + "/pack.json", QJsonObject{{"manifest", 1}, {"id", id}, {"version", id.endsWith("draw") ? "0.12.3" : "1.0.0"},
                  {"name", id.section('.', -1).replace(0, 1, id.section('.', -1).at(0).toUpper())}, {"nodes", QJsonArray{}},
                  {"settings", id.endsWith("keyed") ? QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}, {"label", "API token"},
                      {"description", "Sent to the service this pack's nodes call."}, {"origin", "required"}}} : QJsonArray{}}});
        QDir().mkpath(packs() + "/io.example.broken");
        QFile broken(packs() + "/io.example.broken/pack.json");
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{ not json");
        broken.close();
        App app(load(R"({"fugue": 1, "graph": {"nodes": {
            "words": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a fox"}}}},
            "layout": {"nodes": {"words": {"pos": [0, 0]}}}})"), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        QVERIFY(app.open());
        app.model.select({"words"});
        const QString instance = app.model.groups()->makeGroup();
        app.model.select({instance});
        QVERIFY(app.model.groups()->saveGroupToLibrary());
        fugue::TrustStore trust(trustFile());
        trust.trust("io.example.tools", "1.0.0");
        QVERIFY(app.settings.addPath("model_paths", "~/ComfyUI/models").isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(app.settings.packs()->packs().size() >= 4, 20000);
        const QString tag = qEnvironmentVariable("FUGUE_SHOT_TAG", QStringLiteral("dark"));
        const auto shot = [&](const QString &name) {
            QTest::qWait(500);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/%2-%3.png").arg(out, name, tag)));
        };
        const auto reveal = [&](const QString &name) { // scrolls the panel to the row, as a person does
            QMetaObject::invokeMethod(app.window->findChild<QObject *>("settingsPanel"), "showAll", Q_ARG(QVariant, QVariant::fromValue(NEED(name))), Q_ARG(QVariant, QVariant()));
            QTest::qWait(200);
        };
        app.window->resize(1300, 850);
        app.commands.trigger("app.settings");
        QTRY_VERIFY_WITH_TIMEOUT(panelOpen(app.window), 20000);
        click(app, NEED("settingsPage-packs"));
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "pack-io.example.keyed"), 20000);
        shot("packs-1-list");
        reveal("pack-io.example.keyed");
        clickHeader(app, "pack-io.example.keyed");
        QTRY_VERIFY_WITH_TIMEOUT(openAndGrown(app.window, "pack-io.example.keyed"), 20000);
        shot("packs-2-open");
        reveal("packAddRow");
        clickHeader(app, "packAddRow");
        QTRY_VERIFY_WITH_TIMEOUT(openAndGrown(app.window, "packAddRow"), 20000);
        shot("packs-3-add");
        QMetaObject::invokeMethod(NEED("packSearch"), "focusField");
        for (const QChar c : QStringLiteral("zzz"))
            QTest::keyClick(app.window, c.toLatin1());
        shot("packs-4-nomatch");
        NEED("packSearch")->findChild<QQuickItem *>("packSearchInput")->setProperty("text", "");
        app.window->resize(520, 760);
        shot("packs-5-narrow");
        reveal("pack-io.example.tools");
        clickHeader(app, "pack-io.example.tools");
        QTRY_VERIFY_WITH_TIMEOUT(openAndGrown(app.window, "pack-io.example.tools"), 20000);
        shot("packs-6-narrow-open");

        app.window->resize(1300, 850);
        click(app, NEED("settingsPage-folders"));
        QTRY_VERIFY_WITH_TIMEOUT(find(app.window, "folderRow-output_folder"), 20000);
        shot("folders-1-list");
        reveal("folderRow-output_folder");
        clickHeader(app, "folderRow-output_folder");
        QTRY_VERIFY_WITH_TIMEOUT(openAndGrown(app.window, "folderRow-output_folder"), 20000);
        shot("folders-2-open");
        reveal("folderRow-model_paths");
        clickHeader(app, "folderRow-model_paths");
        QTRY_VERIFY_WITH_TIMEOUT(openAndGrown(app.window, "folderRow-model_paths"), 20000);
        shot("folders-3-models");
        app.window->resize(520, 760);
        shot("folders-4-narrow");
        reveal("folderRow-download_folder");
        clickHeader(app, "folderRow-download_folder");
        QTRY_VERIFY_WITH_TIMEOUT(openAndGrown(app.window, "folderRow-download_folder"), 20000);
        shot("folders-5-narrow-download");
    }
};

FUGUE_TEST_MAIN(TestPacksPage, QGuiApplication)
#include "test_packs_page.moc"
