// The apps shelf's store (#388): the catalog's apps as cards (ready, or what is missing in few words), the shelf's own order,
// choosing, and when the shelf is home. Seam: apps/app_shelf.h; no window (the wiring is test_app_shelf_window).

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "apps/app_shelf.h"

using namespace fugue;

namespace {

// A template as Workflows::templates() maps it, with the fields the shelf reads.
QVariantMap templ(const QString &id, bool app, bool ready = true, const QString &size = {}, int missing = 0, int packs = 0) {
    QVariantList absent;
    for (int i = 0; i < missing; ++i)
        absent << QVariantMap{{"name", QString("m%1.gguf").arg(i)}};
    QStringList needed;
    for (int i = 0; i < packs; ++i)
        needed << QString("pack%1").arg(i);
    return {{"id", id},         {"title", "Title " + id},    {"description", "Does " + id},     {"app", app},
            {"ready", ready},   {"downloadSize", size},      {"missingModels", absent},         {"neededPacks", needed},
            {"status", "status of " + id}, {"preview", "file:///p/" + id + ".png"}, {"local", true}, {"hosts", QStringList()},
            {"offlineHosts", QStringList()}};
}

} // namespace

class TestAppShelf : public QObject {
    Q_OBJECT

private slots:
    // Only apps are cards, and a card carries what the shelf draws: the app's words, its picture, ready, and the places.
    void onlyAppsAreCardsAndTheyCarryTheirWords() {
        const QVariantList cards = AppShelf::cardsFrom({templ("plain", false), templ("one", true)});
        QCOMPARE(cards.size(), 1);
        const QVariantMap card = cards.first().toMap();
        QCOMPARE(card.value("id").toString(), QString("one"));
        QCOMPARE(card.value("title").toString(), QString("Title one"));
        QCOMPARE(card.value("summary").toString(), QString("Does one"));
        QCOMPARE(card.value("preview").toString(), QString("file:///p/one.png"));
        QVERIFY(card.value("ready").toBool());
        QCOMPARE(card.value("needs").toString(), QString());
        QVERIFY(card.value("local").toBool());
        QVERIFY(AppShelf::cardFor(templ("plain", false)).isEmpty());
    }

    // Not ready says what the one sheet will ask for: the download size when it is known, else a count; a pack that is not
    // installed counts even when the models are here.
    void aCardSaysWhatIsMissingInFewWords() {
        const auto needs = [](const QVariantMap &t) { return AppShelf::cardFor(t).value("needs").toString(); };
        QCOMPARE(needs(templ("a", true, false, "2.1 GB", 1)), QString("Needs 2.1 GB"));
        QCOMPARE(needs(templ("b", true, false, "", 2)), QString("Needs 2 models"));
        QCOMPARE(needs(templ("d", true, false, "2.1 GB", 1, 2)), QString("Needs 2.1 GB and 2 packs"));
        const QVariantMap packOnly = AppShelf::cardFor(templ("e", true, true, "", 0, 2));
        QVERIFY(!packOnly.value("ready").toBool());
        QCOMPARE(packOnly.value("needs").toString(), QString("Needs 2 packs"));
        QCOMPARE(packOnly.value("detail").toString(), QString("status of e"));
    }

    // Ready apps first, each group in the catalog's order.
    void readyAppsComeFirst() {
        const QVariantList cards = AppShelf::cardsFrom({templ("n1", true, false, "1 GB", 1), templ("r1", true), templ("x", false),
                                                         templ("n2", true, false, "1 GB", 1), templ("r2", true)});
        QStringList ids;
        for (const QVariant &card : cards)
            ids << card.toMap().value("id").toString();
        QCOMPARE(ids, (QStringList{"r1", "r2", "n1", "n2"}));
    }

    // The catalog is read by refresh() only: a shelf nobody shows costs nothing.
    void theCatalogIsReadOnlyWhenAsked() {
        int reads = 0;
        AppShelf shelf([&] { ++reads; return QVariantList{templ("one", true)}; }, [](const QString &) { return true; });
        shelf.setLanding(true);
        shelf.dismiss();
        QCOMPARE(reads, 0);
        QVERIFY(shelf.apps().isEmpty());
        QSignalSpy changed(&shelf, &AppShelf::appsChanged);
        shelf.refresh();
        QCOMPARE(reads, 1);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(shelf.apps().size(), 1);
        shelf.refresh(); // the same cards: no change, so the grid is not made again
        QCOMPARE(reads, 2);
        QCOMPARE(changed.size(), 1);
    }

    // Choosing opens that template and says so; a failed open says nothing (the window's notice says why).
    void choosingOpensTheTemplate() {
        QStringList opened;
        bool succeed = true;
        AppShelf shelf([] { return QVariantList(); }, [&](const QString &id) { opened << id; return succeed; });
        QSignalSpy chosen(&shelf, &AppShelf::chosen);
        QVERIFY(shelf.choose("one"));
        QCOMPARE(opened, QStringList{"one"});
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.first().first().toString(), QString("one"));
        succeed = false;
        QVERIFY(!shelf.choose("two"));
        QCOMPARE(chosen.size(), 1);
    }

    // Home is: nothing open, and no "Start blank" yet. Start blank holds for the session, whatever opens or closes after.
    void homeIsNothingOpenUntilStartBlank() {
        AppShelf shelf([] { return QVariantList(); }, [](const QString &) { return true; });
        QSignalSpy home(&shelf, &AppShelf::homeChanged);
        QVERIFY(!shelf.atHome());
        shelf.setLanding(true);
        QVERIFY(shelf.atHome());
        shelf.setLanding(true);
        QCOMPARE(home.size(), 1); // no change, no signal
        shelf.setLanding(false); // a workflow was opened
        QVERIFY(!shelf.atHome());
        shelf.setLanding(true); // closed again
        QVERIFY(shelf.atHome());
        shelf.dismiss();
        QVERIFY(!shelf.atHome());
        QCOMPARE(home.size(), 4);
        shelf.setLanding(false); // landing and atHome share the signal: each change of either

        shelf.setLanding(true);
        QVERIFY(!shelf.atHome());
        QCOMPARE(home.size(), 6);
        // Show again (the bar's App segment on the empty graph Start blank gave) undoes the dismissal; without one it does nothing.
        shelf.showAgain();
        QVERIFY(shelf.atHome());
        QCOMPARE(home.size(), 7);
        shelf.showAgain();
        QCOMPARE(home.size(), 7);
    }

    // The search (#470): every word must be in the title, the summary or the kind, any case; `shown` follows live and says so only
    // when it changes.
    void theSearchNarrowsTheShelfByEveryWord() {
        QVariantMap cat = templ("cat", true);
        cat.insert("title", "Cat picker");
        cat.insert("description", "Finds the cat in a photo");
        cat.insert("appKind", "image");
        QVariantMap song = templ("song", true);
        song.insert("title", "Songwriter");
        song.insert("description", "Writes a tune");
        song.insert("appKind", "audio");
        AppShelf shelf([&] { return QVariantList{cat, song}; }, [](const QString &) { return true; });
        shelf.refresh();
        QSignalSpy shown(&shelf, &AppShelf::shownChanged);
        QCOMPARE(shelf.shown().size(), 2);
        shelf.setQuery("CAT");
        QCOMPARE(shelf.shown().size(), 1);
        QCOMPARE(shelf.shown().first().toMap().value("id").toString(), QString("cat"));
        shelf.setQuery("cat photo"); // both words, in the summary
        QCOMPARE(shelf.shown().size(), 1);
        shelf.setQuery("audio"); // the kind
        QCOMPARE(shelf.shown().first().toMap().value("id").toString(), QString("song"));
        shelf.setQuery("cat audio");
        QVERIFY(shelf.shown().isEmpty());
        QCOMPARE(shelf.apps().size(), 2); // the shelf itself keeps them all
        const int before = shown.size();
        shelf.setQuery("cat audio  "); // no change in what is shown
        QCOMPARE(shown.size(), before);
        shelf.setQuery({});
        QCOMPARE(shelf.shown().size(), 2);
    }

    // Bookmarks (#470): a pinned app comes first, in the catalog's order among the pinned, and the pins last (a file, per user).
    void aBookmarkPinsAnAppToTheTopAndIsKept() {
        QTemporaryDir dir;
        const QString file = dir.filePath("state/app-bookmarks.json");
        const auto ids = [](const QVariantList &cards) {
            QStringList out;
            for (const QVariant &card : cards)
                out << card.toMap().value("id").toString();
            return out;
        };
        const auto source = [] { return QVariantList{templ("a", true), templ("b", true), templ("c", true, false, "1 GB", 1)}; };
        {
            AppShelf shelf(source, [](const QString &) { return true; });
            shelf.setBookmarksFile(file);
            shelf.refresh();
            QCOMPARE(ids(shelf.apps()), (QStringList{"a", "b", "c"}));
            QSignalSpy changed(&shelf, &AppShelf::appsChanged);
            shelf.toggleBookmark("c"); // a waiting app, pinned, goes before the ready ones
            QCOMPARE(ids(shelf.apps()), (QStringList{"c", "a", "b"}));
            QVERIFY(shelf.apps().first().toMap().value("bookmarked").toBool());
            QVERIFY(shelf.isBookmarked("c"));
            shelf.toggleBookmark("b");
            QCOMPARE(ids(shelf.apps()), (QStringList{"b", "c", "a"})); // the catalog's order (ready first) among the pinned
            QCOMPARE(changed.size(), 2);
        }
        AppShelf again(source, [](const QString &) { return true; }); // another session of the same person
        again.setBookmarksFile(file);
        again.refresh();
        QCOMPARE(ids(again.apps()), (QStringList{"b", "c", "a"}));
        again.toggleBookmark("b");
        QVERIFY(!again.isBookmarked("b"));
        QCOMPARE(ids(again.apps()), (QStringList{"c", "a", "b"}));
        AppShelf third(source, [](const QString &) { return true; });
        third.setBookmarksFile(file);
        QVERIFY(third.isBookmarked("c") && !third.isBookmarked("b"));
        AppShelf alone(source, [](const QString &) { return true; }); // no file: the pins last the session
        alone.refresh();
        alone.toggleBookmark("a");
        QVERIFY(alone.isBookmarked("a"));
    }

    // Duplicate and Rename (#470): the shelf asks, the store of templates does it. A copy opens in the App view; Rename is for the
    // person's own apps only.
    void duplicateOpensTheCopyAndRenameIsForTheOwn() {
        QVariantMap mine = templ("mine", true);
        mine.insert("own", true);
        QVariantMap bundled = templ("bundled", true);
        QStringList asked, opened, renamed;
        AppShelf shelf([&] { return QVariantList{mine, bundled}; }, [&](const QString &id) { opened << id; return true; });
        shelf.setActions(
            [&](const QString &id, const QString &title) { asked << id + ":" + title; return id == "mine" ? QString() : id + "-copy"; },
            [&](const QString &id, const QString &title) { renamed << id + ":" + title; return true; });
        shelf.refresh();
        QSignalSpy chosen(&shelf, &AppShelf::chosen);
        QVERIFY(shelf.duplicate("bundled"));
        QCOMPARE(asked, QStringList{"bundled:Title bundled copy"});
        QCOMPARE(opened, QStringList{"bundled-copy"});
        QCOMPARE(chosen.size(), 1);
        QVERIFY(!shelf.duplicate("mine")); // the copy could not be made: nothing opens
        QVERIFY(!shelf.duplicate("nobody"));
        QCOMPARE(opened.size(), 1);

        QVERIFY(shelf.apps().first().toMap().value("canRename").toBool());
        QVERIFY(!shelf.rename("bundled", "Mine now")); // not the person's
        QVERIFY(!shelf.rename("mine", "   ")); // a blank title
        QVERIFY(shelf.rename("mine", " New name "));
        QCOMPARE(renamed, QStringList{"mine:New name"});
        QVERIFY(!shelf.rename("nobody", "x"));
    }
};

QTEST_GUILESS_MAIN(TestAppShelf)
#include "test_app_shelf.moc"
