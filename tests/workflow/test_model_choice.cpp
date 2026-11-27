// Model choice (workflow/model_choice.h) at its own interface: a fake owner and two fake destinations, no window, no QML, no worker thread.
// The test plays the worker: a read waits in `world.reads` until the test finishes it. Each case here is one that the review-fix rounds
// of "Use my file" found (commit ids in the comments); the real graph and App view are covered by test_templates and test_app_view.

#include <QSignalSpy>
#include <QtTest>

#include "../fake_app_destination.h"
#include "workflow/model_choice.h"

using namespace fugue;

namespace {

const QString hashA = QStringLiteral("sha256:") + QString(64, u'a');
const QString hashB = QStringLiteral("sha256:") + QString(64, u'b');

OnDiskUse useFor(const QString &hash, const QString &name = QStringLiteral("model.safetensors")) {
    OnDiskUse use;
    use.wanted = QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", name}};
    use.match.name = QStringLiteral("mine-") + name;
    use.match.path = QStringLiteral("/models/mine-") + name;
    use.match.tier = QStringLiteral("Q8_0");
    return use;
}

// The owner, as far as the module sees it.
struct World {
    QString shown = QStringLiteral("t1");
    QStringList open{QStringLiteral("t1")};
    bool sheetOpen = false, sheetReview = false, autoOn = true;
    QList<OnDiskUse> covered;
    QByteArray key = "k";
    QString notice;
    QStringList sheetSaid;
    int changes = 0, forced = 0, offers = 0, ended = 0;
    struct Read {
        QString path;
        std::function<void(int)> progress;
        std::function<void(const QString &)> done;
    };
    QList<Read> reads;

    ChoiceHost host() {
        ChoiceHost h;
        h.facts = [this] {
            ChoiceFacts facts;
            facts.shownTab = shown;
            facts.openTabs = open;
            facts.sheetOpen = sheetOpen;
            facts.sheetReview = sheetReview;
            return facts;
        };
        h.autoOn = [this] { return autoOn; };
        h.modelsKey = [this] { return key; };
        h.covered = [this] { return covered; };
        h.hash = [this](const QString &path, std::function<void(int)> progress, std::function<void(const QString &)> done) {
            reads.append({path, std::move(progress), std::move(done)});
        };
        h.say = [this](const QString &text) { notice = text; };
        h.saying = [this] { return notice; };
        h.sayInSheet = [this](const QString &text) { sheetSaid << text; };
        h.changed = [this](bool force) {
            ++changes;
            forced += force;
        };
        h.endReading = [this] {
            notice.clear();
            ++ended;
        };
        h.offerSheet = [this] { ++offers; };
        h.later = [](std::function<void()> what) { what(); };
        return h;
    }
    // The worker reads the oldest file: its hash is "sha256:" + 64 of `digit`, or none (a file that cannot be read).
    void finishRead(const QString &hash = QStringLiteral("sha256:") + QString(64, u'1')) {
        QVERIFY(!reads.isEmpty());
        const Read read = reads.takeFirst();
        read.done(hash);
    }
};

// A destination that remembers what it took.
class FakeGraph : public ChoiceDestination {
public:
    explicit FakeGraph(World &world) : m_world(world) {}
    int takes = 0;
    QStringList tabs;
    Take result = Take::Applied;
    int undoIndex = 0;
    int reverted = 0;
    bool shown(const QString &tab) const override { return m_world.shown == tab; }
    Uses inForce(const QString &) const override { return {}; }
    Take take(const QString &tab, const QList<OnDiskUse> &, const Uses &, bool, ChoiceRecord *record) override {
        if (m_world.shown != tab)
            return Take::Refused;
        if (result == Take::Applied) {
            ++takes;
            tabs << tab;
            *record = {tab, undoIndex, {}};
        }
        return result;
    }
    ChoiceRecord took(const QString &tab, const Document *) const override { return {tab, undoIndex, {}}; }
    bool holds(const ChoiceRecord &record) const override { return record.tab == m_world.shown && record.stamp == undoIndex; }
    bool revert(const ChoiceRecord &) override {
        ++reverted;
        return true;
    }
    bool undoInput(const QString &, const QString &) override { return false; }

private:
    World &m_world;
};

struct Rig {
    World world;
    FakeGraph graph{world};
    int appTaken = 0;
    bool appShown = false;
    FakeAppDestination app{[this] { return appShown; }, appTaken};
    ModelChoice choice{world.host(), graph};
    Rig() {
        choice.setApp(&app);
        world.covered = {useFor(hashA)};
    }
};

} // namespace

class TestModelChoice : public QObject {
    Q_OBJECT

private slots:
    // One read per target, shared by the asks; each ask is answered with its own token (16ed98fd, 79343fb7).
    void twoAsksForTheSameFilesShareOneReadAndTakeItOnce() {
        Rig rig;
        QSignalSpy finished(&rig.choice, &ModelChoice::finished);
        rig.choice.ask({hashA}, 3);
        rig.choice.ask({hashA}, 0); // a second press during the read
        QCOMPARE(rig.world.reads.size(), 1);
        QVERIFY(rig.choice.hashing());
        QCOMPARE(rig.choice.reading().value(hashA).toInt(), -1);
        rig.world.finishRead();
        QCOMPARE(rig.graph.takes, 1); // the same destination takes the result once
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.at(0).at(2).toInt(), 3);
        QCOMPARE(finished.at(0).at(0).toStringList(), QStringList{hashA});
        QCOMPARE(finished.at(1).at(2).toInt(), 0);
        QVERIFY(!rig.choice.hashing());
        QVERIFY(rig.choice.reading().isEmpty());
    }

    // Cancel is per request: another surface's read of the same files goes on (79343fb7); the last one cancelled drops the result.
    void cancelByTokenKeepsTheOthersAsk() {
        Rig rig;
        QSignalSpy finished(&rig.choice, &ModelChoice::finished);
        rig.choice.ask({hashA}, 7);
        rig.choice.ask({hashA}, 0);
        rig.choice.cancel(7);
        rig.world.finishRead();
        QCOMPARE(rig.graph.takes, 1);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(2).toInt(), 0);

        rig.choice.ask({hashA}, 8);
        rig.choice.cancel(8); // nobody wants it any more
        rig.world.finishRead();
        QCOMPARE(rig.graph.takes, 1);
        QCOMPARE(finished.size(), 1);
    }

    // The destination is the view the person looked at when the ask was made, not when the file is read (7356726c, 2cdcd042); a cancelled
    // read asked again in the App view is the App view's.
    void theDestinationIsFixedWhenTheAskIsMade() {
        Rig rig;
        rig.choice.ask({hashA}, 1);
        rig.appShown = true; // the person goes to the App view during the read
        rig.world.finishRead();
        QCOMPARE(rig.graph.takes, 1);
        QCOMPARE(rig.appTaken, 0);

        rig.choice.ask({hashA}, 7); // in the App view
        rig.choice.cancel(7);
        rig.choice.ask({hashA}, 8);
        rig.appShown = false;
        rig.world.finishRead();
        QCOMPARE(rig.appTaken, 1);
        QCOMPARE(rig.graph.takes, 1);
    }

    // A person's choice for a tab that is not shown is not taken when the other tab's document is on the canvas (graph) (2cdcd042).
    void aGraphChoiceForAnotherTabIsNotTakenAndIsSaid() {
        Rig rig;
        rig.world.open << QStringLiteral("t2");
        QSignalSpy finished(&rig.choice, &ModelChoice::finished);
        rig.choice.ask({hashA}, 1);
        rig.world.shown = QStringLiteral("t2");
        rig.world.finishRead();
        QCOMPARE(rig.graph.takes, 0);
        QCOMPARE(finished.first().at(1).toStringList(), QStringList{hashA});
        QVERIFY(rig.world.notice.contains(QStringLiteral("tab changed")));
    }

    // A file that cannot be read is said, and the ask is answered as not read (5770df99).
    void aFileThatCannotBeReadIsSaid() {
        Rig rig;
        QSignalSpy finished(&rig.choice, &ModelChoice::finished);
        rig.choice.ask({hashA}, 4);
        rig.world.finishRead(QString());
        QCOMPARE(rig.graph.takes, 0);
        QVERIFY(rig.world.notice.startsWith(QStringLiteral("Could not read mine-model.safetensors")));
        QCOMPARE(finished.first().at(1).toStringList(), QStringList{hashA});
        rig.world.sheetReview = true; // the sheet covers the notice: the failure is its own line
        rig.choice.ask({hashA}, 5);
        rig.world.finishRead(QString());
        QCOMPARE(rig.world.sheetSaid.size(), 1);
    }

    // One automatic swap per model; the graph and the App view are tried separately; a read of the other view is not idle (84338522).
    void oneAutomaticSwapPerModelAcrossViews() {
        Rig rig;
        QVERIFY(rig.choice.check(ModelChoice::Check::ModelsChanged));
        QCOMPARE(rig.world.reads.size(), 1);
        rig.appShown = true; // the other view, while the first read goes on
        QVERIFY(!rig.choice.check(ModelChoice::Check::ModelsChanged));
        QCOMPARE(rig.world.reads.size(), 1);
        rig.world.finishRead();
        QCOMPARE(rig.graph.takes, 1);
        QVERIFY(rig.world.notice.startsWith(QStringLiteral("Used your file")));
        // The graph was tried; the App view still has its turn, and then nothing is left.
        QVERIFY(rig.choice.check(ModelChoice::Check::ModelsChanged));
        rig.world.finishRead();
        QCOMPARE(rig.appTaken, 1);
        QVERIFY(!rig.choice.check(ModelChoice::Check::ModelsChanged));
    }

    // An idle tab is not walked again until its models change (the cheap check), and the setting off or a sheet open stops it.
    void anIdleTabIsNotCheckedAgain() {
        Rig rig;
        rig.world.covered.clear();
        QVERIFY(!rig.choice.check(ModelChoice::Check::TabShown));
        rig.world.covered = {useFor(hashA)};
        QVERIFY(!rig.choice.check(ModelChoice::Check::TabShown)); // same models: idle
        rig.world.key = "other";
        rig.world.autoOn = false;
        QVERIFY(!rig.choice.check(ModelChoice::Check::TabShown));
        rig.world.autoOn = true;
        rig.world.sheetOpen = true;
        QVERIFY(!rig.choice.check(ModelChoice::Check::TabShown));
        rig.world.sheetOpen = false;
        QVERIFY(rig.choice.check(ModelChoice::Check::TabShown));
    }

    // A read that ends on a tab that is not shown is dropped and tried again when the tab is shown, in either view (12757c73, 5770df99).
    void aReadEndingOnAnotherTabIsRetriedWhenTheTabIsShown() {
        for (const bool inApp : {false, true}) {
            Rig rig;
            rig.world.open << QStringLiteral("t2");
            rig.appShown = inApp;
            QVERIFY(rig.choice.check(ModelChoice::Check::ModelsChanged));
            rig.world.shown = QStringLiteral("t2"); // a tab switch during the read
            rig.world.finishRead();
            QCOMPARE(rig.graph.takes + rig.appTaken, 0);
            QVERIFY(rig.world.notice.isEmpty()); // not a failure
            rig.world.shown = QStringLiteral("t1");
            QVERIFY(rig.choice.check(ModelChoice::Check::TabShown));
            rig.world.finishRead();
            QCOMPARE(rig.graph.takes + rig.appTaken, 1);
        }
    }

    // A Get ready the person opened during the read wins: nothing is swapped, and both views are marked tried (84338522, 5770df99).
    void aSheetOpenedMeanwhileKeepsThePersonsChoice() {
        Rig rig;
        QVERIFY(rig.choice.check(ModelChoice::Check::ModelsChanged));
        rig.world.sheetOpen = true;
        rig.world.finishRead();
        QCOMPARE(rig.graph.takes, 0);
        rig.world.sheetOpen = false;
        QVERIFY(!rig.choice.check(ModelChoice::Check::ModelsChanged)); // not asked again behind the person's choice
        rig.appShown = true;
        QVERIFY(!rig.choice.check(ModelChoice::Check::ModelsChanged)); // nor in the other view
    }

    // An automatic swap on inputs the person already changed says nothing (5770df99).
    void anEditedInputIsNotAnnounced() {
        Rig rig;
        rig.graph.result = ChoiceDestination::Take::Unchanged;
        QVERIFY(rig.choice.check(ModelChoice::Check::ModelsChanged));
        rig.world.finishRead();
        QVERIFY(rig.world.notice.isEmpty());
        QCOMPARE(rig.world.changes, 0);
    }

    // The banner's Undo: only the graph has one; it holds for its tab until the tab is edited or the line is replaced (79343fb7).
    void theBannersUndoBelongsToTheGraphAndItsTab() {
        Rig rig;
        rig.world.open << QStringLiteral("t2");
        QVERIFY(rig.choice.check(ModelChoice::Check::ModelsChanged));
        rig.world.finishRead();
        QVERIFY(rig.choice.hasRecord());
        QVERIFY(rig.choice.undoable());
        rig.graph.undoIndex = 1; // the person edited the tab
        QVERIFY(!rig.choice.undoable());
        rig.graph.undoIndex = 0;
        rig.world.notice.clear(); // the line is gone
        QVERIFY(!rig.choice.undoable());
        rig.world.notice = QStringLiteral("x");
        rig.choice.undo();
        QCOMPARE(rig.graph.reverted, 1);
        QCOMPARE(rig.world.forced, 1);
        QVERIFY(!rig.choice.hasRecord());
    }

    // The latest-open ticket: a template whose files are read opens only if it is still the last open asked for; its reading line goes
    // with it, by its exact text, and a line another caller said stays (ca20fe69, 4af188a7).
    void theOpenAskedForLastWins() {
        Rig rig;
        int opened = 0;
        QList<bool> answers;
        const auto openIt = [&opened](const Uses &) {
            ++opened;
            return true;
        };
        rig.choice.openAsked(QStringLiteral("open:A"));
        QVERIFY(rig.choice.openWith(QStringLiteral("open:A"), {useFor(hashA)}, openIt, QString(), [&answers](bool done) { answers << done; }));
        QCOMPARE(rig.world.reads.size(), 1);
        rig.world.reads.first().progress(40);
        QVERIFY(rig.world.notice.startsWith(QStringLiteral("Reading your file")));
        rig.choice.openAsked(QStringLiteral("open:B")); // another template meanwhile
        rig.world.finishRead();
        QCOMPARE(opened, 0);
        QCOMPARE(answers, QList<bool>{false});
        QVERIFY(rig.world.notice.isEmpty());
        QVERIFY(!rig.choice.hashing());

        // A, B, then A again opens A; a second click during the read starts no second read.
        rig.choice.openAsked(QStringLiteral("open:A"));
        QVERIFY(rig.choice.openWith(QStringLiteral("open:A"), {useFor(hashA)}, openIt, QString(), [&answers](bool done) { answers << done; }));
        rig.choice.openAsked(QStringLiteral("open:A"));
        QVERIFY(rig.choice.openWith(QStringLiteral("open:A"), {useFor(hashA)}, openIt, QString(), {}));
        QCOMPARE(rig.world.reads.size(), 1);
        rig.world.finishRead();
        QCOMPARE(opened, 1);
        QCOMPARE(answers, (QList<bool>{false, true}));

        // A line somebody else said is not taken back.
        rig.choice.openAsked(QStringLiteral("open:C"));
        rig.choice.openWith(QStringLiteral("open:C"), {useFor(hashA)}, openIt, QString(), {});
        rig.world.reads.first().progress(10);
        rig.choice.openAsked(QStringLiteral("path:x"));
        rig.world.notice = QStringLiteral("Could not open x.");
        rig.world.finishRead();
        QCOMPARE(rig.world.notice, QStringLiteral("Could not open x."));
    }

    // A template opened with the files marks the tab tried in both views, says the file used, and keeps an Undo (84338522, 79343fb7).
    void anOpenWithFilesCoversBothViewsAndHasItsUndo() {
        Rig rig;
        rig.choice.openAsked(QStringLiteral("open:A"));
        bool checkedWhileOpening = true;
        rig.choice.openWith(
            QStringLiteral("open:A"), {useFor(hashA)},
            [&](const Uses &) {
                checkedWhileOpening = rig.choice.check(ModelChoice::Check::ModelsChanged); // not while the template opens
                return true;
            },
            QString(), {});
        rig.world.finishRead();
        QVERIFY(!checkedWhileOpening);
        QVERIFY(rig.world.notice.startsWith(QStringLiteral("Opened with your file")));
        QVERIFY(rig.choice.undoable());
        rig.world.covered = {useFor(hashA)};
        QVERIFY(!rig.choice.check(ModelChoice::Check::ModelsChanged));
        rig.appShown = true;
        QVERIFY(!rig.choice.check(ModelChoice::Check::ModelsChanged));
    }

    // A file that could not be read: the template opens without it, the line says so, and it is not tried again (79343fb7, 5770df99).
    void anOpenWithAFileThatCannotBeReadSaysSoAndIsNotTriedAgain() {
        Rig rig;
        rig.choice.openAsked(QStringLiteral("open:A"));
        bool done = false;
        rig.choice.openWith(QStringLiteral("open:A"), {useFor(hashA)}, [](const Uses &) { return true; }, QString(), [&done](bool ok) { done = ok; });
        rig.world.finishRead(QString());
        QVERIFY(done);
        QVERIFY(rig.world.notice.startsWith(QStringLiteral("Opened without")));
        QVERIFY(!rig.choice.hasRecord());
        QVERIFY(!rig.choice.check(ModelChoice::Check::ModelsChanged));
    }

    // A tab opened by the setting asks for the Get ready sheet when its read ends, for what is left; a switch never does (5770df99).
    void onlyATabJustOpenedOffersTheSheetWhenItsReadEnds() {
        Rig rig;
        QVERIFY(rig.choice.check(ModelChoice::Check::Offer));
        rig.world.finishRead();
        QCOMPARE(rig.world.offers, 1);

        Rig other;
        QVERIFY(other.choice.check(ModelChoice::Check::TabShown));
        other.world.finishRead();
        QCOMPARE(other.world.offers, 0);

        Rig later; // a read of the tab is going, started elsewhere: the sheet waits for it
        QVERIFY(later.choice.check(ModelChoice::Check::ModelsChanged));
        QVERIFY(later.choice.offerAfterRead());
        later.world.finishRead();
        QCOMPARE(later.world.offers, 1);
    }

    // A closed tab forgets its marks: it is tried again if its id ever comes back (the tab's id never does, but its marks must not leak).
    void aClosedTabForgetsItsMarks() {
        Rig rig;
        QVERIFY(rig.choice.check(ModelChoice::Check::ModelsChanged));
        rig.world.finishRead();
        QVERIFY(!rig.choice.check(ModelChoice::Check::ModelsChanged));
        rig.choice.tabClosed(QStringLiteral("t1"));
        QVERIFY(rig.choice.check(ModelChoice::Check::ModelsChanged));
    }

    // A template opened for download is not used by either view.
    void aDeclinedChoiceIsNeverAppliedByItself() {
        Rig rig;
        rig.choice.declined(QStringLiteral("t1"), {hashA});
        QVERIFY(!rig.choice.check(ModelChoice::Check::Offer));
        rig.appShown = true;
        QVERIFY(!rig.choice.check(ModelChoice::Check::Offer));
    }

    // With no tab, or nothing covered, an ask answers at once as not read.
    void anAskWithNothingToUseAnswersAtOnce() {
        Rig rig;
        QSignalSpy finished(&rig.choice, &ModelChoice::finished);
        rig.world.shown.clear();
        rig.choice.ask({hashA}, 2);
        rig.world.shown = QStringLiteral("t1");
        rig.world.covered.clear();
        rig.choice.ask({hashA}, 3);
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.at(0).at(1).toStringList(), QStringList{hashA});
        QVERIFY(rig.world.reads.isEmpty());
    }
};

QTEST_MAIN(TestModelChoice)
#include "test_model_choice.moc"
