// The apps shelf's store (#388): the catalog's apps as cards (ready, or what is missing in few words), the shelf's own order,
// choosing, and when the shelf is home. Seam: apps/app_shelf.h; no window (the wiring is test_app_shelf_window).

#include <QSignalSpy>
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
        shelf.setLanding(false);
        shelf.setLanding(true);
        QVERIFY(!shelf.atHome());
        QCOMPARE(home.size(), 4);
    }
};

QTEST_GUILESS_MAIN(TestAppShelf)
#include "test_app_shelf.moc"
