// Node search in the real window (Main.qml, offscreen): the double-click, Tab and Shift+A
// open it, a link let go on empty canvas opens it filtered, the keyboard picks a result,
// and Escape leaves the graph alone (spec 12.8, 12.4).

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "search/node_search.h"
#include "theme.h"
#include "translator.h"
#include "workflow/workflows.h"

using namespace fugue;

namespace {

QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

void typeText(QWindow *window, const QString &text) {
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

const char *small = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [20, 100]}, "save": {"pos": [660, 300]}},
             "view": {"center": [480, 340], "zoom": 1}}
})";

struct App {
    QTemporaryDir dir;
    Translator translator{dir.path() + "/language"};
    Theme theme{dir.path()};
    Backend backend;
    Registry registry;
    CanvasModel model{&registry};
    CommandRegistry commands;
    fugue::Workflows workflows{&model, &registry, &commands, {dir.path() + "/recovery", dir.path() + "/trust.json"}};
    QQmlApplicationEngine engine;
    QStringList warnings;
    QQuickWindow *window = nullptr;
    QQuickItem *canvas = nullptr;
    QQuickItem *world = nullptr;

    App() {
        nodes::addBuiltinNodeTypes(registry);
        registerDefaultCommands(commands);
        commands.setContextActive("canvas", true);
        model.search()->registerCommands(commands);
        const Result<Document> loaded = Document::load(small);
        if (!loaded.ok())
            qFatal("fixture does not load");
        model.setDocument(loaded.value());
        QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("canvasModel", &model);
        engine.rootContext()->setContextProperty("commands", &commands);
        engine.rootContext()->setContextProperty("workflows", &workflows);
    }

    bool open() {
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().isEmpty())
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!QTest::qWaitForWindowExposed(window))
            return false;
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window))
            return false;
        canvas = item(window->contentItem(), "canvas");
        world = item(window->contentItem(), "world");
        return canvas && world;
    }

    // A world point in window coordinates.
    QPoint screen(qreal x, qreal y) const {
        return canvas->mapToScene(QPointF(world->x() + x * world->scale(), world->y() + y * world->scale())).toPoint();
    }
    NodeSearch *search() { return model.search(); }
    // The popup and its parts are children of the window object tree, not of its content.
    QObject *part(const QString &name) { return engine.rootObjects().first()->findChild<QObject *>(name); }
    // An item inside the result list (its delegates are visual children of the list).
    QQuickItem *inResults(const QString &name) {
        auto *list = qobject_cast<QQuickItem *>(part("searchResults"));
        return list ? item(list, name) : nullptr;
    }
    bool popupOpen() { return part("nodeSearch")->property("opened").toBool(); }
    bool fieldFocused() {
        QObject *field = part("searchField");
        return field && field->property("activeFocus").toBool();
    }
};

} // namespace

class TestNodeSearchWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aDoubleClickOnEmptyCanvasOpensSearchWithTheQueryFocused() {
        App app;
        QVERIFY(app.open());
        QVERIFY(!app.popupOpen());
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 600));
        QTRY_VERIFY(app.popupOpen());
        QVERIFY(app.search()->active());
        QCOMPARE(app.search()->point(), QPointF(300, 600));
        QTRY_VERIFY(app.fieldFocused());
        QVERIFY(app.part("searchResults")->property("count").toInt() > 5);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aDoubleClickOnANodeDoesNotOpenIt() {
        App app;
        QVERIFY(app.open());
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(100, 130));
        QTest::qWait(100);
        QVERIFY(!app.search()->active());
    }

    void typingFiltersAndEnterAddsTheNodeAtThePoint() {
        App app;
        QVERIFY(app.open());
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 600));
        QTRY_VERIFY(app.fieldFocused());
        typeText(app.window, "crop");
        QTRY_VERIFY(app.search()->count() >= 1);
        const int before = app.model.nodes()->rowCount();
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!app.popupOpen());
        QCOMPARE(app.model.nodes()->rowCount(), before + 1);
        const QString id = app.model.selectedIds().value(0);
        QCOMPARE(app.model.position(id), QPointF(300, 600));
        QCOMPARE(app.model.nodes()->data(app.model.nodes()->index(before), NodeList::TypeId).toString(),
                 QString("fugue/crop"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void downArrowMovesTheChoice() {
        App app;
        QVERIFY(app.open());
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 600));
        QTRY_VERIFY(app.fieldFocused());
        typeText(app.window, "image");
        QTRY_VERIFY(app.search()->count() >= 3);
        const QString second =
            app.search()->results()->data(app.search()->results()->index(1), SearchResults::TypeId).toString();
        QTest::keyClick(app.window, Qt::Key_Down);
        QTRY_COMPARE(app.part("searchResults")->property("currentIndex").toInt(), 1);
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!app.popupOpen());
        const int last = app.model.nodes()->rowCount() - 1;
        QCOMPARE(app.model.nodes()->data(app.model.nodes()->index(last), NodeList::TypeId).toString(), second);
    }

    void escapeClosesAndAddsNothing() {
        App app;
        QVERIFY(app.open());
        const int before = app.model.nodes()->rowCount();
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 600));
        QTRY_VERIFY(app.popupOpen());
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!app.popupOpen());
        QTRY_VERIFY(!app.search()->active());
        QCOMPARE(app.model.nodes()->rowCount(), before);
        // And it opens again.
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 620));
        QTRY_VERIFY(app.popupOpen());
    }

    void tabOpensSearchAtThePointer() {
        App app;
        QVERIFY(app.open());
        QTest::mouseMove(app.window, app.screen(250, 560));
        QTest::keyClick(app.window, Qt::Key_Tab);
        QTRY_VERIFY(app.popupOpen());
        QVERIFY(qAbs(app.search()->point().x() - 250) < 2);
        QVERIFY(qAbs(app.search()->point().y() - 560) < 2);
    }

    void shiftAOpensSearchAtThePointer() {
        App app;
        QVERIFY(app.open());
        QTest::mouseMove(app.window, app.screen(400, 520));
        QTest::keyClick(app.window, Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(app.popupOpen());
        QVERIFY(qAbs(app.search()->point().x() - 400) < 2);
    }

    void typingInTheSearchBoxDoesNotRunShortcuts() {
        // `q` quits, `n` and `l` are bare-letter commands; in the box they are letters.
        App app;
        QVERIFY(app.open());
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 600));
        QTRY_VERIFY(app.fieldFocused());
        typeText(app.window, "qnl");
        QCOMPARE(app.part("searchField")->property("text").toString(), QString("qnl"));
    }

    void aLinkLetGoOnEmptyCanvasOpensFilteredSearchAndAcceptingLinksIt() {
        App app;
        QVERIFY(app.open());
        // The prompt's text output (260, 144: 20 + 240 wide, 100 + 28 + 4 + 12) dragged to empty canvas.
        const QPoint from = app.screen(260, 144), to = app.screen(300, 600);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(app.window, from + (to - from) * i / 8);
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, to);
        QTRY_VERIFY(app.popupOpen());
        QCOMPARE(app.search()->link().value("node").toString(), QString("prompt"));
        QVERIFY(app.part("searchFilter")->property("visible").toBool());
        // Nodes with no text input are left out.
        QTRY_VERIFY(app.fieldFocused());
        typeText(app.window, "brightness");
        QTRY_COMPARE(app.search()->count(), 0);
        QTRY_VERIFY(app.part("noResults")->property("visible").toBool());
        for (int i = 0; i < 10; ++i)
            QTest::keyClick(app.window, Qt::Key_Backspace);
        typeText(app.window, "concat");
        QTRY_COMPARE(app.search()->count(), 1);
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_VERIFY(!app.popupOpen());
        QCOMPARE(app.model.links()->rowCount(), 1);
        QCOMPARE(app.model.links()->data(app.model.links()->index(0), LinkList::FromNode).toString(),
                 QString("prompt"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aResultShowsCategoryPortsAndVersion() {
        App app;
        QVERIFY(app.open());
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 600));
        QTRY_VERIFY(app.fieldFocused());
        typeText(app.window, "resize");
        QTRY_VERIFY(app.search()->count() >= 1);
        QTRY_VERIFY(app.inResults("resultCategory"));
        QCOMPARE(app.inResults("resultCategory")->property("text").toString(), QString("Image/Transform"));
        QCOMPARE(app.inResults("resultVersion")->property("text").toString(), QString("v1.0.0"));
        auto *ports = app.inResults("resultPorts");
        QVERIFY(ports);
        QVERIFY(ports->childItems().size() >= 4); // the image and size inputs, an output
    }

    void clickingAResultAddsIt() {
        App app;
        QVERIFY(app.open());
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 600));
        QTRY_VERIFY(app.fieldFocused());
        typeText(app.window, "blend");
        QTRY_COMPARE(app.search()->count(), 1);
        QTRY_VERIFY(app.inResults("searchResult"));
        auto *row = app.inResults("searchResult");
        const int before = app.model.nodes()->rowCount();
        const QPointF at = row->mapToScene(QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, at.toPoint());
        QTRY_COMPARE(app.model.nodes()->rowCount(), before + 1);
    }
};

QTEST_MAIN(TestNodeSearchWindow)
#include "test_node_search_window.moc"
