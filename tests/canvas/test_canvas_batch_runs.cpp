// Canvas > Automate > Batch… (#250): a canvas action over a folder of pictures, through the window's RunService with a
// FakeProvider on the run thread and the canvas store in the test's own FUGUE_HOME. Two seams: CanvasBatch alone (each
// picture becomes a canvas, its run lands a layer, the head is written to the output folder; a failed picture is listed
// and the batch goes on; Cancel ends the run under way and marks the rest), and the dialog in the real window, offscreen,
// QTest events only: the menu row, the action, the prompt, Run, the progress and the results list. The engine's own
// loop is tests/nodes/test_canvas_batch.cpp.

#include <QDir>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_actions_rig.h"
#include "canvas/canvas_batch.h"
#include "engine/canvas_store.h"

using namespace actions_test;

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

QPoint settled(QQuickWindow *window, const QString &name) {
    QPoint at(-1, -1);
    int still = 0;
    for (int i = 0; still < 3 && i < 500; ++i) {
        QTest::qWait(20);
        QQuickItem *now = find(window, name);
        if (!now) {
            still = 0;
            continue;
        }
        const QPoint here = centerOf(now);
        still = here == at ? still + 1 : 0;
        at = here;
    }
    return still >= 3 ? at : QPoint(-1, -1);
}

bool tipShown(QQuickItem *item) { return QQmlProperty(item, "ToolTip.visible", qmlContext(item)).read().toBool(); }

} // namespace

class TestCanvasBatchRuns : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }
    QString pictures(int count) {
        const QString folder = path(QStringLiteral("pictures-%1").arg(m_folders++));
        QDir().mkpath(folder);
        for (int i = 0; i < count; ++i)
            writeFile(QStringLiteral("%1/p%2.png").arg(folder).arg(i + 1), png(64, 64, QColor(40 * i, 90, 160)));
        return folder;
    }
    int m_folders = 0;

    // Answers job `index` with a square picture and waits for the batch to take the next one.
    void finishJob(Rig &r, int index) {
        QTRY_VERIFY(r.jobs().size() > index);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(index), {{"image.png", png(64, 64)}}); });
    }
    int canvases() const { return int(canvas::CanvasStore::forSession().list().size()); }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        QVERIFY(m_dir.isValid());
        for (const char *variable : {"FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR"})
            qunsetenv(variable);
        qputenv("FUGUE_HOME", path("home").toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", path("config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", path("data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", path("state").toLocal8Bit());
        qputenv("XDG_CACHE_HOME", path("cache").toLocal8Bit());
        QDir().mkpath(path("run"));
        qputenv("XDG_RUNTIME_DIR", path("run").toLocal8Bit());
    }
    void init() {
        QDir(path("home")).removeRecursively();
        canvas::CanvasStore store = canvas::CanvasStore::forSession(); // the store outlives a run of the test
        for (const canvas::CanvasInfo &old : store.list())
            store.remove(old.id);
        PaintDocument d(256, 256); // a canvas for the Canvas view to show
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(256, 256, {30, 60, 200, 255}));
        QVERIFY(store.create("Shown", d, "new"));
    }

    void eachPictureIsRunAndExportedAndTheCanvasIsRemoved() {
        Rig r;
        CanvasBatch batch(r.service.get(), r.actions.get());
        const QString in = pictures(2), out = path("out-a");
        // Only actions that can run here are offered: Generative fill's models are not on this machine, so it is listed
        // with its missing models (the dialog greys it) and refused when started.
        const QVariantMap refused = batch.start({{"action", "generative-fill"}, {"inFolder", in}, {"outFolder", out}, {"prompt", "x"}});
        QVERIFY(!refused.value("ok").toBool());
        QVERIFY(refused.value("reason").toString().contains("models"));
        QVERIFY(!batch.start({{"action", "broken"}, {"inFolder", in}, {"outFolder", out}}).value("ok").toBool());
        QVERIFY(!batch.start({{"action", "test-fill"}, {"inFolder", in}, {"outFolder", out}}).value("ok").toBool()); // no prompt
        QVERIFY(!batch.start({{"action", "test-fill"}, {"inFolder", pictures(0)}, {"outFolder", out}, {"prompt", "x"}}).value("ok").toBool());

        const QVariantMap started = batch.start({{"action", "test-fill"}, {"inFolder", QUrl::fromLocalFile(in).toString()}, {"outFolder", out},
                                                 {"format", "png"}, {"prompt", "a hat"}});
        QVERIFY2(started.value("ok").toBool(), qPrintable(started.value("reason").toString()));
        QVERIFY(batch.busy());
        QCOMPARE(batch.total(), 2);
        QVERIFY(!batch.start({{"action", "test-fill"}, {"inFolder", in}, {"outFolder", out}, {"prompt", "x"}}).value("ok").toBool()); // one at a time
        QCOMPARE(batch.results().at(0).toMap().value("status").toString(), QString("running"));
        QCOMPARE(batch.results().at(1).toMap().value("status").toString(), QString("waiting"));
        r.onFake([&](FakeProvider &f) { QTRY_VERIFY(!f.jobs().isEmpty()); });
        finishJob(r, 0);
        QTRY_COMPARE(batch.done(), 1);
        finishJob(r, 1);
        QTRY_VERIFY(batch.finished());
        QVERIFY(!batch.busy());
        QCOMPARE(batch.summary(), QString("2 pictures done"));
        QVERIFY(QFile::exists(out + "/p1.png"));
        QVERIFY(QFile::exists(out + "/p2.png"));
        QCOMPARE(batch.results().at(1).toMap().value("status").toString(), QString("ok"));
        QCOMPARE(canvases(), 1); // only the one the view shows
        QVERIFY(!r.runs->busy()); // the Graph's runs never saw them
        batch.reset();
        QVERIFY(!batch.finished());
    }

    void aFailedPictureIsListedAndTheBatchGoesOn() {
        Rig r;
        CanvasBatch batch(r.service.get(), r.actions.get());
        const QString in = pictures(3);
        QVERIFY(batch.start({{"action", "test-fill"}, {"inFolder", in}, {"outFolder", path("out-b")}, {"prompt", "x"}}).value("ok").toBool());
        QTRY_VERIFY(r.jobs().size() > 0);
        r.onFake([&](FakeProvider &f) { f.fail(f.jobs().at(0), makeError("out_of_memory", "CUDA out of memory: tried to allocate 2 GiB.")); });
        finishJob(r, 1);
        finishJob(r, 2);
        QTRY_VERIFY(batch.finished());
        QCOMPARE(batch.results().at(0).toMap().value("status").toString(), QString("failed"));
        QVERIFY(!batch.results().at(0).toMap().value("error").toString().isEmpty());
        QCOMPARE(batch.results().at(1).toMap().value("status").toString(), QString("ok"));
        QCOMPARE(batch.results().at(2).toMap().value("status").toString(), QString("ok"));
        QCOMPARE(batch.summary(), QString("2 pictures done, 1 failed"));
        QVERIFY(!QFile::exists(path("out-b/p1.png")));
        QVERIFY(QFile::exists(path("out-b/p3.png")));
        QCOMPARE(canvases(), 1); // only the one the view shows
    }

    void cancelEndsTheRunUnderWayAndMarksTheRest() {
        Rig r;
        CanvasBatch batch(r.service.get(), r.actions.get());
        QVERIFY(batch.start({{"action", "test-fill"}, {"inFolder", pictures(3)}, {"outFolder", path("out-c")}, {"prompt", "x"}}).value("ok").toBool());
        QTRY_VERIFY(r.jobs().size() > 0);
        batch.cancel();
        QTRY_VERIFY(batch.finished());
        QCOMPARE(batch.results().at(0).toMap().value("status").toString(), QString("cancelled"));
        QCOMPARE(batch.results().at(2).toMap().value("status").toString(), QString("cancelled"));
        QCOMPARE(batch.summary(), QString("0 pictures done, 3 cancelled"));
        QVERIFY(!QFile::exists(path("out-c/p1.png")));
        QCOMPARE(canvases(), 1); // only the one the view shows
        QCOMPARE(r.jobs().size(), 1); // the other two never started
    }

    // The menu row and the dialog in the real window.
    void theMenuRowOpensTheDialogAndItRunsABatch() {
        Rig r;
        CanvasBatch batch(r.service.get(), r.actions.get());
        r.app.canvasBatch = &batch;
        QVERIFY(r.app.open());
        QVERIFY(r.app.commands.trigger("file.canvas-view"));
        QObject *view = nullptr;
        QTRY_VERIFY((view = r.app.window->findChild<QObject *>("canvasViewDialog")) && view->property("opened").toBool());
        QTRY_VERIFY(find(r.app.window, "canvasMenuBar"));
        const auto click = [&](const QString &name) {
            const QPoint at = settled(r.app.window, name);
            QVERIFY2(at.x() >= 0, qPrintable(name + " never settled"));
            QTest::mouseClick(r.app.window, Qt::LeftButton, {}, at);
        };
        click("canvasMenuTitle-Canvas");
        click("subMenu-Automate");
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = find(r.app.window, "menuItem-canvas-view.batch")));
        QVERIFY(row->isEnabled());
        QVERIFY(!find(r.app.window, "menuPlanned-250-Run an action on a folder…"));
        click("menuItem-canvas-view.batch");
        QObject *dialog = nullptr;
        QTRY_VERIFY((dialog = r.app.window->findChild<QObject *>("canvasBatchDialog")) && dialog->property("opened").toBool());
        QVERIFY(!find(r.app.window, "canvasBatchAction-broken")); // a broken action is not offered
        QVERIFY(!find(r.app.window, "canvasBatchRun")->isEnabled());
        QVERIFY(!find(r.app.window, "canvasBatchAction-generative-fill")->isEnabled()); // its models are missing
        {
            // #281: a disabled button shows its tip on hover: why the action is off.
            QQuickItem *off = find(r.app.window, "canvasBatchAction-generative-fill");
            const QPoint at = settled(r.app.window, "canvasBatchAction-generative-fill");
            QVERIFY(at.x() >= 0);
            QTest::mouseMove(r.app.window, at);
            QTRY_VERIFY(tipShown(off));
            QVERIFY(off->property("tip").toString().startsWith("Its models are not on this machine"));
            const QVariant before = dialog->property("action");
            QTest::mouseClick(r.app.window, Qt::LeftButton, {}, at);
            QCOMPARE(dialog->property("action"), before); // the click did nothing
            QTest::mouseMove(r.app.window, QPoint(2, 2));
            QTRY_VERIFY(!tipShown(off));
        }
        click("canvasBatchAction-test-fill");
        click("canvasBatchFormat-jpg");
        // The portal pickers are the desktop's; a test sets what they would.
        dialog->setProperty("inFolder", QUrl::fromLocalFile(pictures(2)).toString());
        dialog->setProperty("outFolder", QUrl::fromLocalFile(path("out-d")).toString());
        QTRY_VERIFY(find(r.app.window, "canvasBatchInLabel")->property("text").toString().startsWith("2 pictures"));
        QVERIFY(!find(r.app.window, "canvasBatchRun")->isEnabled()); // the prompt is empty
        click("canvasBatchPrompt");
        for (const QChar c : QStringLiteral("hat"))
            QTest::keyClick(r.app.window, c.toLatin1());
        QTRY_VERIFY(find(r.app.window, "canvasBatchRun")->isEnabled());
        click("canvasBatchRun");
        QTRY_VERIFY(find(r.app.window, "canvasBatchResults"));
        QVERIFY(find(r.app.window, "canvasBatchCancel"));
        finishJob(r, 0);
        finishJob(r, 1);
        QTRY_VERIFY(batch.finished());
        QTRY_COMPARE(find(r.app.window, "canvasBatchSummary")->property("text").toString(), QString("2 pictures done"));
        QVERIFY(QFile::exists(path("out-d/p1.jpg")));
        QVERIFY(QFile::exists(path("out-d/p2.jpg")));
        click("canvasBatchClose");
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestCanvasBatchRuns)
#include "test_canvas_batch_runs.moc"
