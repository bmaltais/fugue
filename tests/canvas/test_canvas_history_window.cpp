// The canvas revision history in the real window (#210, epic #206, spec 14.5), offscreen: File > Canvas
// history… lists the canvases of the store, a canvas lists its revisions and candidates, a click compares
// a revision with its parent in the Compare viewer, and Accept and Drop act on a candidate. The store is
// the test's own (FUGUE_HOME and the XDG folders point at a temporary folder); no window is launched
// outside the test process and no real pointer is moved.

#include <QDir>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_app.h"
#include "../window_grab.h"
#include "engine/canvas_store.h"

using namespace canvas_test;
using namespace fugue;
using namespace fugue::mask;

namespace {

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

// Clicks once the layout has settled (the rows are rebuilt when the model changes).
void click(App &app, const QString &name, Qt::KeyboardModifiers modifiers = {}) {
    QPoint at;
    int still = 0;
    for (int i = 0; still < 3 && i < 500; ++i) {
        QTest::qWait(20);
        QQuickItem *now = find(app.window, name);
        if (!now) {
            still = 0;
            continue;
        }
        const QPoint here = centerOf(now);
        still = here == at ? still + 1 : 0;
        at = here;
    }
    QVERIFY2(still >= 3, qPrintable(QStringLiteral("%1 never settled").arg(name)));
    QTest::mouseClick(app.window, Qt::LeftButton, modifiers, at);
}

PaintDocument withFill(PaintDocument doc, const QString &name, int x) {
    doc.addLayer(LayerKind::Paint, name);
    doc.brush.color = Rgba{200, 10, 10, 255};
    doc.fillRect(x, 100, 60, 60);
    return doc;
}

} // namespace

class TestCanvasHistoryWindow : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head, m_a, m_b;

    static QString shortOf(const QString &id) { return id.mid(7, 8); }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        QVERIFY(m_dir.isValid());
        for (const char *variable : {"FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR"})
            qunsetenv(variable);
        qputenv("FUGUE_HOME", m_dir.filePath("home").toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath("config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", m_dir.filePath("data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", m_dir.filePath("state").toLocal8Bit());
        qputenv("XDG_CACHE_HOME", m_dir.filePath("cache").toLocal8Bit());
        QDir().mkpath(m_dir.filePath("run"));
        qputenv("XDG_RUNTIME_DIR", m_dir.filePath("run").toLocal8Bit());
    }

    void init() {
        QDir(m_dir.filePath("home")).removeRecursively();
        canvas::CanvasStore store = canvas::CanvasStore::forSession();
        PaintDocument base(512, 512);
        base.addLayer(LayerKind::Paint, "Base");
        base.brush.color = Rgba{30, 60, 200, 255};
        base.fillRect(0, 0, 512, 512);
        const auto made = store.create("Sketch", base, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
        m_a = store.addRevision(m_canvas, withFill(base, "Fill A", 100), m_head, "canvas-write:a").value().id;
        m_b = store.addRevision(m_canvas, withFill(base, "Fill B", 300), m_head, "canvas-write:b").value().id;
    }

    void theMenuOpensTheListOfCanvasesAndACanvasShowsItsRevisions() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("file.canvas-history"));
        QTRY_VERIFY(app.window->findChild<QObject *>("canvasHistoryDialog")->property("opened").toBool());
        click(app, "canvasHistoryCanvas-" + m_canvas);
        QTRY_VERIFY(find(app.window, "canvasHistoryRevision-" + shortOf(m_head)));
        QVERIFY(find(app.window, "canvasHistoryRevision-" + shortOf(m_a)));
        QVERIFY(find(app.window, "canvasHistoryRevision-" + shortOf(m_b)));
        // The head is selected at first: compared with nothing (it has no parent), nothing to decide.
        QVERIFY(!find(app.window, "canvasHistoryAccept"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theListOfCanvasesWorksFromTheKeyboard() {
        // Up and Down move the current canvas, Enter opens it, as the revision rows do (the cohesion audit's N6).
        canvas::CanvasStore store = canvas::CanvasStore::forSession();
        PaintDocument other(64, 64);
        other.addLayer(LayerKind::Paint, "Base");
        QVERIFY(store.create("Other", other, "new"));
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        app.commands.trigger("file.canvas-history");
        QObject *dialog = app.window->findChild<QObject *>("canvasHistoryDialog");
        QTRY_VERIFY(dialog->property("opened").toBool());
        QObject *history = dialog->property("history").value<QObject *>();
        const QVariantList rows = history->property("canvases").toList();
        QVERIFY(rows.size() >= 2);
        QTRY_VERIFY(find(app.window, "canvasHistoryCanvas-" + m_canvas));
        QTest::keyClick(app.window, Qt::Key_Down);
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_COMPARE(history->property("canvasId").toString(), rows.at(1).toMap().value("id").toString());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aCandidateIsComparedWithTheHeadThenAccepted() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        app.commands.trigger("file.canvas-history");
        click(app, "canvasHistoryCanvas-" + m_canvas);
        click(app, "canvasHistoryRevision-" + shortOf(m_a));
        QObject *dialog = app.window->findChild<QObject *>("canvasHistoryDialog");
        QObject *history = dialog->property("history").value<QObject *>();
        QTRY_COMPARE(history->property("revisionB").toString(), m_a);
        QCOMPARE(history->property("revisionA").toString(), m_head);
        QCOMPARE(history->property("kindB").toString(), QString("candidate"));
        QCOMPARE(history->property("sizeA").toSize(), QSize(512, 512));
        QVERIFY(QFile::exists(history->property("sourceA").toUrl().toLocalFile()));
        QVERIFY(QFile::exists(history->property("sourceB").toUrl().toLocalFile()));
        QVERIFY(QFile::exists(history->property("sourceDifference").toUrl().toLocalFile()));
        QVERIFY(history->property("summary").toStringList().at(0).startsWith("Layer Fill A added"));
        QTRY_VERIFY(find(app.window, "canvasHistorySummary"));
        QVERIFY(find(app.window, "canvasHistoryAccept"));
        QVERIFY(find(app.window, "canvasHistoryDrop"));
        // The viewer is the Compare viewer, on the two pictures.
        QObject *viewer = dialog->property("viewer").value<QObject *>();
        QCOMPARE(viewer->property("sizeB").toSize(), QSize(512, 512));

        click(app, "canvasHistoryAccept");
        QTRY_VERIFY(!find(app.window, "canvasHistoryAccept"));
        QCOMPARE(canvas::CanvasStore::forSession().info(m_canvas)->head, m_a);
        QCOMPARE(history->property("kindB").toString(), QString("head"));
        // The other candidate was made from the old head: still listed, and it can still be accepted (placed on the head).
        // It is compared with the head it would change, not with its parent (one meaning of Compare, as the Canvas view's).
        click(app, "canvasHistoryRevision-" + shortOf(m_b));
        QTRY_COMPARE(history->property("kindB").toString(), QString("candidate"));
        QCOMPARE(history->property("revisionA").toString(), m_a);
        QCOMPARE(find(app.window, "canvasHistoryCompare")->property("tip").toString(), QString("Show this candidate against the Head"));
        click(app, "canvasHistoryAccept");
        QTRY_VERIFY(!find(app.window, "canvasHistoryAccept"));
        const auto head = canvas::CanvasStore::forSession().info(m_canvas)->head;
        QCOMPARE(canvas::CanvasStore::forSession().load(m_canvas, head)->layerCount(), 3);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void dropHidesACandidateAndTheOtherSideCanBeChosenWithControlClick() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        app.commands.trigger("file.canvas-history");
        click(app, "canvasHistoryCanvas-" + m_canvas);
        click(app, "canvasHistoryRevision-" + shortOf(m_b));
        QObject *history = app.window->findChild<QObject *>("canvasHistoryDialog")->property("history").value<QObject *>();
        QTRY_COMPARE(history->property("revisionB").toString(), m_b);
        // Ctrl+click makes a revision the A side: the other candidate, B unchanged.
        click(app, "canvasHistoryRevision-" + shortOf(m_a), Qt::ControlModifier);
        QTRY_COMPARE(history->property("revisionA").toString(), m_a);
        QCOMPARE(history->property("revisionB").toString(), m_b);
        QVERIFY(history->property("summary").toStringList().join('\n').contains("Fill A removed"));
        click(app, "canvasHistoryRevision-" + shortOf(m_b));
        QTRY_COMPARE(history->property("revisionA").toString(), m_head);
        click(app, "canvasHistoryDrop");
        QTRY_COMPARE(canvas::CanvasStore::forSession().candidates(m_canvas)->size(), size_t(1));
        // Dropped is kept in the list, marked, and Accept still offered; Drop is not.
        QTRY_COMPARE(history->property("kindB").toString(), QString("dropped"));
        QVERIFY(find(app.window, "canvasHistoryAccept"));
        QVERIFY(!find(app.window, "canvasHistoryDrop"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aRevisionIsShownAloneAtFullSizeAndCompareIsAToggle() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        app.commands.trigger("file.canvas-history");
        click(app, "canvasHistoryCanvas-" + m_canvas);
        QObject *dialog = app.window->findChild<QObject *>("canvasHistoryDialog");
        QObject *history = dialog->property("history").value<QObject *>();
        QObject *viewer = dialog->property("viewer").value<QObject *>();
        // The head is shown at first, alone: the viewer is in its single mode on the head's picture.
        QTRY_COMPARE(history->property("revisionB").toString(), m_head);
        QCOMPARE(viewer->property("mode").toString(), QString("single"));
        QCOMPARE(viewer->property("sourceA").toUrl(), history->property("sourceB").toUrl());
        QCOMPARE(viewer->property("sizeA").toSize(), QSize(512, 512));
        QVERIFY(!find(app.window, "canvasHistoryMode-slider"));
        // The first revision has no parent: nothing to compare with, so the toggle is off.
        QQuickItem *toggle = find(app.window, "canvasHistoryCompare");
        QVERIFY(toggle);
        QVERIFY(!toggle->isEnabled());
        // A toggle that is on never takes the accent fill: Accept is this window's one primary (the cohesion audit's D7).
        QVERIFY(!toggle->property("accented").toBool());
        QVERIFY(toggle->property("checkable").toBool());
        QVERIFY(find(app.window, "canvasHistoryExport"));

        // A candidate is shown alone too; Compare puts it against its parent in the Compare modes.
        click(app, "canvasHistoryRevision-" + shortOf(m_a));
        QTRY_COMPARE(history->property("revisionB").toString(), m_a);
        QCOMPARE(viewer->property("mode").toString(), QString("single"));
        QCOMPARE(viewer->property("sourceA").toUrl(), history->property("sourceB").toUrl());
        QVERIFY(viewer->property("sourceA").toUrl() != history->property("sourceA").toUrl());
        click(app, "canvasHistoryCompare");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("slider"));
        QCOMPARE(viewer->property("sourceA").toUrl(), history->property("sourceA").toUrl());
        QCOMPARE(viewer->property("sourceB").toUrl(), history->property("sourceB").toUrl());
        QVERIFY(find(app.window, "canvasHistoryMode-side_by_side"));
        // Another revision keeps the comparison; the toggle again goes back to the one picture.
        click(app, "canvasHistoryMode-side_by_side");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("side_by_side"));
        click(app, "canvasHistoryCompare");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("single"));
        QVERIFY(!find(app.window, "canvasHistoryMode-slider"));
        // Compare again comes back in the mode last used.
        click(app, "canvasHistoryCompare");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("side_by_side"));
        // Ctrl+click on a row (the A side) is a comparison, so it turns Compare on.
        click(app, "canvasHistoryCompare");
        QTRY_COMPARE(viewer->property("mode").toString(), QString("single"));
        click(app, "canvasHistoryRevision-" + shortOf(m_b), Qt::ControlModifier);
        QTRY_COMPARE(viewer->property("mode").toString(), QString("side_by_side"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void exportSavesTheShownRevisionAsAFlattenedPngAndNeverReplaces() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        app.commands.trigger("file.canvas-history");
        click(app, "canvasHistoryCanvas-" + m_canvas);
        click(app, "canvasHistoryRevision-" + shortOf(m_a));
        QObject *dialog = app.window->findChild<QObject *>("canvasHistoryDialog");
        QObject *history = dialog->property("history").value<QObject *>();
        QTRY_COMPARE(history->property("revisionB").toString(), m_a);
        QVERIFY(history->property("exportName").toString().startsWith("Sketch-" + shortOf(m_a)));
        QVERIFY(history->property("exportName").toString().endsWith(".png"));

        // The save dialog is the window's FileDialog; its accepted signal is what writes (no dialog is shown).
        QObject *picker = app.window->findChild<QObject *>("canvasHistoryExportDialog");
        QVERIFY(picker);
        const QString target = m_dir.filePath("out/shown.png");
        QVERIFY(QDir().mkpath(m_dir.filePath("out")));
        picker->setProperty("selectedFile", QUrl::fromLocalFile(target));
        QMetaObject::invokeMethod(picker, "accepted");
        QTRY_VERIFY(QFile::exists(target));
        const auto document = canvas::CanvasStore::forSession().load(m_canvas, m_a);
        QFile written(target);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(written.readAll(), document->flattenToPng().value());
        QVERIFY(history->property("notice").toString().contains("shown.png"));
        QVERIFY(history->property("error").toString().isEmpty());

        // A second save to the same name changes nothing and says why.
        written.close();
        const qint64 size = QFileInfo(target).size();
        bool ok = true;
        QMetaObject::invokeMethod(history, "exportPicture", Q_RETURN_ARG(bool, ok), Q_ARG(QString, target));
        QVERIFY(!ok);
        QVERIFY(!history->property("error").toString().isEmpty());
        QCOMPARE(QFileInfo(target).size(), size);
        // Another extension gets .png added, and a file URL works as well as a path.
        QMetaObject::invokeMethod(history, "exportPicture", Q_RETURN_ARG(bool, ok), Q_ARG(QString, QUrl::fromLocalFile(m_dir.filePath("out/other.jpg")).toString()));
        QVERIFY(ok);
        QVERIFY(QFile::exists(m_dir.filePath("out/other.jpg.png")));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aWindowOnAMissingCanvasSaysSoInsteadOfCrashing() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        app.commands.trigger("file.canvas-history");
        QObject *dialog = app.window->findChild<QObject *>("canvasHistoryDialog");
        QTRY_VERIFY(dialog);
        QObject *history = dialog->property("history").value<QObject *>();
        history->setProperty("canvasId", "canvas-nope");
        QVERIFY(history->property("error").toString().contains("canvas-nope"));
        QVERIFY(history->property("revisions").toList().isEmpty());
    }

    // An empty store says what to do in the app (#228, the cohesion audit's L6): New canvas… makes one and opens it.
    void anEmptyStoreOffersToMakeACanvas() {
        QDir(m_dir.filePath("home")).removeRecursively();
        QDir(m_dir.filePath("data")).removeRecursively();
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("file.canvas-history"));
        QTRY_VERIFY(find(app.window, "canvasHistoryNew"));
        QCOMPARE(find(app.window, "canvasHistoryEmpty")->property("text").toString(), QString("No canvases yet"));
        QVERIFY(find(app.window, "canvasHistoryImport"));
        click(app, "canvasHistoryNew");
        QObject *ask = nullptr;
        QTRY_VERIFY((ask = app.window->findChild<QObject *>("newCanvasDialog")) && ask->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(ask, "create"));
        QObject *history = app.window->findChild<QObject *>("canvasHistoryDialog")->property("history").value<QObject *>();
        QTRY_VERIFY(!history->property("canvasId").toString().isEmpty());
        QCOMPARE(canvas::CanvasStore::forSession().info(history->property("canvasId").toString())->name, QString("Untitled 1"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_canvas_history_window savesThePictures (nothing without it), dark and light: the
    // list of canvases, and a candidate compared with the Head.
    void savesThePictures_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << "tokyo-night" << "dark";
        QTest::newRow("light") << "catppuccin-latte" << "light";
    }
    void savesThePictures() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        QFETCH(QString, theme);
        QFETCH(QString, name);
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"), {}, Look{theme, 8});
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("file.canvas-history"));
        QTRY_VERIFY(find(app.window, "canvasHistoryCanvas-" + m_canvas));
        const auto save = [&](const QString &what) {
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/canvas-history-%2-%3.png").arg(QString::fromLocal8Bit(dir), name, what)));
        };
        save("canvases");
        click(app, "canvasHistoryCanvas-" + m_canvas);
        click(app, "canvasHistoryRevision-" + shortOf(m_a));
        click(app, "canvasHistoryCompare");
        QTRY_VERIFY(find(app.window, "canvasHistoryMode-slider"));
        save("compare");
    }
};

QTEST_MAIN(TestCanvasHistoryWindow)
#include "test_canvas_history_window.moc"
