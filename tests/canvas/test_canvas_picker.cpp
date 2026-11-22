// The `canvas` widget in the real window (Main.qml, offscreen) (#211, epic #206, spec 6.2, 14.5, 12.4):
// a Canvas Region's canvas input is a drop-down of this root's canvases (name, size, last change) with
// New canvas… and Import picture…; the value stays the canvas id; an id this root has none of shows as
// missing with the list still there; the open list follows the store. The store is the test's own folder;
// no file dialog is opened (the picture is handed to the New canvas dialog as the dialog would), no real
// window is launched and no real pointer is moved.

#include <QImage>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QUrl>

#include "canvas/widget_edit.h"
#include "canvas_app.h"
#include "engine/canvas_store.h"
#include "engine/paint_file.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::load;

namespace {

const char *fixture = R"({
  "fugue": 1,
  "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.0.0"}}},
  "layout": {"nodes": {"region": {"pos": [20, 20]}}, "view": {"center": [200, 200], "zoom": 1}}
})";

struct PickerApp : App {
    QTemporaryDir store;
    PickerApp(const QString &savedId = {}) : App(load(savedId.isEmpty() ? QByteArray(fixture) : withCanvas(savedId))) {
        model.widgets()->canvasChoices()->setStoreRoot(store.filePath("store"));
    }
    static QByteArray withCanvas(const QString &id) {
        QByteArray text = fixture;
        return text.replace(R"("version": "1.0.0"})", R"("version": "1.0.0", "inputs": {"canvas": ")" + id.toUtf8() + R"("}})");
    }
    canvas::CanvasStore writer() const { return canvas::CanvasStore(store.filePath("store")); }
    QString makeCanvas(const QString &name, int width = 64, int height = 48) const {
        mask::PaintDocument doc(width, height);
        doc.addLayer(mask::LayerKind::Paint, "L");
        canvas::CanvasStore s = writer();
        return s.create(name, doc, "new")->id;
    }

    QQuickItem *widget() const {
        QList<QQuickItem *> todo{window->contentItem()};
        while (!todo.isEmpty()) {
            QQuickItem *at = todo.takeFirst();
            if (at->objectName() == "inputWidget" && at->property("name").toString() == "canvas")
                return at;
            todo << at->childItems();
        }
        return nullptr;
    }
    QString face() const { return canvas_test::item(widget(), "widgetValue")->property("text").toString(); }
    void openList() {
        QTest::mouseClick(window, Qt::LeftButton, {}, widget()->mapToScene(QPointF(widget()->width() / 2, widget()->height() / 2)).toPoint());
    }
    QObject *popup(const char *name) const {
        QQuickItem *w = widget();
        if (auto *pickers = w ? w->property("pickers").value<QObject *>() : nullptr)
            return pickers->findChild<QObject *>(name);
        return nullptr;
    }
    bool listOpen() const {
        QObject *menu = popup("widgetMenu");
        return menu && menu->property("opened").toBool();
    }
    QVariantList entries() const { return popup("widgetMenu")->property("entries").toList(); }
    QStringList texts() const {
        QStringList out;
        for (const QVariant &e : entries())
            out << e.toMap().value("text").toString();
        return out;
    }
    // Chooses the entry with this text, as a click on its row does.
    void choose(const QString &text) {
        const QVariantList shown = popup("widgetMenu")->property("shown").toList();
        for (int i = 0; i < shown.size(); ++i)
            if (shown[i].toMap().value("text").toString() == text) {
                QVERIFY(QMetaObject::invokeMethod(popup("widgetMenu"), "choose", Q_ARG(QVariant, i)));
                return;
            }
        QFAIL(qPrintable("no entry " + text));
    }
    QJsonValue stored() const { return model.document().graph.nodes.value("region").inputs.value("canvas").toJson(); }
    int steps() const { return model.undoStack()->count(); }
    QQuickItem *ask(const QString &name) const { return canvas_test::item(window->contentItem(), name); }
    void fill(const QString &field, const QString &text) {
        QQuickItem *f = ask(field);
        QVERIFY2(f, qPrintable(field));
        f->setProperty("text", text);
    }
    void press(const QString &button) {
        QQuickItem *b = ask(button);
        QVERIFY2(b, qPrintable(button));
        QTest::mouseClick(window, Qt::LeftButton, {}, b->mapToScene(QPointF(b->width() / 2, b->height() / 2)).toPoint());
    }
};

} // namespace

class TestCanvasPicker : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theInputIsAPickerAndAnEmptyRootOffersTheTwoWaysToMakeOne() {
        PickerApp app;
        QVERIFY(app.open());
        QVERIFY(app.widget());
        QCOMPARE(app.widget()->property("kind").toString(), QString("canvas"));
        QCOMPARE(app.face(), QString("Choose a canvas"));
        app.openList();
        QTRY_VERIFY2(app.listOpen(), qPrintable(app.warnings.join('\n')));
        QCOMPARE(app.texts(), (QStringList{"No canvases yet", "New canvas…", "Import picture…"}));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theListShowsNameSizeAndTimeAndPickingStoresTheId() {
        PickerApp app;
        const QString first = app.makeCanvas("Sketch", 800, 600);
        QTest::qWait(1100);
        const QString second = app.makeCanvas("Photo", 320, 200);
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY2(app.listOpen(), qPrintable(app.warnings.join('\n')));
        QCOMPARE(app.texts(), (QStringList{"Photo", "Sketch", "New canvas…", "Import picture…"}));
        const QVariantMap photo = app.entries().at(0).toMap();
        QVERIFY2(photo.value("detail").toString().startsWith("320 × 200 · "), qPrintable(photo.value("detail").toString()));
        QCOMPARE(photo.value("note").toString(), second); // the id, so two canvases of one name can be told apart
        QVERIFY(!photo.value("checked").toBool());
        app.choose("Sketch");
        QTRY_COMPARE(app.stored(), QJsonValue(first)); // the value is the id, not the name
        QCOMPARE(app.steps(), 1);
        QTRY_COMPARE(app.face(), QString("Sketch"));
        app.openList();
        QTRY_VERIFY(app.listOpen());
        QVERIFY(app.entries().at(1).toMap().value("checked").toBool());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.listOpen());
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.stored(), QJsonValue(QJsonValue::Undefined));
        QTRY_COMPARE(app.face(), QString("Choose a canvas"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The owner's case: an id made in another root (a project has its own store).
    void anIdThisRootHasNoneOfShowsAsMissingWithTheListStillThere() {
        PickerApp app("canvas-0123456789ab");
        const QString mine = app.makeCanvas("Mine");
        QVERIFY(app.open());
        QCOMPARE(app.face(), QString("canvas-0123456789ab (missing)"));
        app.openList();
        QTRY_VERIFY2(app.listOpen(), qPrintable(app.warnings.join('\n')));
        QCOMPARE(app.texts(), (QStringList{"canvas-0123456789ab", "Mine", "New canvas…", "Import picture…"}));
        const QVariantMap gone = app.entries().at(0).toMap();
        QVERIFY(gone.value("dim").toBool());
        QVERIFY(gone.value("checked").toBool());
        QCOMPARE(gone.value("detail").toString(), QString("not in this root"));
        app.choose("Mine");
        QTRY_COMPARE(app.stored(), QJsonValue(mine));
        QTRY_COMPARE(app.face(), QString("Mine"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void newCanvasAsksANameAndASizeAndPutsTheNewIdInTheInput() {
        PickerApp app;
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY(app.listOpen());
        app.choose("New canvas…");
        QTRY_VERIFY(app.popup("newCanvasAsk"));
        // The defaults: Untitled, 1024 × 1024.
        QCOMPARE(app.ask("newCanvasName")->property("text").toString(), QString("Untitled"));
        QCOMPARE(app.ask("newCanvasWidth")->property("text").toString(), QString("1024"));
        QCOMPARE(app.ask("newCanvasHeight")->property("text").toString(), QString("1024"));
        app.fill("newCanvasName", "Poster");
        app.fill("newCanvasWidth", "640");
        app.fill("newCanvasHeight", "480");
        app.press("newCanvasOk");
        QTRY_VERIFY(!app.popup("newCanvasAsk"));
        const std::vector<canvas::CanvasInfo> made = app.writer().list();
        QCOMPARE(int(made.size()), 1);
        QCOMPARE(made[0].name, QString("Poster"));
        QCOMPARE(app.stored(), QJsonValue(made[0].id));
        QCOMPARE(app.steps(), 1);
        QTRY_COMPARE(app.face(), QString("Poster"));
        QCOMPARE(app.writer().load(made[0].id, made[0].head)->width(), 640);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aSizeOutsideTheLimitsIsRefusedInTheDialogAndNothingIsMade() {
        PickerApp app;
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY(app.listOpen());
        app.choose("New canvas…");
        QTRY_VERIFY(app.popup("newCanvasAsk"));
        app.fill("newCanvasWidth", "20000");
        app.press("newCanvasOk");
        QTRY_VERIFY(app.ask("newCanvasProblem") && app.ask("newCanvasProblem")->isVisible());
        QVERIFY(app.ask("newCanvasProblem")->property("text").toString().contains("at most 16384 px a side"));
        QVERIFY(app.popup("newCanvasAsk")); // still open, to correct it
        QVERIFY(app.writer().list().empty());
        QCOMPARE(app.steps(), 0);
        app.fill("newCanvasWidth", "");
        app.press("newCanvasOk"); // not a number
        QTRY_VERIFY(app.ask("newCanvasProblem")->isVisible());
        QVERIFY(app.writer().list().empty());
        app.press("newCanvasCancel");
        QTRY_VERIFY(!app.popup("newCanvasAsk"));
        QCOMPARE(app.stored(), QJsonValue(QJsonValue::Undefined));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void newCanvasFromAPictureTakesItsSizeAndNameAndCanGoBackToBlank() {
        PickerApp app;
        QTemporaryDir files;
        QImage image(300, 120, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        QVERIFY(image.save(files.filePath("harbour.png")));
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY(app.listOpen());
        app.choose("New canvas…");
        QTRY_VERIFY(app.popup("newCanvasAsk"));
        // What the file dialog does on Open: the dialog is handed the picture.
        QObject *ask = app.popup("newCanvasAsk");
        QVERIFY(ask);
        ask->setProperty("picture", QUrl::fromLocalFile(files.filePath("harbour.png")));
        QTRY_COMPARE(app.ask("newCanvasPictureLine")->property("text").toString(), QString("harbour.png · 300 × 120"));
        QTRY_COMPARE(app.ask("newCanvasName")->property("text").toString(), QString("harbour"));
        QVERIFY(!app.ask("newCanvasWidth")->isVisible());
        app.press("newCanvasFromPicture"); // Blank canvas instead
        QTRY_VERIFY(app.ask("newCanvasWidth")->isVisible());
        ask->setProperty("picture", QUrl::fromLocalFile(files.filePath("harbour.png")));
        QTRY_VERIFY(!app.ask("newCanvasWidth")->isVisible());
        app.press("newCanvasOk");
        QTRY_VERIFY(!app.popup("newCanvasAsk"));
        const std::vector<canvas::CanvasInfo> made = app.writer().list();
        QCOMPARE(int(made.size()), 1);
        QCOMPARE(made[0].name, QString("harbour"));
        QCOMPARE(app.writer().load(made[0].id, made[0].head)->height(), 120);
        QCOMPARE(app.stored(), QJsonValue(made[0].id));
        QCOMPARE(app.steps(), 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theOpenListFollowsTheStore() {
        PickerApp app;
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY(app.listOpen());
        QCOMPARE(app.texts(), (QStringList{"No canvases yet", "New canvas…", "Import picture…"}));
        app.makeCanvas("Arrived"); // `fugue canvas new` in another process, or a Canvas Write
        QTRY_COMPARE(app.texts(), (QStringList{"Arrived", "New canvas…", "Import picture…"}));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestCanvasPicker)
#include "test_canvas_picker.moc"
