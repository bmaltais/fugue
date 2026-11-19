// Editing input values with the widgets in the real window (Main.qml, offscreen), with keys
// and clicks (spec 6.2, 3.2, 2.5, 12.4): every widget of 6.2 and the model picker, one undo
// step per edit, the inline error that keeps a bad value out, a linked input showing its
// source, Tab between fields, and the text-field focus rules that keep typed keys away from
// the commands.

#include <QJsonArray>
#include <QJsonObject>
#include <QQuickStyle>
#include <QUndoStack>

#include "canvas/widget_edit.h"
#include "canvas_app.h"
#include "widget_fixture.h"
#include "../engine/omarchy_fixture.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::load;

namespace {

// The all-widgets node, a Text linked into a Text Concat; zoom 1, the world's origin at the
// window's top left.
const char *fixture = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "all": {"type": "io.example.widgets/all", "version": "1.0.0", "inputs": {"count": 7}},
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "join": {"type": "fugue/text-concat", "version": "1.0.0", "inputs": {"a": {"from": "prompt.text"}}}
  }},
  "layout": {"nodes": {"all": {"pos": [20, 20]}, "prompt": {"pos": [400, 20], "title": "Prompt"},
                       "join": {"pos": [400, 200]}},
             "view": {"center": [480, 340], "zoom": 1}}
})";

struct WidgetApp : App {
    int quits = 0, fits = 0, searches = 0;
    explicit WidgetApp(const char *graph = fixture) : App(load(graph), widget_test::addWidgetType) {
        // Commands whose keys a field must keep: count them instead of acting.
        commands.setHandler("app.quit", [this] { ++quits; });
        commands.setHandler("view.fit", [this] { ++fits; });
        commands.setHandler("node.search", [this] { ++searches; });
        // The fixture's node type is a command node: allow its pack, as the trust question would.
        workflows.allowPendingTrust();
    }

    QQuickItem *widget(const QString &node, const QString &input) const {
        QList<QQuickItem *> todo{window->contentItem()};
        while (!todo.isEmpty()) {
            QQuickItem *at = todo.takeFirst();
            if (at->objectName() == "inputWidget" && at->property("nodeId").toString() == node
                && at->property("name").toString() == input)
                return at;
            todo << at->childItems();
        }
        return nullptr;
    }
    // A point in a widget, `share` of the way across, in window coordinates.
    QPoint in(const QString &node, const QString &input, qreal share = 0.5) const {
        QQuickItem *w = widget(node, input);
        if (!w)
            qFatal("no widget for %s.%s", qPrintable(node), qPrintable(input));
        return w->mapToScene(QPointF(w->width() * share, w->height() / 2)).toPoint();
    }
    QPoint atEnd(const QString &node, const QString &input) const {
        QQuickItem *w = widget(node, input);
        return w->mapToScene(QPointF(w->width() - 4, w->height() / 2)).toPoint();
    }
    void click(QPoint at) { QTest::mouseClick(window, Qt::LeftButton, {}, at); }
    void type(const QString &text) {
        for (const QChar c : text)
            QTest::keyClick(window, c.toLatin1());
    }

    QJsonValue stored(const QString &node, const QString &input) const {
        return model.document().graph.nodes.value(node).inputs.value(input).toJson();
    }
    int steps() const { return model.undoStack()->count(); }
    QQuickItem *focused() const { return window->activeFocusItem(); }
    bool typing() const { return commands.textFieldFocus(); }
    // A widget's menu or picker: they belong to the widget's pickers (WidgetPicker.qml).
    QObject *popup(const char *name) const {
        QList<QQuickItem *> todo{window->contentItem()};
        while (!todo.isEmpty()) {
            QQuickItem *at = todo.takeFirst();
            if (at->objectName() == "inputWidget")
                if (auto *pickers = at->property("pickers").value<QObject *>())
                    if (QObject *found = pickers->findChild<QObject *>(name))
                        return found;
            todo << at->childItems();
        }
        return nullptr;
    }
    bool menuOpen() const {
        QObject *menu = popup("widgetMenu");
        return menu && menu->property("opened").toBool();
    }
    QQuickItem *error() const { return canvas_test::item(window->contentItem(), "widgetError"); }
};

} // namespace

class TestCanvasWidgets : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aSpinTakesTypedNumbersAndKeepsTheKeys() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.in("all", "count"));
        QTRY_VERIFY(app.typing());
        // The field starts with the value, selected: typing replaces it.
        QCOMPARE(app.focused()->property("text").toString(), QString("7"));
        app.type("12");
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "count"), QJsonValue(12));
        QCOMPARE(app.steps(), 1);
        QTRY_VERIFY(!app.typing());

        // Up and Down step; the edit is one step however many keys it took.
        app.click(app.in("all", "count"));
        QTRY_VERIFY(app.typing());
        app.key(Qt::Key_Up);
        app.key(Qt::Key_Up);
        app.key(Qt::Key_Down);
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "count"), QJsonValue(13));
        QCOMPARE(app.steps(), 2);

        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.stored("all", "count"), QJsonValue(12));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aBadValueShowsWhyAndIsNotWritten() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.in("all", "count"));
        QTRY_VERIFY(app.typing());
        app.type("101");
        QTRY_VERIFY(app.error() && app.error()->isVisible());
        QVERIFY(app.error()->property("visible").toBool());
        app.key(Qt::Key_Return); // refused: the field stays
        QVERIFY(app.typing());
        // 1 and 10 were valid as they were typed; 101 is not, so 10 stays.
        QCOMPARE(app.stored("all", "count"), QJsonValue(10));
        app.key(Qt::Key_Backspace);
        QCOMPARE(app.stored("all", "count"), QJsonValue(10));
        QTRY_VERIFY(!app.error()->isVisible());
        // Escape puts the value back and leaves no step.
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.typing());
        QCOMPARE(app.stored("all", "count"), QJsonValue(7));
        QCOMPARE(app.steps(), 0);
        QVERIFY(!app.error() || !app.error()->isVisible());
    }

    void spinChevronsStepAndADragScrubs() {
        WidgetApp app;
        QVERIFY(app.open());
        QTest::mouseMove(app.window, app.in("all", "count"));
        app.click(app.in("all", "count", 0.02));
        QCOMPARE(app.stored("all", "count"), QJsonValue(6));
        app.click(app.atEnd("all", "count"));
        app.click(app.atEnd("all", "count"));
        QCOMPARE(app.stored("all", "count"), QJsonValue(8));
        QCOMPARE(app.steps(), 3);
        QVERIFY(!app.typing());
        // A drag of 60 units to the right is ten steps, one undo step in all.
        const QPoint from = app.in("all", "count");
        app.drag(Qt::LeftButton, from, from + QPoint(60, 0));
        QCOMPARE(app.stored("all", "count"), QJsonValue(18));
        QCOMPARE(app.steps(), 4);
        QCOMPARE(app.model.position("all"), QPointF(20, 20)); // the node did not move
    }

    void aSliderFollowsADragAndTakesTypedValues() {
        WidgetApp app;
        QVERIFY(app.open());
        app.drag(Qt::LeftButton, app.in("all", "amount", 0.1), app.in("all", "amount", 0.8));
        QCOMPARE(app.stored("all", "amount"), QJsonValue(0.8));
        QCOMPARE(app.steps(), 1);
        // A click with no drag types. "." would fit the view, and does not here.
        app.click(app.in("all", "amount"));
        QTRY_VERIFY(app.typing());
        const QPointF pan = app.pan();
        app.type("0.35");
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "amount"), QJsonValue(0.35));
        QCOMPARE(app.fits, 0);
        QCOMPARE(app.pan(), pan);
    }

    void aLineFieldOwnsBareKeysAndBackspace() {
        WidgetApp app;
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(60, 30)); // select the node
        QCOMPARE(app.model.selectedIds(), QStringList{"all"});
        app.click(app.in("all", "name"));
        QTRY_VERIFY(app.typing());
        app.type("quilt.");
        app.key(Qt::Key_Backspace);
        app.key(Qt::Key_Tab); // to the next field, not node search
        QCOMPARE(app.stored("all", "name"), QJsonValue("quilt"));
        QCOMPARE(app.quits, 0);
        QCOMPARE(app.fits, 0);
        QCOMPARE(app.searches, 0);
        QVERIFY(app.model.document().graph.nodes.contains("all")); // Backspace did not delete it
        // Tab went to the next typed field (notes), Shift+Tab comes back.
        QTRY_COMPARE(app.focused()->objectName(), QString("widgetArea"));
        app.key(Qt::Key_Tab, Qt::ShiftModifier);
        QTRY_COMPARE(app.focused()->objectName(), QString("widgetInput"));
        QCOMPARE(app.focused()->property("text").toString(), QString("quilt"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.typing());
        QCOMPARE(app.steps(), 1);
        // Without a field, the same keys are commands again.
        app.key(Qt::Key_Period);
        QCOMPARE(app.fits, 1);
    }

    // A wheel over an open multiline field scrolls its text while there is more to read;
    // anywhere else, or with nothing to scroll, it is the canvas's (zoom, in the legacy preset).
    void aWheelOverAnOpenMultilineFieldScrollsItOtherwiseZooms() {
        WidgetApp app;
        QVERIFY(app.open());
        auto wheelAt = [&](QPoint at) {
            QWheelEvent event(at, app.window->mapToGlobal(at), {}, QPoint(0, -120), Qt::NoButton, {},
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(app.window, &event);
        };
        app.click(app.in("all", "notes"));
        QTRY_COMPARE(app.focused()->objectName(), QString("widgetArea"));
        QQuickItem *flick = app.focused()->parentItem()->parentItem();
        // Short text: nothing to scroll, so the wheel zooms the canvas.
        app.type("one");
        wheelAt(app.focused()->mapToScene(QPointF(20, 10)).toPoint());
        QVERIFY2(app.zoom() < 1.0, qPrintable(QString::number(app.zoom())));
        const qreal zoom = app.zoom();
        // Long text: the wheel scrolls the field and the zoom stays.
        for (int i = 0; i < 40; ++i)
            app.key(Qt::Key_Return);
        QTRY_VERIFY(flick->property("contentHeight").toReal() > flick->height());
        flick->setProperty("contentY", 0);
        wheelAt(app.focused()->mapToScene(QPointF(20, 10)).toPoint());
        QTRY_VERIFY2(flick->property("contentY").toReal() > 0, "the field did not scroll");
        QCOMPARE(app.zoom(), zoom);
    }

    void aMultilineFieldTakesNewLinesUntilFocusLeaves() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.in("all", "notes"));
        QTRY_COMPARE(app.focused()->objectName(), QString("widgetArea"));
        QVERIFY(app.typing());
        app.type("a fox");
        app.key(Qt::Key_Return);
        app.type("in snow");
        QCOMPARE(app.stored("all", "notes"), QJsonValue("a fox\nin snow")); // in the document as typed
        app.click(app.screen(700, 600)); // empty canvas
        QTRY_VERIFY(!app.typing());
        QCOMPARE(app.stored("all", "notes"), QJsonValue("a fox\nin snow"));
        QCOMPARE(app.steps(), 1);
        // The face shows the first line.
        QTRY_COMPARE(canvas_test::item(app.widget("all", "notes"), "widgetValue")->property("text").toString(),
                     QString("a fox"));
        // Escape keeps the text too: a prompt is never lost to a key.
        app.click(app.in("all", "notes"));
        QTRY_VERIFY(app.typing());
        app.key(Qt::Key_End, Qt::ControlModifier);
        app.type("!");
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.typing());
        QCOMPARE(app.stored("all", "notes"), QJsonValue("a fox\nin snow!"));
        QCOMPARE(app.steps(), 2);
    }

    void aToggleFlipsOnAClick() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.in("all", "flag"));
        QCOMPARE(app.stored("all", "flag"), QJsonValue(true));
        QTRY_VERIFY(canvas_test::item(app.widget("all", "flag"), "widgetToggle"));
        app.click(app.in("all", "flag"));
        QCOMPARE(app.stored("all", "flag"), QJsonValue(false));
        QCOMPARE(app.steps(), 2);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.stored("all", "flag"), QJsonValue(true));
    }

    void aChoiceOpensItsMenu() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.in("all", "mode"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        // The window's keys sleep while the menu is open: Down and Enter choose.
        app.key(Qt::Key_Down);
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "mode"), QJsonValue("b"));
        QTRY_VERIFY(!app.popup("widgetMenu"));
        app.click(app.in("all", "mode"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.popup("widgetMenu"));
        QCOMPARE(app.stored("all", "mode"), QJsonValue("b"));
        QCOMPARE(app.steps(), 1);
    }

    void fileAndFolderTakeTypedPathsAndThePicker() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.in("all", "file"));
        QTRY_VERIFY(app.typing());
        app.type("/tmp/fox.png");
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "file"), QJsonValue("/tmp/fox.png"));

        app.click(app.atEnd("all", "file"));
        QObject *files = nullptr;
        QTRY_VERIFY((files = app.popup("widgetFileDialog")));
        const QString owl = app.dir.path() + "/owl.png";
        QFile file(owl);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        files->setProperty("selectedFile", QUrl::fromLocalFile(owl));
        QMetaObject::invokeMethod(files, "accepted"); // as the portal answers
        QVERIFY(QMetaObject::invokeMethod(files, "close"));
        QCOMPARE(app.stored("all", "file"), QJsonValue(owl));

        app.click(app.atEnd("all", "folder"));
        QObject *folders = nullptr;
        QTRY_VERIFY((folders = app.popup("widgetFolderDialog")));
        folders->setProperty("selectedFolder", QUrl::fromLocalFile(app.dir.path()));
        QMetaObject::invokeMethod(folders, "accepted");
        QVERIFY(QMetaObject::invokeMethod(folders, "close"));
        QCOMPARE(app.stored("all", "folder"), QJsonValue(app.dir.path()));
        QCOMPARE(app.steps(), 3);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aColorTakesHexAndThePicker() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.in("all", "tint"));
        QTRY_VERIFY(app.typing());
        app.type("teal");
        QTRY_VERIFY(app.error() && app.error()->isVisible());
        app.key(Qt::Key_A, Qt::ControlModifier);
        app.type("#00ff0080");
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "tint"), QJsonValue("#00ff0080"));
        QCOMPARE(app.steps(), 1);

        app.click(app.atEnd("all", "tint"));
        QObject *colors = nullptr;
        QTRY_VERIFY((colors = app.popup("widgetColorDialog")));
        colors->setProperty("selectedColor", QColor(255, 0, 0));
        QMetaObject::invokeMethod(colors, "accepted");
        QCOMPARE(app.stored("all", "tint"), QJsonValue("#ff0000"));
        QCOMPARE(app.steps(), 2);
    }

    void aSeedIsTypedOrSetToRandomizeOrIncrement() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.atEnd("all", "seed"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        app.key(Qt::Key_Down); // from Fixed to Randomize each run
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "seed"), QJsonValue(QJsonObject{{"auto", "random"}}));
        // The value, then the dice glyph (an icon font glyph of its own, docs/canvas-look.md 16).
        QTRY_COMPARE(canvas_test::item(app.widget("all", "seed"), "widgetValue")->property("text").toString(),
                     QString("random"));
        QCOMPARE(app.widget("all", "seed")->property("glyph").toString(), QString("dices"));

        app.click(app.atEnd("all", "seed"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        app.key(Qt::Key_Down); // Increment after each run
        app.key(Qt::Key_Return);
        QCOMPARE(app.model.widgets()->seedMode("all", "seed"), QString("increment"));
        QVERIFY(app.stored("all", "seed").isDouble());

        app.click(app.in("all", "seed"));
        QTRY_VERIFY(app.typing());
        app.type("1234");
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "seed"), QJsonValue(1234));
        app.model.widgets()->advanceSeeds(); // after a run starts (#19)
        QCOMPARE(app.stored("all", "seed"), QJsonValue(1235));
        QCOMPARE(app.steps(), 4);
    }

    void theModelPickerListsModelsAndTakesAName() {
        WidgetApp app;
        const QJsonObject zimage{{"resource", "fugue/image-model"}, {"name", "z-image-turbo"}};
        app.model.widgets()->setModelSource([&](const QString &, const QStringList &,
                                                std::function<void(QList<WidgetEdit::Model>)> done) {
            done({{"z-image-turbo", zimage, "ready", "This machine", "Z-Image Turbo", ""},
                  {"flux2-klein", QJsonObject{{"resource", "fugue/image-model"}, {"name", "flux2-klein"}}, "loading",
                   "gx10", "", "flux2-klein is starting. Wait for it to finish loading."}});
        });
        QVERIFY(app.open());
        QCOMPARE(canvas_test::item(app.widget("all", "model"), "widgetValue")->property("text").toString(),
                 QString("Any ready model"));
        QCOMPARE(app.widget("all", "model")->property("glyph").toString(), QString("chevron-down"));
        app.click(app.in("all", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        // Any ready model, each instance's name over its model, Type a name.
        QCOMPARE(app.popup("widgetMenu")->property("entries").toList().size(), 6);
        app.key(Qt::Key_Down); // the heading cannot be chosen: straight to z-image-turbo
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "model"), QJsonValue(zimage));

        app.click(app.in("all", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        app.key(Qt::Key_Down); // from z-image-turbo, past gx10's heading, to flux2-klein
        app.key(Qt::Key_Down);
        app.key(Qt::Key_Return); // Type a name…
        QTRY_VERIFY(app.typing());
        QCOMPARE(app.focused()->property("text").toString(), QString("z-image-turbo"));
        app.type("my-model");
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("all", "model"),
                 QJsonValue(QJsonObject{{"resource", "fugue/image-model"}, {"name", "my-model"}}));

        app.click(app.in("all", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        app.key(Qt::Key_Return); // Any ready model: nothing listed is chosen, so it is first
        QCOMPARE(app.stored("all", "model"), QJsonValue(QJsonValue::Undefined));
        QCOMPARE(app.steps(), 3);
    }

    void onFirstRunThePickerListsThePairedHostsModelsByHost() {
        // No providers.json, this machine with only text models, studio paired and running
        // two image models, lab paired and offline (spec 8.2's default): the real model
        // source, in the real window.
        test::PairedHome home;
        WidgetApp app(R"({"fugue": 1,
          "graph": {"nodes": {"generate": {"type": "fugue/generate-image", "version": "1.0.0",
                                           "inputs": {"prompt": "a lighthouse"}}}},
          "layout": {"nodes": {"generate": {"pos": [20, 20]}}, "view": {"center": [480, 340], "zoom": 1}}})");
        QVERIFY(app.open());
        app.click(app.in("generate", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QObject *menu = app.popup("widgetMenu");
        QTRY_VERIFY(menu->property("entries").toList().size() == 7);
        QStringList rows;
        for (const QVariant &entry : menu->property("entries").toList()) {
            const QVariantMap e = entry.toMap();
            rows << (e.value("heading").toBool() ? "# " : "") + e.value("text").toString()
                        + (e.value("detail").toString().isEmpty() ? QString() : " | " + e.value("detail").toString());
        }
        QCOMPARE(rows, (QStringList{"Any ready model", "# lab", "Z-Image Turbo | not running", "# studio",
                                    "FLUX.2 Klein 4B", "Z-Image Turbo", "Type a name…"}));
        // What a model that is not ready needs, under its name.
        QVERIFY(menu->property("entries").toList().at(2).toMap().value("note").toString().contains("Turn on lab"));
        bool noteShown = false;
        QList<QQuickItem *> todo{menu->property("contentItem").value<QQuickItem *>()};
        while (!todo.isEmpty()) {
            QQuickItem *at = todo.takeFirst();
            if (at->objectName() == "widgetMenuNote" && at->isVisible() && at->height() > 0
                && at->property("text").toString().contains("Turn on lab"))
                noteShown = true;
            todo << at->childItems();
        }
        QVERIFY(noteShown);
        // Down skips the headings: lab's model, then studio's first.
        app.key(Qt::Key_Down);
        app.key(Qt::Key_Down);
        app.key(Qt::Key_Return);
        QCOMPARE(app.stored("generate", "model"),
                 QJsonValue(QJsonObject{{"resource", "fugue/image-model"}, {"name", "flux2-klein-4b-sdcpp-cuda"}}));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void withNoModelThePickerSaysHowToGetOne() {
        WidgetApp app(R"({"fugue": 1,
          "graph": {"nodes": {"edit": {"type": "fugue/edit-image", "version": "1.0.0", "inputs": {"instruction": "x"}}}},
          "layout": {"nodes": {"edit": {"pos": [20, 20]}}, "view": {"center": [480, 340], "zoom": 1}}})");
        app.model.widgets()->setModelSource([](const QString &, const QStringList &capabilities,
                                               std::function<void(QList<WidgetEdit::Model>)> done) {
            QCOMPARE(capabilities, QStringList{"image.edit"});
            done({});
        });
        QVERIFY(app.open());
        app.click(app.in("edit", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QStringList rows;
        for (const QVariant &entry : app.popup("widgetMenu")->property("entries").toList())
            rows << entry.toMap().value("text").toString();
        QCOMPARE(rows, (QStringList{"Any ready model",
                                    "No image model available: start one in omarchy-local-ai or add a provider",
                                    "Type a name…"}));
    }

    void aLinkedInputShowsItsSourceAndTakesNoTyping() {
        WidgetApp app;
        QVERIFY(app.open());
        QQuickItem *linked = app.widget("join", "a");
        QVERIFY(linked);
        QCOMPARE(canvas_test::item(linked, "widgetValue")->property("text").toString(), QString("Prompt · Text"));
        // A click there is a click on the node.
        app.click(app.in("join", "a"));
        QVERIFY(!app.typing());
        QCOMPARE(app.model.selectedIds(), QStringList{"join"});
        // Once the link is out, the widget is back.
        app.model.disconnectInput("join", "a");
        QTRY_COMPARE(canvas_test::item(app.widget("join", "a"), "widgetValue")->property("text").toString(),
                     QString(""));
        app.click(app.in("join", "a"));
        QTRY_VERIFY(app.typing());
    }

    void collapsingTheNodeEndsTheEdit() {
        WidgetApp app;
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(60, 30)); // select the node
        app.click(app.in("all", "name"));
        QTRY_VERIFY(app.typing());
        app.type("owl");
        app.key(Qt::Key_C, Qt::AltModifier); // node.collapse is not a field's key
        QTRY_VERIFY(!app.typing());
        QCOMPARE(app.stored("all", "name"), QJsonValue("owl"));
        // The typing and the collapse are a step each.
        QCOMPARE(app.steps(), 2);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.stored("all", "name"), QJsonValue("owl"));
        QVERIFY(!app.model.document().layout.value("nodes").toObject().value("all").toObject().value("collapsed").toBool());
        // Bypass while typing (Ctrl+B is not a field's key): again a step each.
        app.click(app.in("all", "name"));
        QTRY_VERIFY(app.typing());
        app.key(Qt::Key_End);
        app.type("s");
        app.key(Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(!app.typing());
        QCOMPARE(app.steps(), 3);
        QCOMPARE(app.stored("all", "name"), QJsonValue("owls"));
        QCOMPARE(app.model.document().graph.nodes.value("all").mode, NodeMode::Bypass);
    }

    void anotherTabEndsTheEditWithoutWritingIntoIt() {
        WidgetApp app;
        QVERIFY(app.open());
        app.click(app.in("prompt", "text"));
        QTRY_VERIFY(app.typing());
        app.key(Qt::Key_End, Qt::ControlModifier);
        app.type(" in snow");
        app.key(Qt::Key_N, Qt::ControlModifier); // a new tab: the field's edit ends first
        QCOMPARE(app.workflows.count(), 2);
        QTRY_VERIFY(!app.typing());
        QVERIFY(app.model.document().graph.nodes.isEmpty());
        app.workflows.setCurrent(0);
        QCOMPARE(app.stored("prompt", "text"), QJsonValue("a red fox in snow"));
        QCOMPARE(app.steps(), 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestCanvasWidgets)
#include "test_canvas_widgets.moc"
