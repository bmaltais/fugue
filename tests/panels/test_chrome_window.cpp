// The bars and panels in the real window (Main.qml, offscreen, #129): the Bars and panels menu lists each with
// a check, H hides all the chrome and brings it back as it was, a hidden menu bar comes back with
// F10 and says so, and what was chosen is kept.

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

    void theMenuBarComesBackWithF10AndSaysSoWhenHidden() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QTRY_VERIFY(!there(app, "menuBar"));
        QTRY_VERIFY(named(app, "chromeHint"));
        QVERIFY(named(app, "chromeHint")->property("text").toString().contains("F10"));
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_F10);
        QTRY_VERIFY(there(app, "menuBar"));
        QVERIFY(checked(app, "view.toggle-menu-bar"));
        QTRY_VERIFY(!named(app, "chromeHint")); // the hint goes by itself
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
        QTRY_VERIFY(!there(app, "menuBar"));
        QTRY_VERIFY(!named(app, "sidePanel"));
        QTRY_VERIFY(!there(app, "zoomPill"));
        QTRY_COMPARE(app.canvas->x(), 0.0);
        QTRY_VERIFY(named(app, "chromeHint"));
        app.key(Qt::Key_H);
        QTRY_VERIFY(there(app, "menuBar"));
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
        QTRY_VERIFY(!there(app, "menuBar"));
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_W);
        QTRY_VERIFY(named(app, "workflowsPanel"));
        QVERIFY(!there(app, "menuBar"));
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
        QVERIFY(!visible.contains("menu-bar") || visible.value("menu-bar").toBool());
    }

    void resetBringsEverythingBack() {
        App app(load(small));
        QVERIFY(app.open());
        QVERIFY(app.commands.trigger("view.toggle-menu-bar"));
        QVERIFY(app.commands.trigger("view.toggle-tabs"));
        QTRY_VERIFY(!there(app, "menuBar"));
        QVERIFY(app.commands.trigger("view.reset-bars"));
        QTRY_VERIFY(there(app, "menuBar"));
        QTRY_VERIFY(tabsAllowed(app));
    }
};

QTEST_MAIN(TestChromeWindow)
#include "test_chrome_window.moc"
