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
#include "engine/hash.h"
#include "engine/model_catalog.h"
#include "canvas_app.h"
#include "widget_fixture.h"
#include "../engine/fake_http.h"
#include "../engine/omarchy_fixture.h"
#include "../scroll_checks.h"
#include "../window_grab.h"
#include "../test_shard.h"

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

    // While a run is going Escape cancels it (12.4), but not from a field being typed into:
    // there Escape puts the value back (spin, line) or ends the edit (multiline).
    void escapeInAFieldDuringARunIsTheFields() {
        WidgetApp app;
        int cancels = 0;
        app.commands.setHandler("run.cancel", [&cancels] { ++cancels; });
        app.commands.setContextActive("run", true);
        QVERIFY(app.open());
        app.click(app.in("all", "count"));
        QTRY_VERIFY(app.typing());
        app.type("12");
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.typing());
        QCOMPARE(app.stored("all", "count"), QJsonValue(7));
        QCOMPARE(cancels, 0);

        app.click(app.in("all", "notes"));
        QTRY_VERIFY(app.typing());
        app.type("fox");
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.typing());
        QCOMPARE(app.stored("all", "notes"), QJsonValue("fox"));
        QCOMPARE(cancels, 0);

        // On the canvas Escape is the run's again.
        app.key(Qt::Key_Escape);
        QCOMPARE(cancels, 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
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

    void aChoiceMenuNearTheWindowsBottomOpensAboveAndStaysInside() {
        WidgetApp app;
        QVERIFY(app.open());
        QQuickItem *field = app.widget("all", "mode");
        QVERIFY(field);
        const int bottom = field->mapToScene(QPointF(0, field->height())).toPoint().y();
        app.window->resize(app.window->width(), bottom + 20);
        QTRY_COMPARE(app.window->height(), bottom + 20);
        QTRY_COMPARE(app.window->contentItem()->height(), qreal(bottom + 20)); // the popups' layer too
        app.click(app.in("all", "mode"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QObject *menu = app.popup("widgetMenu");
        auto *parent = menu->property("parent").value<QQuickItem *>();
        const QPointF at = parent->mapToScene(QPointF(menu->property("x").toReal(), menu->property("y").toReal()));
        const QRectF rect(at, QSizeF(menu->property("width").toReal(), menu->property("height").toReal()));
        QVERIFY2(QRectF(0, 0, app.window->width(), app.window->height()).contains(rect),
                 qPrintable(QString("%1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())));
        QVERIFY2(rect.bottom() <= field->mapToScene(QPointF(0, 0)).y() + 1, qPrintable(QString("%1,%2 %3x%4 window %5").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()).arg(app.window->height()))); // above the field
        // Right above it and aligned to it, not pushed into a corner.
        QVERIFY2(rect.bottom() >= field->mapToScene(QPointF(0, 0)).y() - 1
                     && qAbs(rect.left() - field->mapToScene(QPointF(0, 0)).x()) <= 1,
                 qPrintable(QString("%1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.popup("widgetMenu"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
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

    // A narrow node at half zoom: its choice list is sized for its entries, not for the field, and drawn at screen scale,
    // so every entry reads whole (owner: "it is impossible to read the items because the text is cut out").
    void aNarrowNodesChoiceListFitsItsEntriesAtScreenScale() {
        WidgetApp app(R"({"fugue": 1,
          "graph": {"nodes": {"pick": {"type": "io.example.widgets/long", "version": "1.0.0"}}},
          "layout": {"nodes": {"pick": {"pos": [40, 40], "size": [120, 0]}}, "view": {"center": [480, 340], "zoom": 0.5}}})");
        QVERIFY(app.open());
        QQuickItem *field = app.widget("pick", "sampler");
        QVERIFY(field);
        const qreal fieldOnScreen = field->mapToScene(QPointF(field->width(), 0)).x() - field->mapToScene(QPointF(0, 0)).x();
        app.click(app.in("pick", "sampler"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QObject *menu = app.popup("widgetMenu");
        QQuickItem *list = canvas_test::item(app.window->contentItem(), "widgetMenuList");
        QVERIFY(list);
        app.window->grabWindow(); // lays the rows out
        // Not bound to the field: much wider than it, and at scale 1 in the window whatever the canvas zoom.
        const qreal width = menu->property("width").toReal();
        QVERIFY2(width > 2 * fieldOnScreen, qPrintable(QString("menu %1, field %2").arg(width).arg(fieldOnScreen)));
        QCOMPARE(list->mapToScene(QPointF(100, 0)).x() - list->mapToScene(QPointF(0, 0)).x(), 100.0);
        int rows = 0;
        QList<QQuickItem *> todo{list};
        while (!todo.isEmpty()) {
            QQuickItem *at = todo.takeFirst();
            if (at->objectName() == "widgetMenuText" && at->isVisible()) {
                ++rows;
                QVERIFY2(!at->property("truncated").toBool(), qPrintable(at->property("text").toString()));
            }
            todo << at->childItems();
        }
        QCOMPARE(rows, 3);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.popup("widgetMenu"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // FUGUE_SHOT_DIR=<folder>: a narrow node's model list open, at 100% and 50% zoom. Pictures only; nothing is asserted.
    void aNarrowNodesModelListLook() {
        const QString out = qEnvironmentVariable("FUGUE_SHOT_DIR");
        if (out.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to take the pictures");
        QDir().mkpath(out);
        QTemporaryDir models;
        for (const char *name : {"flux1-dev-kontext-Q4_K_M.gguf", "flux1-dev-kontext-Q8_0.gguf",
                                 "qwen-image-edit-2509-fp8_e4m3fn_scaled.safetensors", "hidream_i1_full_fp16.safetensors",
                                 "wan2.2_i2v_low_noise_14B_Q4_K_S.gguf"})
            QVERIFY(test::writeFixtureFile(models.filePath(QString("image-model/") + name), QByteArray(1000, 'm')));
        for (const char *zoom : {"1", "0.5"}) {
            const QByteArray graph = QByteArray(R"({"fugue": 1,
              "graph": {"nodes": {"all": {"type": "io.example.widgets/all", "version": "1.0.0",
                "inputs": {"model": {"resource": "fugue/image-model", "name": "qwen-image-edit-2509-fp8_e4m3fn_scaled.safetensors"}}}}},
              "layout": {"nodes": {"all": {"pos": [40, 40], "size": [170, 0]}}, "view": {"center": [480, 340], "zoom": )")
                                     + zoom + "}}}";
            WidgetApp app(graph.constData());
            app.model.widgets()->setModelRoots({models.path()});
            app.model.widgets()->setModelSource([](const QString &, const QStringList &,
                                                   std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
            QVERIFY(app.open());
            app.click(app.in("all", "model"));
            QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
            QTest::qWait(150);
            QVERIFY(test_support::grab(app.window).save(QString("%1/narrow-model-list-%2.png").arg(out, zoom)));
        }
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
            done({widget_test::served("z-image-turbo", zimage, "ready", "This machine", "Z-Image Turbo"),
                  widget_test::served("flux2-klein", QJsonObject{{"resource", "fugue/image-model"}, {"name", "flux2-klein"}},
                                      "loading", "gx10", "", "flux2-klein is starting. Wait for it to finish loading.")});
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

    // #164: each model row says where the model lives: a glyph for this machine (a file, a
    // server here) or a paired host, its tooltip naming the host; a host that does not answer
    // keeps its models, dimmed, "host offline".
    void theModelPickerRowsSayWhereTheModelsLive() {
        QTemporaryDir models;
        QVERIFY(test::writeFixtureFile(models.filePath("image-model/turbo.safetensors"), QByteArray(2000, 'm')));
        WidgetApp app;
        app.model.widgets()->setModelRoots({models.path()});
        const auto handle = [](const QString &name) { return QJsonObject{{"resource", "fugue/image-model"}, {"name", name}}; };
        app.model.widgets()->setModelSource([&](const QString &, const QStringList &,
                                                std::function<void(QList<WidgetEdit::Model>)> done) {
            done({widget_test::served("here", handle("here"), "ready", "This machine"),
                  widget_test::served("shared", handle("shared"), "ready", "gx10", "", "", "gx10"),
                  widget_test::served("dark", handle("dark"), "unavailable", "spark", "", "spark is not answering.", "spark")});
        });
        QVERIFY(app.open());
        app.click(app.in("all", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QMap<QString, QVariantMap> rows;
        for (const QVariant &entry : app.popup("widgetMenu")->property("entries").toList())
            rows.insert(entry.toMap().value("text").toString(), entry.toMap());
        QCOMPARE(rows.value("here").value("place").toString(), QString("local"));
        QCOMPARE(rows.value("turbo.safetensors").value("place").toString(), QString("local"));
        QCOMPARE(rows.value("shared").value("place").toString(), QString("host"));
        QCOMPARE(rows.value("shared").value("placeHost").toString(), QString("gx10"));
        QVERIFY(!rows.value("shared").value("dim").toBool());
        QCOMPARE(rows.value("dark").value("place").toString(), QString("host"));
        QVERIFY(rows.value("dark").value("dim").toBool());
        QCOMPARE(rows.value("dark").value("detail").toString(), QString("host offline"));
        // The badges are drawn, and say it in words.
        QStringList tips;
        std::function<void(QQuickItem *)> collect = [&](QQuickItem *from) {
            for (QQuickItem *child : from->childItems()) {
                if (child->objectName() == "widgetMenuPlace" && child->isVisible())
                    tips << child->property("tip").toString();
                collect(child);
            }
        };
        collect(app.window->contentItem());
        tips.sort();
        QCOMPARE(tips, (QStringList{"Host offline: spark", "On this machine", "On this machine", "Shared by gx10"}));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aModelFileOnDiskIsPickedFromItsFolderAndStoredByHash() {
        // A ComfyUI models folder under Extra model folders (8.9), and no provider model.
        QTemporaryDir models;
        const QByteArray bytes(2 * 1000 * 1000, 'm');
        QVERIFY(test::writeFixtureFile(models.filePath("image-model/turbo.safetensors"), bytes));
        WidgetApp app;
        app.model.widgets()->setModelRoots({models.path()});
        app.model.widgets()->setModelSource([](const QString &, const QStringList &,
                                               std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
        QVERIFY(app.open());
        app.click(app.in("all", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QStringList rows;
        for (const QVariant &entry : app.popup("widgetMenu")->property("entries").toList()) {
            const QVariantMap e = entry.toMap();
            rows << (e.value("heading").toBool() ? "# " : "") + e.value("text").toString()
                        + (e.value("detail").toString().isEmpty() ? QString() : " | " + e.value("detail").toString());
        }
        QCOMPARE(rows, (QStringList{"Any ready model", "# " + models.filePath("image-model"), "turbo.safetensors | 2 MB",
                                    "Type a name…"}));
        app.key(Qt::Key_Down);
        app.key(Qt::Key_Return);
        // Hashed in the background, then stored as one step; the face shows the file's name.
        QTRY_COMPARE(app.stored("all", "model"),
                     QJsonValue(QJsonObject{{"resource", "fugue/image-model"}, {"name", "turbo.safetensors"},
                                            {"hash", sha256(bytes)}}));
        QTRY_COMPARE(canvas_test::item(app.widget("all", "model"), "widgetValue")->property("text").toString(),
                     QString("turbo.safetensors"));
        QCOMPARE(app.steps(), 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #84: a long ComfyUI folder is filtered by typing. Every word must be in the file's name
    // or its folder; a heading with no match under it is left out; Escape empties the field first.
    void typingInTheModelPickerFiltersTheModels() {
        QTemporaryDir models;
        for (const char *file : {"image-model/z-image-turbo-Q8_0.gguf", "image-model/flux-2-klein-4b-Q8_0.gguf",
                                 "image-model/flux-2-klein-9b-Q8_0.gguf", "image-model/sdxl/RealVisXL.safetensors"})
            QVERIFY(test::writeFixtureFile(models.filePath(file), "weights"));
        WidgetApp app;
        app.model.widgets()->setModelRoots({models.path()});
        app.model.widgets()->setModelSource([](const QString &, const QStringList &,
                                               std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
        QVERIFY(app.open());
        app.click(app.in("all", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QObject *menu = app.popup("widgetMenu");
        const auto rows = [&] {
            QStringList out;
            for (const QVariant &entry : menu->property("shown").toList()) {
                const QVariantMap e = entry.toMap();
                out << (e.value("heading").toBool() ? "# " : "") + e.value("text").toString();
            }
            return out;
        };
        QTRY_COMPARE(rows().size(), 7); // Any ready model, the folder, four files, Type a name…
        QVERIFY(canvas_test::item(app.window->contentItem(), "widgetMenuSearch")); // shown, and has the keys

        app.type("klein 9");
        QTRY_COMPARE(rows(), (QStringList{"# " + models.filePath("image-model"), "flux-2-klein-9b-Q8_0.gguf",
                                          "Type a name…"}));
        app.key(Qt::Key_Escape); // empties the field, the menu stays
        QTRY_COMPARE(rows().size(), 7);
        QVERIFY(app.menuOpen());

        app.type("sdxl"); // the subfolder is part of the name
        QTRY_COMPARE(rows(), (QStringList{"# " + models.filePath("image-model"), "sdxl/RealVisXL.safetensors",
                                          "Type a name…"}));
        app.key(Qt::Key_Return); // the first match
        QTRY_VERIFY(!app.menuOpen());
        QTRY_COMPARE(app.stored("all", "model").toObject().value("name").toString(), QString("sdxl/RealVisXL.safetensors"));

        app.click(app.in("all", "model"));
        QTRY_VERIFY(app.menuOpen());
        menu = app.popup("widgetMenu");
        app.type("nothing like it");
        QTRY_COMPARE(rows(), (QStringList{"No model matches “nothing like it”", "Type a name…"}));
        app.key(Qt::Key_Escape);
        app.key(Qt::Key_Escape); // then closes
        QTRY_VERIFY(!app.menuOpen());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A long model list shows a scroll bar with a thumb of a grabbable length, a press on the track
    // pages, and a short list shows none.
    void aLongModelListShowsAScrollBarAndAShortOneDoesNot() {
        QTemporaryDir models;
        for (int i = 0; i < 400; ++i)
            QVERIFY(test::writeFixtureFile(models.filePath(QStringLiteral("image-model/model-%1.gguf").arg(i, 3, 10, QLatin1Char('0'))), "w"));
        WidgetApp app;
        app.model.widgets()->setModelRoots({models.path()});
        app.model.widgets()->setModelSource([](const QString &, const QStringList &,
                                               std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
        QVERIFY(app.open());
        app.click(app.in("all", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QQuickItem *list = canvas_test::item(app.window->contentItem(), "widgetMenuList");
        QVERIFY(list);
        QQuickItem *bar = canvas_test::item(list, "fugueScrollBar");
        QVERIFY(bar);
        QTRY_VERIFY(bar->isVisible());
        QVERIFY(bar->property("size").toDouble() < 0.2);
        // 400 rows: the thumb is still 32px long.
        QQuickItem *thumb = bar->property("contentItem").value<QQuickItem *>();
        QVERIFY(thumb);
        QVERIFY2(thumb->height() >= 31.5, qPrintable(QString::number(thumb->height())));
        // Not a check: with FUGUE_SHOT_DIR set, saves the window with the long list open.
        if (const QByteArray dir = qgetenv("FUGUE_SHOT_DIR"); !dir.isEmpty()) {
            QTest::qWait(150);
            QVERIFY(test_support::grab(app.window).save(QString::fromUtf8(dir) + "/model-picker-scrollbar.png"));
        }
        // A press on the track under the thumb scrolls the list down a page.
        QCOMPARE(list->property("contentY").toDouble(), 0.0);
        const QPointF below = bar->mapToScene(QPointF(bar->width() / 2, bar->height() - 4));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, below.toPoint());
        QTRY_VERIFY(list->property("contentY").toDouble() > 0);
        // No row's text runs under the bar, and the wheel and the thumb scroll the list.
        const QString under = scroll_checks::contentUnderBar(list, bar);
        QVERIFY2(under.isEmpty(), qPrintable(under));
        const QString failed = scroll_checks::scrollsByWheelAndThumb(app.window, list);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        app.key(Qt::Key_Escape);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.menuOpen());

        // A menu of a few choices fits: no bar.
        app.click(app.in("all", "mode"));
        QTRY_VERIFY(app.menuOpen());
        QQuickItem *shortList = canvas_test::item(app.window->contentItem(), "widgetMenuList");
        QVERIFY(shortList);
        QQuickItem *none = canvas_test::item(shortList, "fugueScrollBar");
        QVERIFY(none);
        QVERIFY(!none->isVisible());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #134: the Load Upscale Model node's picker lists the catalog under "Download", a filter finds
    // a model by its license or architecture, and picking one asks first: its license, its size and
    // where it comes from. Cancel fetches nothing; Download fetches it, checks it, and fills the input.
    void theUpscaleModelPickerOffersCatalogModelsAndAsksBeforeDownloading() {
        QTemporaryDir models;
        const QByteArray bytes(50000, 'u');
        fugue::test::FakeHttpServer server;
        QVERIFY(server.listen());
        int requests = 0;
        server.on("GET", "/m.safetensors", [&](const fugue::test::HttpRequest &) {
            ++requests;
            fugue::test::HttpResponse response;
            response.body = bytes;
            return response;
        });
        const auto entry = [&](const QString &id, const QString &license, bool commercial) {
            return QJsonObject{{"id", id}, {"resource", "fugue/upscale-model"}, {"title", "Model " + id},
                               {"description", "For " + id}, {"file", id + ".safetensors"},
                               {"url", QString::fromLatin1(server.origin()) + "/m.safetensors"}, {"size", bytes.size()},
                               {"sha256", sha256(bytes).mid(7)}, {"scale", 4}, {"architecture", "ESRGAN"},
                               {"license", license}, {"commercial", commercial}};
        };
        WidgetApp app(R"({"fugue": 1,
          "graph": {"nodes": {"load": {"type": "fugue/load-upscale-model", "version": "1.0.0"}}},
          "layout": {"nodes": {"load": {"pos": [20, 20]}}, "view": {"center": [480, 340], "zoom": 1}}})");
        app.model.widgets()->setModelRoots({models.path()});
        app.model.widgets()->setModelSource([](const QString &, const QStringList &,
                                               std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
        app.model.widgets()->setCatalog(ModelCatalog::parse(
            QJsonDocument(QJsonObject{{"catalog", 1}, {"models", QJsonArray{entry("alpha", "MIT", true), entry("beta", "CC-BY-NC-SA-4.0", false)}}})
                .toJson(),
            "test"));
        QVERIFY(app.open());
        app.click(app.in("load", "model"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QObject *menu = app.popup("widgetMenu");
        const auto rows = [&] {
            QStringList out;
            for (const QVariant &entry : menu->property("shown").toList()) {
                const QVariantMap e = entry.toMap();
                out << (e.value("heading").toBool() ? "# " : "") + e.value("text").toString();
            }
            return out;
        };
        QTRY_COMPARE(rows(), (QStringList{"# Download", "Model alpha", "Model beta", "Type a name…"}));
        const QString shots = qEnvironmentVariable("FUGUE_REAL_SHOTS"); // as the real-folder test: a picture of each
        if (!shots.isEmpty())
            app.window->grabWindow().save(shots + "/upscale-picker.png");
        app.type("nc"); // matches the license "CC-BY-NC-SA-4.0" in the keywords, not in the row's text
        QTRY_COMPARE(rows(), (QStringList{"# Download", "Model beta", "Type a name…"}));
        app.key(Qt::Key_Return);
        QTRY_VERIFY(!app.menuOpen());
        // The question names the license, says it is not for commercial use, and has fetched nothing.
        QObject *ask = nullptr;
        QTRY_VERIFY((ask = app.popup("downloadAsk")) != nullptr);
        QCOMPARE(canvas_test::item(app.window->contentItem(), "downloadAskLicense")->property("text").toString(),
                 QString("CC-BY-NC-SA-4.0, not for commercial use"));
        QCOMPARE(canvas_test::item(app.window->contentItem(), "downloadAskHost")->property("text").toString(), QString("127.0.0.1"));
        QCOMPARE(requests, 0);
        if (!shots.isEmpty()) {
            QTest::qWait(200);
            app.window->grabWindow().save(shots + "/upscale-ask.png");
        }
        QMetaObject::invokeMethod(canvas_test::item(app.window->contentItem(), "downloadAskCancel"), "clicked");
        QTRY_VERIFY(!app.popup("downloadAsk"));
        QTest::qWait(100);
        QCOMPARE(requests, 0);
        QCOMPARE(app.stored("load", "model"), QJsonValue(QJsonValue::Undefined));

        // Download: after the question's yes the file comes, and its handle is in the input.
        app.click(app.in("load", "model"));
        QTRY_VERIFY(app.menuOpen());
        app.type("alpha");
        app.key(Qt::Key_Return);
        QTRY_VERIFY(app.popup("downloadAsk"));
        QMetaObject::invokeMethod(canvas_test::item(app.window->contentItem(), "downloadAskOk"), "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(app.stored("load", "model").toObject().value("hash").toString(), sha256(bytes), 10000);
        QCOMPARE(app.stored("load", "model").toObject().value("name").toString(), QString("alpha.safetensors"));
        QCOMPARE(requests, 1);
        QVERIFY(QFileInfo::exists(models.filePath("upscale_models/alpha.safetensors")));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #85: with FLUX.2 Klein 9B picked in Load Models, its text-encoder list puts Qwen3-8B first;
    // Qwen3-4B (too narrow) and T5-XXL (as wide, but T5) follow under "Other", dimmed; a file
    // whose header tells nothing is under "Other" too, not dimmed.
    void loadModelsListsTheTextEncodersThatFitFirst() {
        QTemporaryDir models;
        const auto header = [](const QJsonObject &shapes) {
            QJsonObject tensors;
            for (auto it = shapes.begin(); it != shapes.end(); ++it)
                tensors.insert(it.key(), QJsonObject{{"dtype", "F16"}, {"shape", it.value()}, {"data_offsets", QJsonArray{0, 0}}});
            const QByteArray json = QJsonDocument(tensors).toJson(QJsonDocument::Compact);
            QByteArray out;
            for (int i = 0; i < 8; ++i)
                out.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
            return out + json;
        };
        const QByteArray klein9b = header({{"txt_in.weight", QJsonArray{4096, 12288}}, {"img_in.weight", QJsonArray{4096, 128}}});
        QVERIFY(test::writeFixtureFile(models.filePath("diffusion_models/klein9b.safetensors"), klein9b));
        QVERIFY(test::writeFixtureFile(models.filePath("text_encoders/qwen_3_4b.safetensors"),
                                       header({{"model.embed_tokens.weight", QJsonArray{151936, 2560}}})));
        QVERIFY(test::writeFixtureFile(models.filePath("text_encoders/qwen_3_8b.safetensors"),
                                       header({{"model.embed_tokens.weight", QJsonArray{151936, 4096}}})));
        QVERIFY(test::writeFixtureFile(models.filePath("text_encoders/t5xxl.safetensors"),
                                       header({{"encoder.embed_tokens.weight", QJsonArray{32128, 4096}},
                                               {"encoder.block.0.layer.0.SelfAttention.q.weight", QJsonArray{4096, 4096}}})));
        QVERIFY(test::writeFixtureFile(models.filePath("text_encoders/mystery.safetensors"), "not a header"));
        // Picked as the picker stores it: by hash (2.5).
        const QByteArray document = R"({"fugue": 1,
          "graph": {"nodes": {"models": {"type": "fugue/load-models", "version": "1.0.0",
            "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "name": "klein9b.safetensors",
                                           "hash": "HASH"}}}}},
          "layout": {"nodes": {"models": {"pos": [20, 20]}}, "view": {"center": [480, 340], "zoom": 1}}})";
        WidgetApp app(QByteArray(document).replace("HASH", sha256(klein9b).toUtf8()).constData());
        app.model.widgets()->setModelRoots({models.path()});
        app.model.widgets()->setModelSource([](const QString &, const QStringList &,
                                               std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
        app.model.widgets()->setCatalog(ModelCatalog()); // the picker's own rows, not the shipped Download rows
        QVERIFY(app.open());
        app.click(app.in("models", "text_encoder"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QObject *menu = app.popup("widgetMenu");
        const auto rows = [&] {
            QStringList out;
            for (const QVariant &entry : menu->property("entries").toList()) {
                const QVariantMap e = entry.toMap();
                out << (e.value("heading").toBool() ? "# " : "") + e.value("text").toString()
                           + (e.value("dim").toBool() ? " (dim)" : "");
            }
            return out;
        };
        QTRY_COMPARE(rows().join('|'), (QStringList{"None", "# Fits klein9b.safetensors", "qwen_3_8b.safetensors", "# Other",
                                                    "qwen_3_4b.safetensors (dim)", "mystery.safetensors", "t5xxl.safetensors (dim)",
                                                    "Family: All", "Type a name…"})
                                           .join('|'));
        app.key(Qt::Key_Down); // None is first (the text encoder is optional since 1.4.0), then the one that fits
        app.key(Qt::Key_Return);
        QTRY_COMPARE(app.stored("models", "text_encoder").toObject().value("name").toString(), QString("qwen_3_8b.safetensors"));
        // The list opens on the model in use, checked (the owner, 2026-11-24): the stored diffusion model is
        // matched by its hash when the file's is known, else by its name, and only one entry is checked.
        QTRY_VERIFY(!app.menuOpen());
        app.click(app.in("models", "diffusion_model"));
        QTRY_VERIFY(app.menuOpen());
        menu = app.popup("widgetMenu");
        QStringList checked;
        for (const QVariant &entry : menu->property("entries").toList())
            if (entry.toMap().value("checked").toBool())
                checked << entry.toMap().value("text").toString();
        QCOMPARE(checked, QStringList{"klein9b.safetensors"});
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #86: Apply LoRA, its model from Load Models with Klein 9B picked: the Klein 9B LoRA first,
    // the Klein 4B one (3072 wide) dimmed under "Other".
    void applyLoraListsTheLorasThatFitFirst() {
        QTemporaryDir models;
        const auto header = [](const QJsonObject &shapes) {
            QJsonObject tensors;
            for (auto it = shapes.begin(); it != shapes.end(); ++it)
                tensors.insert(it.key(), QJsonObject{{"dtype", "F16"}, {"shape", it.value()}, {"data_offsets", QJsonArray{0, 0}}});
            const QByteArray json = QJsonDocument(tensors).toJson(QJsonDocument::Compact);
            QByteArray out;
            for (int i = 0; i < 8; ++i)
                out.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
            return out + json;
        };
        const QByteArray klein9b = header({{"txt_in.weight", QJsonArray{4096, 12288}},
                                           {"img_in.weight", QJsonArray{4096, 128}},
                                           {"double_blocks.0.img_attn.proj.weight", QJsonArray{4096, 4096}}});
        QVERIFY(test::writeFixtureFile(models.filePath("diffusion_models/klein9b.safetensors"), klein9b));
        const auto lora = [&](int width) {
            return header({{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight", QJsonArray{16, width}},
                           {"diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight", QJsonArray{width, 16}}});
        };
        QVERIFY(test::writeFixtureFile(models.filePath("loras/a-klein4b.safetensors"), lora(3072)));
        QVERIFY(test::writeFixtureFile(models.filePath("loras/b-klein9b.safetensors"), lora(4096)));
        const QByteArray document = R"({"fugue": 1,
          "graph": {"nodes": {
            "models": {"type": "fugue/load-models", "version": "1.0.0",
              "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "name": "klein9b.safetensors", "hash": "HASH"}}},
            "lora": {"type": "fugue/apply-lora", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}}}}},
          "layout": {"nodes": {"models": {"pos": [20, 20]}, "lora": {"pos": [360, 20]}},
                     "view": {"center": [480, 340], "zoom": 1}}})";
        WidgetApp app(QByteArray(document).replace("HASH", sha256(klein9b).toUtf8()).constData());
        app.model.widgets()->setModelRoots({models.path()});
        app.model.widgets()->setModelSource([](const QString &, const QStringList &,
                                               std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
        app.model.widgets()->setCatalog(ModelCatalog()); // the picker's own rows, not the shipped Download rows
        QVERIFY(app.open());
        app.click(app.in("lora", "lora"));
        QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
        QObject *menu = app.popup("widgetMenu");
        const auto rows = [&] {
            QStringList out;
            for (const QVariant &entry : menu->property("entries").toList()) {
                const QVariantMap e = entry.toMap();
                out << (e.value("heading").toBool() ? "# " : "") + e.value("text").toString()
                           + (e.value("dim").toBool() ? " (dim)" : "");
            }
            return out;
        };
        QTRY_COMPARE(rows(), (QStringList{"# Fits klein9b.safetensors", "b-klein9b.safetensors", "# Other",
                                          "a-klein4b.safetensors (dim)", "Type a name…"}));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // By hand, against a real models folder (#85, #86): FUGUE_REAL_MODELS=<folders, ":" between>
    // and FUGUE_REAL_DIFFUSION=<a diffusion model's file name>:<its sha256> open Load Models and an
    // Apply LoRA wired from it, then each picker; every row is logged, and with FUGUE_REAL_SHOTS=<a
    // folder> each open menu is saved as a PNG there.
    void realModelPickersByHand() {
        const QString roots = qEnvironmentVariable("FUGUE_REAL_MODELS");
        const QStringList diffusion = qEnvironmentVariable("FUGUE_REAL_DIFFUSION").split(':');
        if (roots.isEmpty() || diffusion.size() != 2)
            QSKIP("set FUGUE_REAL_MODELS and FUGUE_REAL_DIFFUSION to look at real model folders");
        const QByteArray document = R"({"fugue": 1,
          "graph": {"nodes": {
            "models": {"type": "fugue/load-models", "version": "1.0.0",
              "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "name": "NAME", "hash": "sha256:HASH"}}},
            "lora": {"type": "fugue/apply-lora", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}}}}},
          "layout": {"nodes": {"models": {"pos": [20, 20]}, "lora": {"pos": [520, 20]}},
                     "view": {"center": [480, 340], "zoom": 1}}})";
        WidgetApp app(QByteArray(document).replace("NAME", diffusion.at(0).toUtf8()).replace("HASH", diffusion.at(1).toUtf8()).constData());
        app.model.widgets()->setModelRoots(roots.split(':'));
        app.model.widgets()->setModelSource([](const QString &, const QStringList &,
                                               std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
        QVERIFY(app.open());
        const QString shots = qEnvironmentVariable("FUGUE_REAL_SHOTS");
        for (const auto &[node, input] : {std::pair{"models", "diffusion_model"}, std::pair{"models", "text_encoder"},
                                          std::pair{"models", "vae"}, std::pair{"lora", "lora"}}) {
            app.click(app.in(node, input));
            QTRY_VERIFY2(app.menuOpen(), qPrintable(app.warnings.join('\n')));
            QObject *menu = app.popup("widgetMenu");
            // The headers are read on the pool: until a heading says what was judged, or 30 s.
            const auto judged = [&] {
                for (const QVariant &entry : menu->property("entries").toList())
                    if (entry.toMap().value("heading").toBool()
                        && (entry.toMap().value("text").toString().startsWith("Fits ") || entry.toMap().value("text") == "Other"))
                        return true;
                return false;
            };
            if (!QTest::qWaitFor(judged, 30000))
                qInfo("(nothing judged in 30 s)");
            qInfo("=== %s.%s", node, input);
            for (const QVariant &entry : menu->property("entries").toList()) {
                const QVariantMap e = entry.toMap();
                qInfo("%s%s%s%s", e.value("heading").toBool() ? "# " : "   ", qPrintable(e.value("text").toString()),
                      e.value("dim").toBool() ? "  (dim)" : "",
                      e.value("note").toString().isEmpty() ? "" : qPrintable("  -- " + e.value("note").toString()));
            }
            if (!shots.isEmpty())
                app.window->grabWindow().save(QStringLiteral("%1/%2-%3.png").arg(shots, QLatin1String(node), QLatin1String(input)));
            app.key(Qt::Key_Escape);
            app.key(Qt::Key_Escape);
            QTRY_VERIFY(!app.menuOpen());
        }
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
        QCOMPARE(rows, (QStringList{"Any ready model", "# lab", "Z-Image Turbo | host offline", "# studio",
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

FUGUE_TEST_MAIN(TestCanvasWidgets, QGuiApplication)
#include "test_canvas_widgets.moc"
