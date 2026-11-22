// Installing, updating, removing and trusting packs (spec 6.6, 6.7, 6.8, 13.6): a person's
// answer comes only through PackPrompts, and no answer is `needs_confirmation`.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/pack_loader.h"
#include "engine/pack_manager.h"

using namespace fugue;

namespace {

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(bytes);
}

void writeJson(const QString &path, const QJsonObject &object) { write(path, QJsonDocument(object).toJson()); }

// A pack `source/<id>` holding one command node "tool"; `network` and `setup` are optional.
QString makePack(const QString &root, const QString &id, const QString &version, bool network = false,
                 const QJsonObject &setup = {}, const QString &nodeVersion = "1.0.0") {
    const QString dir = root + "/" + id;
    QJsonObject pack{{"manifest", 1}, {"id", id}, {"version", version}, {"name", "Test pack"},
                     {"nodes", QJsonArray{"tool"}}};
    if (!setup.isEmpty())
        pack["setup"] = setup;
    writeJson(dir + "/pack.json", pack);
    QJsonObject node{{"manifest", 1}, {"id", id + "/tool"}, {"version", nodeVersion}, {"name", "Tool"},
                     {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                     {"inputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
                     {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}}};
    if (network)
        node["needs"] = QJsonObject{{"network", true}, {"gpu", QJsonObject{{"api", "any"}}}};
    writeJson(dir + "/tool/node.json", node);
    write(dir + "/tool/run.sh", "#!/bin/sh\n");
    return dir;
}

bool git(const QString &repo, const QStringList &args) {
    QProcess process;
    process.setWorkingDirectory(repo);
    process.start("git", QStringList{"-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false"} + args);
    return process.waitForFinished(120000) && process.exitCode() == 0;
}

PackPrompts answering(Answer answer) {
    PackPrompts prompts;
    prompts.install = prompts.remove = prompts.trust = prompts.runSetup = [answer](const PackSummary &) { return answer; };
    return prompts;
}

} // namespace

class TestPackManager : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString packs() const { return m_dir.path() + "/packs"; }
    QString source() const { return m_dir.path() + "/source"; }
    QString trustFile() const { return m_dir.path() + "/trust.json"; }
    TrustStore &trustStore() { return m_trust; }
    TrustStore m_trust{m_dir.path() + "/trust.json"};
    void reset() {
        QDir(packs()).removeRecursively();
        QDir(source()).removeRecursively();
        QFile::remove(trustFile());
    }
    QStringList leftovers() const {
        QStringList names = QDir(packs()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        return names;
    }

private slots:
    void init() { reset(); }

    void aFolderIsCopiedAfterThePersonAgrees() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0", true);
        PackSummary shown;
        PackPrompts prompts = answering(Answer::Yes);
        prompts.install = [&](const PackSummary &pack) { shown = pack; return Answer::Yes; };
        const Result<PackAdded> added = addPack(dir, {}, packs(), trustStore(), prompts);
        QVERIFY2(added.ok(), qPrintable(added.error().message));
        QCOMPARE(shown.id, QString("io.example.tools"));
        QCOMPARE(shown.runtimeKinds, QStringList{"command"});
        QCOMPARE(shown.permissions, (QStringList{"gpu", "network"}));
        QVERIFY(QFileInfo(packs() + "/io.example.tools/tool/node.json").isFile());
        QCOMPARE(readPackSource(packs() + "/io.example.tools").kind, PackSource::Kind::Path);
        QCOMPARE(leftovers(), QStringList{"io.example.tools"}); // no staging folder stays behind
        // It loads like any pack, and is not trusted.
        Registry registry;
        QVERIFY(loadPacks(registry, {packs()}).packs.first().enabled);
        QVERIFY(!TrustStore(trustFile()).isTrusted("io.example.tools", "1.0.0"));
    }

    void withoutAPersonNothingIsInstalled() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        const Result<PackAdded> added = addPack(dir, {}, packs(), trustStore(), PackPrompts{});
        QVERIFY(!added.ok());
        QCOMPARE(added.error().code, QString("needs_confirmation"));
        QCOMPARE(addPack(dir, {}, packs(), trustStore(), answering(Answer::NoHuman)).error().code, QString("needs_confirmation"));
        QVERIFY(leftovers().isEmpty());
    }

    void sayingNoInstallsNothing() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        QCOMPARE(addPack(dir, {}, packs(), trustStore(), answering(Answer::No)).error().code, QString("cancelled"));
        QVERIFY(leftovers().isEmpty());
    }

    void aPackThatIsInstalledIsNotAddedAgain() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        QVERIFY(addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QCOMPARE(addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("pack_exists"));
        QCOMPARE(leftovers(), QStringList{"io.example.tools"});
    }

    void badPacksAreRefused() {
        write(source() + "/empty/readme.txt", "x");
        QCOMPARE(addPack(source() + "/empty", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("pack_invalid"));
        writeJson(source() + "/fake/pack.json", {{"manifest", 1}, {"id", "fugue"}, {"version", "1.0.0"}, {"name", "x"}, {"nodes", QJsonArray{}}});
        QCOMPARE(addPack(source() + "/fake", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("pack_invalid"));
        writeJson(source() + "/escape/pack.json", {{"manifest", 1}, {"id", "../evil"}, {"version", "1.0.0"}, {"name", "x"}, {"nodes", QJsonArray{}}});
        QCOMPARE(addPack(source() + "/escape", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("pack_invalid"));
        QCOMPARE(addPack(source() + "/missing", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("file_not_found"));
        QVERIFY(leftovers().isEmpty());
        QVERIFY(!QFileInfo(m_dir.path() + "/evil").exists());
    }

    void gitPacksNeedAPinAndAKnownScheme() {
        QCOMPARE(addPack("https://example.com/x.git", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("usage"));
        QCOMPARE(addPack("https://example.com/x.git", "--upload-pack=x", packs(), trustStore(), answering(Answer::Yes)).error().code, QString("usage"));
        QCOMPARE(addPack("ext::sh -c 'touch /tmp/pwned'", "v1", packs(), trustStore(), answering(Answer::Yes)).error().code, QString("file_not_found"));
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        QCOMPARE(addPack(dir, "v1", packs(), trustStore(), answering(Answer::Yes)).error().code, QString("usage"));
        QVERIFY(leftovers().isEmpty());
    }

    void aGitPackIsClonedAtATagAndPinned() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        QVERIFY(git(dir, {"init", "-q", "-b", "main"}) && git(dir, {"add", "-A"}) && git(dir, {"commit", "-q", "-m", "one"})
                && git(dir, {"tag", "v1"}));
        makePack(source(), "io.example.tools", "2.0.0");
        QVERIFY(git(dir, {"add", "-A"}) && git(dir, {"commit", "-q", "-m", "two"}) && git(dir, {"tag", "v2"}));
        const QString url = "file://" + dir;

        const Result<PackAdded> added = addPack(url, "v1", packs(), trustStore(), answering(Answer::Yes));
        QVERIFY2(added.ok(), qPrintable(added.error().message + added.error().hint));
        QCOMPARE(added->pack.version, QString("1.0.0")); // the tag, not the newest commit
        QCOMPARE(added->pack.source.ref, QString("v1"));
        QCOMPARE(added->pack.source.commit.size(), 40);
        QVERIFY(!QFileInfo(packs() + "/io.example.tools/.git").exists());

        QCOMPARE(addPack(url, "nope", packs(), trustStore(), answering(Answer::Yes)).error().code, QString("fetch_failed"));
        QCOMPARE(leftovers(), QStringList{"io.example.tools"});

        // Update to the tag v2; the same pin again changes nothing.
        Result<QList<PackAdded>> same = updatePacks("io.example.tools", {}, packs(), trustStore(), answering(Answer::Yes));
        QVERIFY(same.ok());
        QVERIFY(same->isEmpty());
        PackSummary shown;
        PackPrompts prompts = answering(Answer::Yes);
        prompts.install = [&](const PackSummary &pack) { shown = pack; return Answer::Yes; };
        const Result<QList<PackAdded>> changed = updatePacks("io.example.tools", "v2", packs(), trustStore(), prompts);
        QVERIFY2(changed.ok(), qPrintable(changed.error().message));
        QCOMPARE(changed->size(), 1);
        QCOMPARE(shown.oldVersion, QString("1.0.0"));
        QCOMPARE(shown.version, QString("2.0.0"));
        QCOMPARE(readPackSource(packs() + "/io.example.tools").ref, QString("v2"));
    }

    void updatingAFolderPackAsksAndTrustStartsOver() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        QVERIFY(addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        TrustStore trust(trustFile());
        QVERIFY(trustPack("io.example.tools", packs(), trust, answering(Answer::Yes)).ok());
        QVERIFY(trust.isTrusted("io.example.tools", "1.0.0"));

        makePack(source(), "io.example.tools", "1.1.0");
        QCOMPARE(updatePacks("io.example.tools", {}, packs(), trustStore(), answering(Answer::NoHuman)).error().code, QString("needs_confirmation"));
        QCOMPARE(summarizePack(packs() + "/io.example.tools")->version, QString("1.0.0"));
        QVERIFY(updatePacks("io.example.tools", {}, packs(), trustStore(), answering(Answer::No))->isEmpty());
        QCOMPARE(summarizePack(packs() + "/io.example.tools")->version, QString("1.0.0"));

        const Result<QList<PackAdded>> changed = updatePacks({}, {}, packs(), trustStore(), answering(Answer::Yes));
        QVERIFY(changed.ok());
        QCOMPARE(changed->size(), 1);
        QCOMPARE(summarizePack(packs() + "/io.example.tools")->version, QString("1.1.0"));
        QVERIFY(!trust.isTrusted("io.example.tools", "1.1.0")); // a new version asks again
        QCOMPARE(leftovers(), QStringList{"io.example.tools"});
    }

    void updateNeedsAnInstalledPackWithASource() {
        QCOMPARE(updatePacks("io.example.nothing", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("unknown_pack"));
        makePack(packs(), "io.example.byhand", "1.0.0"); // copied in by hand: no source recorded
        QCOMPARE(updatePacks("io.example.byhand", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("usage"));
        QVERIFY(updatePacks({}, {}, packs(), trustStore(), answering(Answer::Yes))->isEmpty());
        QCOMPARE(updatePacks("../x", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("unknown_pack"));
    }

    void setupRunsOnlyAfterTheExactCommandIsShownAndAccepted() {
        const QJsonObject setup{{"command", QJsonArray{"touch", "setup ran"}}, {"description", "Make the marker"}};
        const QString dir = makePack(source(), "io.example.tools", "1.0.0", false, setup);

        QStringList shown;
        PackPrompts prompts = answering(Answer::Yes);
        prompts.runSetup = [&](const PackSummary &pack) { shown = pack.setupCommand; return Answer::No; };
        const Result<PackAdded> declined = addPack(dir, {}, packs(), trustStore(), prompts);
        QVERIFY(declined.ok());
        QCOMPARE(shown, (QStringList{"touch", "setup ran"}));
        QVERIFY(declined->setupSkipped && !declined->setupRan);
        QVERIFY(!QFileInfo(packs() + "/io.example.tools/setup ran").exists());
        QCOMPARE(displayCommand(shown), QString("touch 'setup ran'"));

        // With no person to ask the setup, nothing is installed.
        QDir(packs()).removeRecursively();
        prompts.runSetup = nullptr;
        QCOMPARE(addPack(dir, {}, packs(), trustStore(), prompts).error().code, QString("needs_confirmation"));
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());

        QDir(packs()).removeRecursively();
        const Result<PackAdded> done = addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes));
        QVERIFY(done.ok());
        QVERIFY(done->setupRan);
        QVERIFY(QFileInfo(packs() + "/io.example.tools/setup ran").exists()); // ran inside the pack folder
    }

    void aFailingSetupIsReported() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0", false,
                                     {{"command", QJsonArray{"false"}}});
        QCOMPARE(addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("setup_failed"));
        QVERIFY(leftovers().isEmpty()); // not half installed
        QCOMPARE(addPack(makePack(source(), "io.example.gone", "1.0.0", false, {{"command", QJsonArray{"no-such-program-xyz"}}}), {},
                         packs(), trustStore(), answering(Answer::Yes)).error().code, QString("setup_failed"));
    }

    // 6.6: another major of an installed pack goes beside it, as `<id>@<major>`, and neither
    // displaces nor untrusts the other.
    void packsInstallSideBySideByMajor() {
        const QString id = "io.example.tools";
        const QString one = makePack(source() + "/one", id, "1.2.0");
        const QString two = makePack(source() + "/two", id, "2.0.0", false, {}, "2.0.0");
        QVERIFY(addPack(one, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        TrustStore trust(trustFile());
        QVERIFY(trustPack(id, packs(), trust, answering(Answer::Yes)).ok());

        const Result<PackAdded> added = addPack(two, {}, packs(), trustStore(), answering(Answer::Yes));
        QVERIFY2(added.ok(), qPrintable(added.error().message));
        QCOMPARE(QFileInfo(added->pack.folder).fileName(), QString("io.example.tools@2"));
        QVERIFY(QFileInfo(packs() + "/io.example.tools/tool/node.json").isFile()); // major 1 stayed
        QVERIFY(QFileInfo(packs() + "/io.example.tools@2/tool/node.json").isFile());
        QVERIFY(TrustStore(trustFile()).isTrusted(id, "1.2.0")); // and stayed trusted
        QVERIFY(!TrustStore(trustFile()).isTrusted(id, "2.0.0"));

        // The same major again is still "already installed".
        QCOMPARE(addPack(makePack(source() + "/three", id, "2.1.0", false, {}, "2.1.0"), {}, packs(), trustStore(), answering(Answer::Yes))
                     .error().code, QString("pack_exists"));

        // Both load, and each node major runs on the pack it came from.
        Registry registry;
        const PackReport report = loadPacks(registry, {packs()});
        QVERIFY2(!report.hasProblems(), "a pack of each major loads");
        QCOMPARE(registry.majors(id + "/tool"), (QList<int>{1, 2}));
        QCOMPARE(registry.nodeTypeAt(id + "/tool", 1)->packVersion, QString("1.2.0"));
        QCOMPARE(registry.nodeTypeAt(id + "/tool", 2)->packVersion, QString("2.0.0"));

        // Trust is given to the one named, by its folder.
        QVERIFY(trustPack("io.example.tools@2", packs(), trust, answering(Answer::Yes)).ok());
        QVERIFY(TrustStore(trustFile()).isTrusted(id, "2.0.0"));

        // Removing one leaves the other installed and trusted.
        QVERIFY(removePack("io.example.tools@2", packs(), trust, answering(Answer::Yes)).ok());
        QVERIFY(!QFileInfo(packs() + "/io.example.tools@2").exists());
        QVERIFY(QFileInfo(packs() + "/io.example.tools").isDir());
        QVERIFY(TrustStore(trustFile()).isTrusted(id, "1.2.0"));
        QVERIFY(!TrustStore(trustFile()).isTrusted(id, "2.0.0"));
    }

    // An update of a side-by-side pack stays in its own major.
    void updatingASideBySidePackKeepsItsMajor() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source() + "/one", id, "1.2.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        const QString two = makePack(source() + "/two", id, "2.0.0", false, {}, "2.0.0");
        QVERIFY(addPack(two, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        makePack(source() + "/two", id, "2.1.0", false, {}, "2.1.0");
        const Result<QList<PackAdded>> updated = updatePacks("io.example.tools@2", {}, packs(), trustStore(), answering(Answer::Yes));
        QVERIFY2(updated.ok(), qPrintable(updated.error().message));
        QCOMPARE(updated->size(), 1);
        Registry registry;
        loadPacks(registry, {packs()});
        QCOMPARE(registry.packVersions(id), (QStringList{"1.2.0", "2.1.0"}));
        // A new major does not replace an `@2` folder.
        makePack(source() + "/two", id, "3.0.0", false, {}, "3.0.0");
        QVERIFY(!updatePacks("io.example.tools@2", {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(QFileInfo(packs() + "/io.example.tools@2/tool/node.json").isFile());
    }

    void trustIsAskedAndRecordedPerVersion() {
        QVERIFY(addPack(makePack(source(), "io.example.tools", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        TrustStore trust(trustFile());
        QCOMPARE(trustPack("io.example.tools", packs(), trust, PackPrompts{}).error().code, QString("needs_confirmation"));
        QCOMPARE(trustPack("io.example.tools", packs(), trust, answering(Answer::No)).error().code, QString("cancelled"));
        QVERIFY(!trust.isTrusted("io.example.tools", "1.0.0"));
        QVERIFY(trustPack("io.example.tools", packs(), trust, answering(Answer::Yes)).ok());
        QVERIFY(trust.isTrusted("io.example.tools", "1.0.0"));
        QCOMPARE(trustPack("fugue", packs(), trust, answering(Answer::Yes)).error().code, QString("usage"));
        QCOMPARE(trustPack("io.example.nothing", packs(), trust, answering(Answer::Yes)).error().code, QString("unknown_pack"));
    }

    void removingAsksAndForgetsTrust() {
        QVERIFY(addPack(makePack(source(), "io.example.tools", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        TrustStore trust(trustFile());
        QVERIFY(trustPack("io.example.tools", packs(), trust, answering(Answer::Yes)).ok());

        QCOMPARE(removePack("io.example.tools", packs(), trust, PackPrompts{}).error().code, QString("needs_confirmation"));
        QCOMPARE(removePack("io.example.tools", packs(), trust, answering(Answer::No)).error().code, QString("cancelled"));
        QVERIFY(QFileInfo(packs() + "/io.example.tools").isDir());

        QVERIFY(removePack("io.example.tools", packs(), trust, answering(Answer::Yes)).ok());
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());
        QVERIFY(!trust.isTrusted("io.example.tools", "1.0.0"));
        QCOMPARE(removePack("io.example.tools", packs(), trust, answering(Answer::Yes)).error().code, QString("unknown_pack"));
        QCOMPARE(removePack("../source", packs(), trust, answering(Answer::Yes)).error().code, QString("unknown_pack"));
    }

    void aBrokenPackCanStillBeRemoved() {
        write(packs() + "/io.example.broken/pack.json", "{ not json");
        TrustStore trust(trustFile());
        QVERIFY(removePack("io.example.broken", packs(), trust, answering(Answer::Yes)).ok());
        QVERIFY(!QFileInfo(packs() + "/io.example.broken").exists());
    }

    void aLinkedPackFolderIsNotFollowedOnRemove() {
        const QString outside = m_dir.path() + "/outside";
        write(outside + "/keep.txt", "keep");
        QDir().mkpath(packs());
        QVERIFY(QFile::link(outside, packs() + "/io.example.linked"));
        TrustStore trust(trustFile());
        QCOMPARE(removePack("io.example.linked", packs(), trust, answering(Answer::Yes)).error().code, QString("unknown_pack"));
        QVERIFY(QFileInfo(outside + "/keep.txt").exists());
    }

    void aBranchIsNotAPin() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        QVERIFY(git(dir, {"init", "-q", "-b", "main"}) && git(dir, {"add", "-A"}) && git(dir, {"commit", "-q", "-m", "one"}));
        const Result<PackAdded> added = addPack("file://" + dir, "main", packs(), trustStore(), answering(Answer::Yes));
        QCOMPARE(added.error().code, QString("fetch_failed"));
        QVERIFY(leftovers().isEmpty());
    }

    void textThatCouldRewriteAPromptIsRefused() {
        const QJsonObject setup{{"command", QJsonArray{"sh", "-c", "evil\r\x1b[2Kuv sync"}}};
        QCOMPARE(addPack(makePack(source(), "io.example.tools", "1.0.0", false, setup), {}, packs(), trustStore(), answering(Answer::Yes)).error().code,
                 QString("pack_invalid"));
        QVERIFY(leftovers().isEmpty());
    }

    void aFolderThatHoldsThePacksFolderIsNotCopied() {
        write(m_dir.path() + "/pack.json", "{}");
        QCOMPARE(addPack(m_dir.path(), {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("pack_invalid"));
        QVERIFY(!QFileInfo(packs() + "/.staging").exists());
        QVERIFY(leftovers().isEmpty());
    }

    void anUpdateAsksForTrustAgainEvenAtTheSameVersion() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        QVERIFY(addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack("io.example.tools", packs(), trustStore(), answering(Answer::Yes)).ok());
        write(dir + "/tool/run.sh", "#!/bin/sh\necho changed\n"); // new code, same version
        QVERIFY(updatePacks("io.example.tools", {}, packs(), trustStore(), answering(Answer::Yes))->size() == 1);
        QVERIFY(!trustStore().isTrusted("io.example.tools", "1.0.0"));
    }

    void aFailingSetupOnUpdateKeepsTheOldPack() {
        const QString dir = makePack(source(), "io.example.tools", "1.0.0");
        QVERIFY(addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        makePack(source(), "io.example.tools", "1.1.0", false, {{"command", QJsonArray{"false"}}});
        QCOMPARE(updatePacks("io.example.tools", {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("setup_failed"));
        QCOMPARE(summarizePack(packs() + "/io.example.tools")->version, QString("1.0.0"));
        QCOMPARE(leftovers(), QStringList{"io.example.tools"});
    }

    void updatingAllGoesOnPastOneThatFails() {
        QVERIFY(addPack(makePack(source(), "io.example.a", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(addPack(makePack(source(), "io.example.b", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QDir(source() + "/io.example.a").removeRecursively(); // a's source is gone
        makePack(source(), "io.example.b", "1.1.0");
        const Result<QList<PackAdded>> changed = updatePacks({}, {}, packs(), trustStore(), answering(Answer::Yes));
        QVERIFY(changed.ok());
        QCOMPARE(changed->size(), 1);
        QCOMPARE(changed->first().pack.id, QString("io.example.b"));
        QVERIFY(!changed.warnings().isEmpty()); // a is reported as a warning
        QCOMPARE(updatePacks({}, "v1", packs(), trustStore(), answering(Answer::Yes)).error().code, QString("usage"));
    }

    void leftoversOfAnInterruptedInstallAreCleanedUp() {
        QDir().mkpath(packs() + "/.staging-old/work");
        const QDateTime past = QDateTime::currentDateTime().addSecs(-3600);
        QFile marker(packs() + "/.staging-old");
        QVERIFY(QFile(packs() + "/.staging-old").exists() || QFileInfo(packs() + "/.staging-old").isDir());
        QVERIFY(QProcess::execute("touch", {"-d", past.toString(Qt::ISODate), packs() + "/.staging-old"}) == 0);
        QVERIFY(addPack(makePack(source(), "io.example.tools", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QCOMPARE(leftovers(), QStringList{"io.example.tools"});
    }
};

QTEST_GUILESS_MAIN(TestPackManager)
#include "test_pack_manager.moc"
