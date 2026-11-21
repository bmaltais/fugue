// The bars and panels in the real window (Main.qml, offscreen, #129): the Bars and panels menu lists each with
// a check, H hides all the chrome and brings it back as it was, and what was chosen is kept. A menu
// bar that is not shown auto-hides (#160): a handle stays, the bar slides in over the canvas for the
// pointer, F10 and the Alt keys and slides out again.

#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"

using namespace canvas_test;

namespace {

const char *small = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [20, 100]}}}
})";

bool checked(App &app, const QString &id) { return app.commands.entry(id).value("checked").toBool(); }

QQuickItem *named(App &app, const QString &name) { return item(app.window->contentItem(), name); }

// Made and shown: an item the window drops, or hides, is not "there".
bool there(App &app, const QString &name) {
    const QQuickItem *found = named(app, name);
    return found && found->isVisible();
}

// The tabs show from the second workflow on; whether the menu allows them is its own property.
bool tabsAllowed(App &app) { return named(app, "workflowTabs")->property("allowed").toBool(); }

QQuickItem *host(App &app) { return named(app, "autoHide"); }
bool pinned(App &app) { return host(app)->property("pinned").toBool(); }
qreal slide(App &app) { return host(app)->property("slide").toDouble(); }
// A top menu's popup is open.
bool isOpen(QQuickWindow *window, const QString &name) {
    auto *popup = window->findChild<QObject *>(name);
    return popup && popup->property("visible").toBool();
}
QPoint centerOf(QQuickItem *it) { return it->mapToScene(QPointF(it->width() / 2, it->height() / 2)).toPoint(); }
// The pointer to a place of the window, then away to the middle of the canvas.
// QTest sends no move to where the last one went (also in an earlier window), so a window that has
// never had the pointer would keep its hover where Qt guessed it: move off the spot first.
void pointerTo(App &app, const QPoint &at) {
    QTest::mouseMove(app.window, at + QPoint(5, 5));
    QTest::mouseMove(app.window, at);
}
void pointerAway(App &app) { pointerTo(app, QPoint(app.window->width() / 2, app.window->height() / 2)); }

QVariantList titles(const QVariantList &menu) {
    QVariantList names;
    for (const QVariant &entry : menu)
        names << entry.toMap().value("title");
    return names;
}

} // namespace

class TestChromeWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theViewMenuListsEveryBarWithACheck() {
        App app(load(small));
        QVERIFY(app.open());
        const QVariantList view = app.commands.menu("bar").at(2).toMap().value("items").toList();
        QVERIFY(titles(view).contains(QString("&Bars and panels")));
        for (const char *id : {"view.toggle-menu-bar", "view.toggle-tabs", "view.toggle-run-toolbar", "view.toggle-selection-toolbar",
                               "view.toggle-breadcrumb", "view.toggle-run-status", "view.toggle-zoom-readout"}) {
            QVERIFY2(app.commands.contains(id), id);
            QVERIFY2(checked(app, id), id);
        }
        QVERIFY(there(app, "menuBar"));
        QVERIFY(named(app, "workflowTabs"));
        QVERIFY(there(app, "zoomPill"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aHiddenBarIsGoneAndItsCheckIsOff() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("view.toggle-tabs"));
        QTRY_VERIFY(!tabsAllowed(app));
        QVERIFY(!checked(app, "view.toggle-tabs"));
        QVERIFY(app.commands.trigger("view.toggle-zoom-readout"));
        QTRY_VERIFY(!there(app, "zoomPill"));
        QVERIFY(app.commands.trigger("view.toggle-tabs"));
        QTRY_VERIFY(tabsAllowed(app));
    }

    void hidingTheMenuBarLeavesAHandleNotNothing() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(pinned(app));
        const qreal canvasTop = app.canvas->mapToScene(QPointF(0, 0)).y();
        QVERIFY(canvasTop >= 28);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QVERIFY(!checked(app, "view.toggle-menu-bar"));
        QTRY_VERIFY(!pinned(app));
        QTRY_COMPARE(slide(app), 0.0);
        // The handle and the hot edge are drawn, 28px or more, and no notice says "F10".
        QQuickItem *handle = named(app, "autoHideHandle");
        QVERIFY(handle && handle->isVisible());
        QVERIFY(handle->width() >= 28 && handle->height() >= 28);
        QQuickItem *edge = named(app, "autoHideEdge");
        QVERIFY(edge && edge->isVisible());
        QCOMPARE(edge->width(), qreal(app.window->width()));
        QVERIFY(handle->mapToScene(QPointF(0, 0)).y() >= 0);
        QVERIFY(!named(app, "chromeHint"));
        // The bar is out of the window, and the canvas took its place: only the edge is left.
        QQuickItem *bar = named(app, "menuBar");
        QVERIFY(!bar || !bar->isVisible() || bar->mapToScene(QPointF(0, bar->height())).y() <= 0);
        QVERIFY(app.canvas->mapToScene(QPointF(0, 0)).y() < 10);
        // Checking it again pins it back.
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_VERIFY(pinned(app));
        QTRY_COMPARE(app.canvas->mapToScene(QPointF(0, 0)).y(), canvasTop);
    }

    void thePointerAtTheEdgeSlidesItInAndLeavingSlidesItOutWithoutResizingTheCanvas() {
        App app(load(small));
        QVERIFY(app.open());
        pointerAway(app);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_COMPARE(slide(app), 0.0);
        const QRectF before(app.canvas->mapToScene(QPointF(0, 0)), QSizeF(app.canvas->width(), app.canvas->height()));
        pointerTo(app, QPoint(400, 1));
        // Not at once (a short dwell), then in; the canvas keeps its size and place all along.
        bool sawBetween = false;
        QElapsedTimer clock;
        clock.start();
        while (slide(app) < 1.0 && clock.elapsed() < 3000) {
            const qreal s = slide(app);
            sawBetween = sawBetween || (s > 0.0 && s < 1.0);
            const QRectF now(app.canvas->mapToScene(QPointF(0, 0)), QSizeF(app.canvas->width(), app.canvas->height()));
            QCOMPARE(now, before);
            QTest::qWait(10);
        }
        QCOMPARE(slide(app), 1.0);
        QVERIFY2(sawBetween, "it slides, it does not jump");
        QQuickItem *bar = named(app, "menuBar");
        QVERIFY(bar && bar->isVisible());
        QCOMPARE(bar->mapToScene(QPointF(0, 0)).y(), 0.0);
        pointerAway(app);
        while (slide(app) > 0.0 && clock.elapsed() < 6000) {
            const QRectF now(app.canvas->mapToScene(QPointF(0, 0)), QSizeF(app.canvas->width(), app.canvas->height()));
            QCOMPARE(now, before);
            QTest::qWait(10);
        }
        QCOMPARE(slide(app), 0.0);
        QVERIFY(!bar->isVisible());
    }

    void aClickOnTheHandleSlidesItIn() {
        App app(load(small));
        QVERIFY(app.open());
        pointerAway(app);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_COMPARE(slide(app), 0.0);
        const QPoint at = centerOf(named(app, "autoHideHandle"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, at);
        QVERIFY(host(app)->property("revealed").toBool());
        QTRY_COMPARE(slide(app), 1.0);
    }

    void f10SlidesItInOpensTheFirstMenuAndTheMenuClosingSlidesItOut() {
        App app(load(small));
        QVERIFY(app.open());
        pointerAway(app);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_COMPARE(slide(app), 0.0);
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_F10);
        QTRY_COMPARE(slide(app), 1.0);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QVERIFY(!pinned(app)); // the remembered mode did not change
        QVERIFY(!checked(app, "view.toggle-menu-bar"));
        // The menu stays under its title while it is open, however long.
        QTest::qWait(700);
        QCOMPARE(slide(app), 1.0);
        app.key(Qt::Key_Escape); // closes the menu; the bar goes with it
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));
        QTRY_COMPARE(slide(app), 0.0);
    }

    void aCommandChosenFromTheMenuSlidesItOut() {
        App app(load(small));
        QVERIFY(app.open());
        pointerAway(app);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_COMPARE(slide(app), 0.0);
        app.canvas->forceActiveFocus();
        const int tabs = app.workflows.tabs().size();
        app.key(Qt::Key_F10);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QTest::qWait(300);
        app.key(Qt::Key_Return); // File > New
        QTRY_VERIFY(app.workflows.tabs().size() > tabs);
        QTRY_COMPARE(slide(app), 0.0);
    }

    void escapeSlidesItOutWhenNoMenuIsOpen() {
        App app(load(small));
        QVERIFY(app.open());
        pointerAway(app);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_COMPARE(slide(app), 0.0);
        pointerTo(app, QPoint(400, 1));
        QTRY_COMPARE(slide(app), 1.0);
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_Escape);
        QTRY_COMPARE(slide(app), 0.0);
        // The pointer is still on the edge: it stays out until the pointer leaves and returns.
        QTest::qWait(400);
        QCOMPARE(slide(app), 0.0);
        pointerAway(app);
        pointerTo(app, QPoint(400, 1));
        QTRY_COMPARE(slide(app), 1.0);
    }

    void altMnemonicsWorkWhileTheBarIsOut() {
        App app(load(small));
        QVERIFY(app.open());
        pointerAway(app);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_COMPARE(slide(app), 0.0);
        app.canvas->forceActiveFocus();
        QTest::keyClick(app.window, 'R', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QCOMPARE(named(app, "menuBar")->property("current").toInt(), 4);
        QTRY_COMPARE(slide(app), 1.0);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));
        QTRY_COMPARE(slide(app), 0.0);
    }

    void focusModeHidesTheHandleAndRestoresTheModeButF10StillWorks() {
        App app(load(small));
        QVERIFY(app.open());
        pointerAway(app);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_VERIFY(there(app, "autoHideHandle"));
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_H);
        QTRY_VERIFY(!there(app, "autoHideHandle"));
        QTRY_VERIFY(!there(app, "autoHideEdge"));
        QTRY_COMPARE(app.canvas->mapToScene(QPointF(0, 0)).y(), 0.0);
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_F10); // shows alone, over the canvas
        QTRY_COMPARE(slide(app), 1.0);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QCOMPARE(app.canvas->mapToScene(QPointF(0, 0)).y(), 0.0);
        app.key(Qt::Key_Escape);
        QTRY_COMPARE(slide(app), 0.0);
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_H);
        QTRY_VERIFY(there(app, "autoHideHandle")); // back as it was: auto-hide
        QVERIFY(!pinned(app));
    }

    // Pictures of the bar out (the edge and the handle), and in: skipped unless FUGUE_SHOT_DIR names a
    // folder (docs/recording-screenshots.md). Not committed; for reading.
    void picturesOfTheBarOutAndIn() {
        const QString out = qEnvironmentVariable("FUGUE_SHOT_DIR");
        if (out.isEmpty())
            QSKIP("FUGUE_SHOT_DIR names the folder for the pictures");
        App app(load(small));
        QVERIFY(app.open());
        pointerAway(app);
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_COMPARE(slide(app), 0.0);
        QTest::qWait(100);
        QVERIFY(test_support::grab(app.window).copy(0, 0, 480, 140).save(out + "/menubar-out.png"));
        // The handle under the pointer, then the bar in, then a menu open under it.
        pointerTo(app, centerOf(named(app, "autoHideHandle")));
        QTest::qWait(60);
        QVERIFY(test_support::grab(app.window).copy(0, 0, 480, 140).save(out + "/menubar-handle-hover.png"));
        QTRY_COMPARE(slide(app), 1.0);
        QTest::qWait(100);
        QVERIFY(test_support::grab(app.window).copy(0, 0, 480, 140).save(out + "/menubar-in.png"));
        app.canvas->forceActiveFocus();
        QTest::keyClick(app.window, 'E', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QTest::qWait(300);
        QVERIFY(test_support::grab(app.window).copy(0, 0, 480, 360).save(out + "/menubar-in-menu.png"));
    }

    void theModeIsKeptAsAWordAndResetShowsTheBar() {
        App app(load(small));
        QVERIFY(app.open());
        const auto stored = [&] {
            QFile file(app.dir.path() + "/chrome.json");
            return file.open(QIODevice::ReadOnly)
                       ? QJsonDocument::fromJson(file.readAll()).object().value("visible").toObject().value("menu-bar")
                       : QJsonValue();
        };
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QCOMPARE(stored().toString(), QString("auto-hide"));
        QVERIFY(app.commands.trigger("view.reset-bars"));
        QTRY_VERIFY(pinned(app));
        QCOMPARE(stored().toString(), QString("shown"));
    }

    void hKeyHidesAllTheChromeAndBringsItBackAsItWas() {
        App app(load(small));
        QVERIFY(app.open());
        app.canvas->forceActiveFocus();
        app.sidePanels.setTab("nodes");
        QTRY_VERIFY(named(app, "sidePanel"));
        QVERIFY(app.commands.trigger("view.toggle-tabs")); // already hidden: stays so
        QTRY_VERIFY(!tabsAllowed(app));
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_H);
        QTRY_VERIFY(!pinned(app));
        QTRY_VERIFY(!there(app, "autoHideHandle")); // focus mode hides the handle too
        QTRY_VERIFY(!named(app, "sidePanel"));
        QTRY_VERIFY(!there(app, "zoomPill"));
        QTRY_COMPARE(app.canvas->x(), 0.0);
        QTRY_VERIFY(named(app, "chromeHint"));
        app.key(Qt::Key_H);
        QTRY_VERIFY(pinned(app));
        QTRY_VERIFY(named(app, "sidePanel"));
        QTRY_VERIFY(there(app, "zoomPill"));
        QVERIFY(!tabsAllowed(app));
        QCOMPARE(app.sidePanels.tab(), QString("nodes"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aPanelOpenedInFocusModeShowsAlone() {
        App app(load(small));
        QVERIFY(app.open());
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_H);
        QTRY_VERIFY(!pinned(app));
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_W);
        QTRY_VERIFY(named(app, "workflowsPanel"));
        QVERIFY(!pinned(app));
    }

    void whatWasChosenIsKept() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("view.toggle-breadcrumb"));
        QVERIFY(app.commands.trigger("view.focus-mode")); // not kept
        QFile file(app.dir.path() + "/chrome.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject visible = QJsonDocument::fromJson(file.readAll()).object().value("visible").toObject();
        QCOMPARE(visible.value("breadcrumb").toBool(true), false);
        QVERIFY(!visible.contains("menu-bar") || visible.value("menu-bar").toString() == "shown");
    }

    void resetBringsEverythingBack() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QVERIFY(app.commands.trigger("view.toggle-tabs"));
        QTRY_VERIFY(!pinned(app));
        QVERIFY(app.commands.trigger("view.reset-bars"));
        QTRY_VERIFY(pinned(app));
        QTRY_VERIFY(tabsAllowed(app));
    }
};

QTEST_MAIN(TestChromeWindow)
#include "test_chrome_window.moc"
