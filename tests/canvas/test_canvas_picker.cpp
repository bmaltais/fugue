// The `canvas` widget in the real window (Main.qml, offscreen) (#211, epic #206, spec 6.2, 14.5, 12.4):
// a Canvas Region's canvas input is a drop-down of this root's canvases (name, size, last change) with
// New canvas… and Import picture…; the value stays the canvas id; an id this root has none of shows as
// missing with the list still there; the open list follows the store. The store is the test's own folder;
// no file dialog is opened, no real window is launched and no real pointer is moved. New canvas… opens the
// New canvas dialog (#232); the dialog's own behavior is tested in test_new_canvas_window.

#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUndoStack>

#include "canvas/widget_edit.h"
#include "canvas_app.h"
#include "engine/canvas_store.h"
#include "engine/paint_file.h"
#include "engine/paths.h"

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
        QFile::remove(stateDir() + "/new-canvas.json"); // the New canvas dialog's remembered settings: each test starts on the defaults
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
            if (!e.toMap().value("separator").toBool())
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
    // What a person does: click the field, select its text, type over it.
    void fill(const QString &field, const QString &text) {
        QQuickItem *f = ask(field);
        QVERIFY2(f, qPrintable(field));
        QTest::mouseClick(window, Qt::LeftButton, {}, f->mapToScene(QPointF(f->width() / 2, f->height() / 2)).toPoint());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const QChar c : text)
            QTest::keyClick(window, c.toLatin1());
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
        QCOMPARE(app.face(), QString("Active canvas")); // empty is the Active canvas (#246)
        app.openList();
        QTRY_VERIFY2(app.listOpen(), qPrintable(app.warnings.join('\n')));
        QCOMPARE(app.texts(), (QStringList{"Active canvas", "No canvases yet", "New canvas…", "Import picture…", "Pick region…"}));
        QVERIFY(app.entries().first().toMap().value("checked").toBool()); // the active canvas is the checked default
        // The active canvas is whatever the Canvas view shows: a region can be picked on it.
        QVERIFY(app.entries().last().toMap().value("enabled", true).toBool());
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
        QCOMPARE(app.texts(), (QStringList{"Active canvas", "Photo", "Sketch", "New canvas…", "Import picture…", "Pick region…"}));
        // The actions are a chunk of their own: a hairline between the canvases and New canvas… (the cohesion audit's N10).
        QVERIFY(app.entries().at(3).toMap().value("separator").toBool());
        QCOMPARE(app.entries().at(4).toMap().value("text").toString(), QString("New canvas…"));
        const QVariantMap photo = app.entries().at(1).toMap();
        QVERIFY2(photo.value("detail").toString().startsWith("320 × 200 · "), qPrintable(photo.value("detail").toString()));
        QCOMPARE(photo.value("note").toString(), second); // the id, so two canvases of one name can be told apart
        QVERIFY(!photo.value("checked").toBool());
        app.choose("Sketch");
        QTRY_COMPARE(app.stored(), QJsonValue(first)); // the value is the id, not the name
        QCOMPARE(app.steps(), 1);
        QTRY_COMPARE(app.face(), QString("Sketch · pinned")); // a chosen canvas is pinned, and says so
        app.openList();
        QTRY_VERIFY(app.listOpen());
        QVERIFY(app.entries().at(2).toMap().value("checked").toBool());
        QVERIFY(!app.entries().at(0).toMap().value("checked").toBool());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.listOpen());
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.stored(), QJsonValue(QJsonValue::Undefined));
        QTRY_COMPARE(app.face(), QString("Active canvas"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #246: one click on the first row takes a pinned node back to the Active canvas.
    void activeCanvasUnpinsANodeInOneStep() {
        PickerApp app;
        const QString only = app.makeCanvas("Sketch");
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY(app.listOpen());
        app.choose("Sketch");
        QTRY_COMPARE(app.stored(), QJsonValue(only));
        app.openList();
        QTRY_VERIFY(app.listOpen());
        app.choose("Active canvas");
        QTRY_COMPARE(app.face(), QString("Active canvas"));
        QVERIFY(app.stored().toString().isEmpty());
        QCOMPARE(app.steps(), 2);
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
        QCOMPARE(app.texts(), (QStringList{"Active canvas", "canvas-0123456789ab", "Mine", "New canvas…", "Import picture…", "Pick region…"}));
        const QVariantMap gone = app.entries().at(1).toMap();
        QVERIFY(gone.value("dim").toBool());
        QVERIFY(gone.value("checked").toBool());
        QCOMPARE(gone.value("detail").toString(), QString("Not in this root"));
        app.choose("Mine");
        QTRY_COMPARE(app.stored(), QJsonValue(mine));
        QTRY_COMPARE(app.face(), QString("Mine · pinned"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #232: New canvas… is the New canvas dialog (NewCanvasDialog.qml; its own tests are test_new_canvas_window).
    void newCanvasOpensTheDialogAndPutsTheNewIdInTheInput() {
        PickerApp app;
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY(app.listOpen());
        app.choose("New canvas…");
        QTRY_VERIFY(app.popup("newCanvasDialog"));
        QTRY_VERIFY(app.popup("newCanvasDialog")->property("opened").toBool());
        // The defaults: Untitled 1, 1024 × 1024.
        QCOMPARE(app.ask("newCanvasName")->property("text").toString(), QString("Untitled 1"));
        QCOMPARE(app.ask("newCanvasWidth")->property("text").toString(), QString("1024"));
        QCOMPARE(app.ask("newCanvasHeight")->property("text").toString(), QString("1024"));
        app.fill("newCanvasName", "Poster");
        app.press("newCanvasPreset-landscape");
        app.press("newCanvasOk");
        QTRY_VERIFY(!app.popup("newCanvasDialog")->property("opened").toBool());
        const std::vector<canvas::CanvasInfo> made = app.writer().list();
        QCOMPARE(int(made.size()), 1);
        QCOMPARE(made[0].name, QString("Poster"));
        QCOMPARE(app.stored(), QJsonValue(made[0].id));
        QCOMPARE(app.steps(), 1);
        QTRY_COMPARE(app.face(), QString("Poster · pinned"));
        QCOMPARE(app.writer().load(made[0].id, made[0].head)->width(), 1024);
        QCOMPARE(app.writer().load(made[0].id, made[0].head)->height(), 768);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void cancelLeavesTheInputAndTheStoreAlone() {
        PickerApp app;
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY(app.listOpen());
        app.choose("New canvas…");
        QTRY_VERIFY(app.popup("newCanvasDialog"));
        QTRY_VERIFY(app.popup("newCanvasDialog")->property("opened").toBool());
        app.press("newCanvasCancel");
        QTRY_VERIFY(!app.popup("newCanvasDialog")->property("opened").toBool());
        QVERIFY(app.writer().list().empty());
        QCOMPARE(app.stored(), QJsonValue(QJsonValue::Undefined));
        QCOMPARE(app.steps(), 0);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theOpenListFollowsTheStore() {
        PickerApp app;
        QVERIFY(app.open());
        app.openList();
        QTRY_VERIFY(app.listOpen());
        QCOMPARE(app.texts(), (QStringList{"Active canvas", "No canvases yet", "New canvas…", "Import picture…", "Pick region…"}));
        app.makeCanvas("Arrived"); // `fugue canvas new` in another process, or a Canvas Write
        QTRY_COMPARE(app.texts(), (QStringList{"Active canvas", "Arrived", "New canvas…", "Import picture…", "Pick region…"}));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestCanvasPicker)
#include "test_canvas_picker.moc"
