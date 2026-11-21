// The registry of bars and panels (#129): what is shown, what focus mode hides and brings back,
// and what is kept between runs. No window, no QML: the logic is all here.
//
// Seam: panels/chrome_visibility.h.

#include <QTemporaryDir>
#include <QtTest>

#include "panels/chrome_visibility.h"

using namespace fugue;

class TestChromeVisibility : public QObject {
    Q_OBJECT

    static void write(const QString &path, const QByteArray &json) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(json);
    }
    static QJsonObject read(const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    }

private slots:
    void anElementStartsWithItsDefault() {
        ChromeVisibility chrome({});
        chrome.add("bar");
        chrome.add("panel", false);
        QVERIFY(chrome.isVisible("bar"));
        QVERIFY(!chrome.isVisible("panel"));
        QVERIFY(!chrome.isVisible("unknown"));
        QCOMPARE(chrome.ids(), QStringList({"bar", "panel"}));
    }

    void showHideAndToggleTellOnlyAboutChanges() {
        ChromeVisibility chrome({});
        chrome.add("bar");
        QSignalSpy changed(&chrome, &ChromeVisibility::visibilityChanged);
        chrome.setVisible("bar", true); // already so
        QCOMPARE(changed.count(), 0);
        chrome.toggle("bar");
        QVERIFY(!chrome.isVisible("bar"));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.at(0).at(0).toString(), QString("bar"));
        QCOMPARE(changed.at(0).at(1).toBool(), false);
        chrome.setVisible("nothing", false); // not registered: nothing happens
        QCOMPARE(changed.count(), 1);
    }

    void shownIsAMapOfWhatIsVisible() {
        ChromeVisibility chrome({});
        chrome.add("a");
        chrome.add("b", false);
        QCOMPARE(chrome.shown().value("a").toBool(), true);
        QCOMPARE(chrome.shown().value("b").toBool(), false);
    }

    void whatWasChosenIsKeptAndReadBack() {
        QTemporaryDir dir;
        const QString path = dir.filePath("bars.json");
        {
            ChromeVisibility chrome(path);
            chrome.add("a");
            chrome.add("b", false);
            chrome.setVisible("a", false);
            chrome.setVisible("b", true);
        }
        ChromeVisibility again(path);
        again.add("a");
        again.add("b", false);
        QVERIFY(!again.isVisible("a"));
        QVERIFY(again.isVisible("b"));
    }

    void anAutoHideBarKeepsAModeWordAndReadsTheOlderBool() {
        QTemporaryDir dir;
        const QString path = dir.filePath("bars.json");
        // The older file: hidden is auto-hide, shown is shown.
        write(path, R"({"visible": {"menu-bar": false, "other": false}})");
        {
            ChromeVisibility chrome(path);
            chrome.add("menu-bar");
            chrome.setAutoHide("menu-bar");
            chrome.add("other");
            QVERIFY(!chrome.isVisible("menu-bar"));
            QCOMPARE(chrome.mode("menu-bar"), QString("auto-hide"));
            chrome.setVisible("menu-bar", true);
            QCOMPARE(chrome.mode("menu-bar"), QString("shown"));
            chrome.setVisible("menu-bar", false);
        }
        // Written as words for the auto-hide bar, bools for the rest.
        const QJsonObject saved = read(path).value("visible").toObject();
        QCOMPARE(saved.value("menu-bar").toString(), QString("auto-hide"));
        QVERIFY(saved.value("other").isBool());
        ChromeVisibility again(path);
        again.add("menu-bar");
        again.setAutoHide("menu-bar");
        QVERIFY(!again.isVisible("menu-bar"));
        // A word an older build wrote as true, and "shown", both read as shown; reset writes "shown".
        write(path, R"({"visible": {"menu-bar": "shown"}})");
        ChromeVisibility shown(path);
        shown.add("menu-bar");
        shown.setAutoHide("menu-bar");
        QVERIFY(shown.isVisible("menu-bar"));
        shown.setVisible("menu-bar", false);
        shown.reset();
        QCOMPARE(read(path).value("visible").toObject().value("menu-bar").toString(), QString("shown"));
    }

    void anElementNotRegisteredThisTimeKeepsItsChoice() {
        QTemporaryDir dir;
        const QString path = dir.filePath("bars.json");
        write(path, R"({"visible": {"later": false}})");
        {
            ChromeVisibility chrome(path);
            chrome.add("a");
            chrome.setVisible("a", false);
        }
        QCOMPARE(read(path).value("visible").toObject().value("later").toBool(true), false);
        ChromeVisibility again(path);
        again.add("later");
        QVERIFY(!again.isVisible("later"));
    }

    void aDamagedFileGivesTheDefaults() {
        QTemporaryDir dir;
        const QString path = dir.filePath("bars.json");
        write(path, R"({"visible": {"a": 3, "b": "no", "c": false}, "other": 1})");
        ChromeVisibility chrome(path);
        chrome.add("a");
        chrome.add("b", false);
        chrome.add("c");
        QVERIFY(chrome.isVisible("a"));
        QVERIFY(!chrome.isVisible("b"));
        QVERIFY(!chrome.isVisible("c"));
        write(path, "not json");
        ChromeVisibility broken(path);
        broken.add("a");
        QVERIFY(broken.isVisible("a"));
    }

    void aSeedFromAnOlderFileCountsOnlyWhenNothingIsStored() {
        QTemporaryDir dir;
        const QString path = dir.filePath("bars.json");
        {
            ChromeVisibility chrome(path);
            chrome.add("a");
            chrome.seed("a", false); // an older file said hidden
            QVERIFY(!chrome.isVisible("a"));
            chrome.setVisible("a", true);
        }
        ChromeVisibility again(path);
        again.add("a");
        again.seed("a", false); // the older file is still there; ours wins
        QVERIFY(again.isVisible("a"));
    }

    void oneOfAnExclusiveGroupIsShownAtATime() {
        ChromeVisibility chrome({});
        chrome.add("nodes", false);
        chrome.add("workflows", false);
        chrome.setExclusive({"nodes", "workflows"});
        chrome.setVisible("nodes", true);
        chrome.setVisible("workflows", true);
        QVERIFY(!chrome.isVisible("nodes"));
        QVERIFY(chrome.isVisible("workflows"));
        chrome.setVisible("workflows", false);
        QVERIFY(!chrome.isVisible("nodes"));
        QVERIFY(!chrome.isVisible("workflows"));
    }

    void aGroupFoundWithTwoShownKeepsTheFirst() {
        ChromeVisibility chrome({});
        chrome.add("nodes");
        chrome.add("workflows");
        chrome.setExclusive({"nodes", "workflows"});
        QVERIFY(chrome.isVisible("nodes"));
        QVERIFY(!chrome.isVisible("workflows"));
    }

    void focusModeHidesEverythingAndRestoresExactlyWhatWasShown() {
        ChromeVisibility chrome({});
        chrome.add("a");
        chrome.add("b");
        chrome.add("hidden", false);
        QSignalSpy changed(&chrome, &ChromeVisibility::visibilityChanged);
        chrome.setFocusMode(true);
        QVERIFY(chrome.focusMode());
        QVERIFY(!chrome.isVisible("a"));
        QVERIFY(!chrome.isVisible("b"));
        QCOMPARE(changed.count(), 2); // "hidden" was not shown, so it did not change
        QVERIFY(chrome.wanted("a"));  // the choice is untouched
        chrome.setFocusMode(false);
        QVERIFY(chrome.isVisible("a"));
        QVERIFY(chrome.isVisible("b"));
        QVERIFY(!chrome.isVisible("hidden"));
        QCOMPARE(changed.count(), 4);
    }

    void focusModeToggles() {
        ChromeVisibility chrome({});
        chrome.add("a");
        chrome.toggleFocusMode();
        QVERIFY(!chrome.isVisible("a"));
        chrome.toggleFocusMode();
        QVERIFY(chrome.isVisible("a"));
    }

    void anElementAskedForInFocusModeShowsAloneAndTheRestStaysAway() {
        ChromeVisibility chrome({});
        chrome.add("menu");
        chrome.add("panel", false);
        chrome.setFocusMode(true);
        chrome.setVisible("panel", true); // opened on purpose
        QVERIFY(chrome.isVisible("panel"));
        QVERIFY(!chrome.isVisible("menu"));
        chrome.setFocusMode(false);
        QVERIFY(chrome.isVisible("panel")); // it stays, as it was opened
        QVERIFY(chrome.isVisible("menu"));
    }

    void anElementHiddenInFocusModeStaysHiddenAfter() {
        ChromeVisibility chrome({});
        chrome.add("a");
        chrome.add("b");
        chrome.setFocusMode(true);
        chrome.setVisible("a", true);
        chrome.setVisible("a", false);
        chrome.setFocusMode(false);
        QVERIFY(!chrome.isVisible("a"));
        QVERIFY(chrome.isVisible("b"));
    }

    void focusModeIsNotKept() {
        QTemporaryDir dir;
        const QString path = dir.filePath("bars.json");
        {
            ChromeVisibility chrome(path);
            chrome.add("a");
            chrome.setFocusMode(true);
        }
        ChromeVisibility again(path);
        again.add("a");
        QVERIFY(!again.focusMode());
        QVERIFY(again.isVisible("a")); // nothing is lost to a window closed in focus mode
    }

    void resetBringsBackTheDefaultsAndLeavesFocusMode() {
        ChromeVisibility chrome({});
        chrome.add("a");
        chrome.add("b", false);
        chrome.setVisible("a", false);
        chrome.setVisible("b", true);
        chrome.setFocusMode(true);
        chrome.reset();
        QVERIFY(!chrome.focusMode());
        QVERIFY(chrome.isVisible("a"));
        QVERIFY(!chrome.isVisible("b"));
    }

    void hidingTellsWhatWentAway() {
        ChromeVisibility chrome({});
        chrome.add("menu");
        QSignalSpy hidden(&chrome, &ChromeVisibility::hiddenByRequest);
        chrome.setVisible("menu", false);
        QCOMPARE(hidden.count(), 1);
        QCOMPARE(hidden.at(0).at(0).toString(), QString("menu"));
        chrome.setVisible("menu", true);
        chrome.setFocusMode(true); // focus mode has its own notice
        QCOMPARE(hidden.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestChromeVisibility)
#include "test_chrome_visibility.moc"
