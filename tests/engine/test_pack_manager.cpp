// Installing, updating, removing and trusting packs (spec 6.6, 6.7, 6.8, 13.6): a person's
// answer comes only through PackPrompts, and no answer is `needs_confirmation`.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <unistd.h>

#include "engine/pack_loader.h"
#include "engine/pack_manager.h"
#include "engine/registry.h"
#include "engine/sandbox.h"

using namespace fugue;

namespace {

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(bytes);
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
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
    void initTestCase() {
        // These tests run real setup commands; the sandbox itself is tested in test_sandbox.
        if (!sandbox::available())
            sandbox::runSetupUnsandboxedForTest(true);
    }
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
        const QJsonObject setup{{"command", QJsonArray{"touch", ".venv/setup ran"}}, {"description", "Make the marker"}};
        const QString dir = makePack(source(), "io.example.tools", "1.0.0", false, setup);

        QStringList shown;
        PackPrompts prompts = answering(Answer::Yes);
        prompts.runSetup = [&](const PackSummary &pack) { shown = pack.setupCommand; return Answer::No; };
        const Result<PackAdded> declined = addPack(dir, {}, packs(), trustStore(), prompts);
        QVERIFY(declined.ok());
        QCOMPARE(shown, (QStringList{"touch", ".venv/setup ran"}));
        QVERIFY(declined->setupSkipped && !declined->setupRan);
        QVERIFY(!QFileInfo(packs() + "/io.example.tools/.venv/setup ran").exists());
        QCOMPARE(displayCommand(shown), QString("touch '.venv/setup ran'"));

        // With no person to ask the setup, nothing is installed.
        QDir(packs()).removeRecursively();
        prompts.runSetup = nullptr;
        QCOMPARE(addPack(dir, {}, packs(), trustStore(), prompts).error().code, QString("needs_confirmation"));
        QVERIFY(!QFileInfo(packs() + "/io.example.tools").exists());

        QDir(packs()).removeRecursively();
        const Result<PackAdded> done = addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes));
        QVERIFY(done.ok());
        QVERIFY(done->setupRan);
        QVERIFY(QFileInfo(packs() + "/io.example.tools/.venv/setup ran").exists()); // ran inside the pack folder
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

    // #497: trust covers the secrets a pack declared when it was said yes to, and where it came from. A pack.json edited in
    // place to read others, or a changed `.fugue-source.json` (the key the values are stored under), asks again once the
    // loader has read it; the new yes covers the new set. The digest is the one of the pack the loader parsed, so a decoy
    // folder of the same name does not keep trust. Uses the session's own packs folder (the test's temporary home).
    void aPackJsonEditedInPlaceToReadSecretsAsksAgain() {
        const QString id = "io.example.keyed";
        const QString folder = packsDir() + "/" + id;
        QDir(packsDir()).removeRecursively();
        const QString dir = makePack(source(), id, "1.0.0");
        QVERIFY(addPack(dir, {}, packsDir(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packsDir(), trustStore(), answering(Answer::Yes)).ok());
        const auto load = [&] { Registry registry; loadPacks(registry, {packsDir()}); };
        load();
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));

        // The same file again: still trusted.
        QJsonObject pack = QJsonDocument::fromJson(readAll(folder + "/pack.json")).object();
        writeJson(folder + "/pack.json", pack);
        load();
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
        // A decoy: another folder with the old pack.json, which the loader rejects. It does not stand in for the real one.
        QDir().mkpath(packsDir() + "/" + id + "@1");
        QVERIFY(QFile::copy(folder + "/pack.json", packsDir() + "/" + id + "@1/pack.json"));
        // A new secret asks again.
        pack["settings"] = QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}}};
        writeJson(folder + "/pack.json", pack);
        load();
        QVERIFY(!trustStore().isTrusted(id, "1.0.0"));
        QVERIFY(trustPack(id, packsDir(), trustStore(), answering(Answer::Yes)).ok());
        load();
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
        // A label edit does not matter; the rule of where a value may go does.
        pack["settings"] = QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}, {"label", "Key"}}};
        writeJson(folder + "/pack.json", pack);
        load();
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
        pack["settings"] = QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}, {"origin", "required"}}};
        writeJson(folder + "/pack.json", pack);
        load();
        QVERIFY(!trustStore().isTrusted(id, "1.0.0"));
        QVERIFY(trustPack(id, packsDir(), trustStore(), answering(Answer::Yes)).ok());
        load();
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
        // Where the pack came from decides the key its secrets are stored under: a changed record asks again.
        write(folder + "/.fugue-source.json", R"({"kind": "path", "location": "/somewhere/else"})");
        load();
        QVERIFY(!trustStore().isTrusted(id, "1.0.0"));
        QDir(packsDir()).removeRecursively();
    }

    // A pack trusted before digests were kept is trusted on first sight when the loader reads it, so a later edit asks.
    void aTrustedPackWithNoDigestGetsOneWhenLoaded() {
        const QString id = "io.example.old";
        const QString folder = packsDir() + "/" + id;
        QDir(packsDir()).removeRecursively();
        makePack(packsDir(), id, "1.0.0");
        // A yes the way an older Fugue wrote it: the version, no digest.
        write(TrustStore::defaultPath(), QByteArray(R"({"packs": {"io.example.old": ["1.0.0"]}})"));
        TrustStore trust; // the session's own file, which the loader keeps the digest in
        QVERIFY(trust.isTrusted(id, "1.0.0"));
        Registry first;
        loadPacks(first, {packsDir()});
        QVERIFY(readAll(TrustStore::defaultPath()).contains("secrets")); // the digest is kept now
        QVERIFY(trust.isTrusted(id, "1.0.0"));
        QJsonObject pack = QJsonDocument::fromJson(readAll(folder + "/pack.json")).object();
        pack["settings"] = QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}}};
        writeJson(folder + "/pack.json", pack);
        Registry second;
        loadPacks(second, {packsDir()});
        QVERIFY(!trust.isTrusted(id, "1.0.0"));
        QDir(packsDir()).removeRecursively();
        QFile::remove(TrustStore::defaultPath());
    }

    // #521: declining the setup question of an update used to swap in the fresh copy (no environment) and delete
    // the old folder with its environment. The old pack now stays as it was.
    void decliningTheSetupOfAnUpdateKeepsTheOldPackAndItsEnvironment() {
        const QJsonObject setup{{"command", QJsonArray{"sh", "-c", "mkdir -p .venv && touch .venv/ok"}}};
        const QString dir = makePack(source(), "io.example.tools", "1.0.0", false, setup);
        QVERIFY(addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(QFileInfo(packs() + "/io.example.tools/.venv/ok").isFile());
        QVERIFY(trustPack("io.example.tools", packs(), trustStore(), answering(Answer::Yes)).ok());

        makePack(source(), "io.example.tools", "1.0.1", false, setup);
        PackPrompts prompts = answering(Answer::Yes);
        prompts.runSetup = [](const PackSummary &) { return Answer::No; };
        QCOMPARE(updatePacks("io.example.tools", {}, packs(), trustStore(), prompts).error().code, QString("setup_declined"));
        QCOMPARE(summarizePack(packs() + "/io.example.tools")->version, QString("1.0.0"));
        QVERIFY(QFileInfo(packs() + "/io.example.tools/.venv/ok").isFile());
        QVERIFY(trustStore().isTrusted("io.example.tools", "1.0.0")); // nothing changed, so trust stays
        QCOMPARE(leftovers(), QStringList{"io.example.tools"});

        // Updating every pack reports it as a warning and goes on.
        const Result<QList<PackAdded>> all = updatePacks({}, {}, packs(), trustStore(), prompts);
        QVERIFY(all.ok());
        QVERIFY(all->isEmpty());
        QCOMPARE(all.warnings().size(), 1);
        QVERIFY(QFileInfo(packs() + "/io.example.tools/.venv/ok").isFile());

        // Accepting the setup updates it and the environment is made again.
        QCOMPARE(updatePacks("io.example.tools", {}, packs(), trustStore(), answering(Answer::Yes))->size(), 1);
        QCOMPARE(summarizePack(packs() + "/io.example.tools")->version, QString("1.0.1"));
        QVERIFY(QFileInfo(packs() + "/io.example.tools/.venv/ok").isFile());
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
        QVERIFY(QFileInfo(packs() + "/.staging-old").isDir());
        QVERIFY(QProcess::execute("touch", {"-d", past.toString(Qt::ISODate), packs() + "/.staging-old"}) == 0);
        QVERIFY(addPack(makePack(source(), "io.example.tools", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QCOMPARE(leftovers(), QStringList{"io.example.tools"});
    }
    // #418: a stop kills the setup, and that is a stop, not a failed setup; nothing of the pack stays.
    void aStoppedSetupIsCancelledAndLeavesNothing() {
        // The setup runs in the sandbox, which writes only the pack's own folder: the marker is made there.
        const QString marker = packs() + "/io.example.tools/.venv/setup-started";
        const QJsonObject setup{{"command", QJsonArray{"sh", "-c", "touch .venv/setup-started; sleep 30"}}};
        PackPrompts prompts = answering(Answer::Yes);
        prompts.cancelled = [&] { return QFileInfo::exists(marker); };
        const Result<PackAdded> added = addPack(makePack(source(), "io.example.tools", "1.0.0", false, setup), {}, packs(), trustStore(), prompts);
        QVERIFY(!added.ok());
        QCOMPARE(added.error().code, QString("cancelled"));
        QVERIFY(leftovers().isEmpty());
        // An update that is stopped puts the old version back.
        QFile::remove(marker);
        QVERIFY(addPack(makePack(source(), "io.example.tools", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        makePack(source(), "io.example.tools", "1.0.1", false, setup);
        const Result<QList<PackAdded>> updated = updatePacks("io.example.tools", {}, packs(), trustStore(), prompts);
        QCOMPARE(updated.error().code, QString("cancelled"));
        QCOMPARE(summarizePack(packs() + "/io.example.tools")->version, QString("1.0.0"));
        QCOMPARE(leftovers(), QStringList{"io.example.tools"});
    }

    // The pid no process has: the kernel's limit is never handed out.
    static qint64 deadPid() { return QString::fromUtf8(readAll("/proc/sys/kernel/pid_max")).trimmed().toLongLong(); }
    // An owner as a folder name carries it: <pid>-<start ticks>-<boot id> (#673). A pid nobody has gets any ticks.
    static QString bootId() { return QString::fromUtf8(readAll("/proc/sys/kernel/random/boot_id")).trimmed().remove('-'); }
    static qint64 ticksOf(qint64 pid) {
        const QByteArray stat = readAll(QString("/proc/%1/stat").arg(pid).toUtf8());
        return stat.mid(stat.lastIndexOf(')') + 2).split(' ').value(19).toLongLong();
    }
    static QString ownerOf(qint64 pid) { return QString("%1-%2-%3").arg(pid).arg(ticksOf(pid)).arg(bootId()); }
    static QString self() { return ownerOf(QCoreApplication::applicationPid()); }
    static QString gonePid() { return QString("%1-7-%2").arg(deadPid()).arg(bootId()); }
    static void makeOld(const QString &dir) { QVERIFY(QProcess::execute("touch", {"-d", "2 hours ago", dir}) == 0); }

    // #418: a staging folder goes by its owner's pid, not by time: a live owner's slow clone stays, a dead one's goes, and a
    // link is never followed.
    void staleFoldersAreAgedByTheirOwner() {
        QVERIFY(deadPid() > 0);
        const QString mine = ".staging-" + self() + "-aaaaaaaaaaaa";
        const QString gone = ".staging-" + gonePid() + "-bbbbbbbbbbbb";
        const QString trash = ".trash-" + gonePid() + "-cccccccccccc"; // a failed delete's leftover goes like a staging folder
        for (const QString &name : {mine, gone, trash, QString(".staging-legacy")}) {
            QDir().mkpath(packs() + "/" + name + "/work");
            makeOld(packs() + "/" + name);
        }
        QTemporaryDir outside;
        write(outside.path() + "/keep", "x");
        QVERIFY(QFile::link(outside.path(), packs() + "/.staging-" + gonePid() + "-dddddddddddd"));
        QVERIFY(addPack(makePack(source(), "io.example.tools", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(QFileInfo(packs() + "/" + mine + "/work").isDir()); // alive, however old
        QVERIFY(!QFileInfo::exists(packs() + "/" + gone));
        QVERIFY(!QFileInfo::exists(packs() + "/" + trash));
        QVERIFY(!QFileInfo::exists(packs() + "/.staging-legacy")); // no pid to ask: by time, as before
        QVERIFY(QFileInfo(outside.path() + "/keep").isFile()); // the link's target is untouched
    }

    // #418: an update killed mid-way leaves the old pack in `.old-<pid>-<random>-<folder>`: the sweep puts it back, or deletes it
    // only when the new copy is whole; a live owner's backup is kept whatever its age.
    void aDeadOwnersBackupComesBackUnlessTheNewCopyIsWhole() {
        const QString id = "io.example.tools";
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QString dead = QString(".old-%1-%2-aaaaaaaaaaaa-%3").arg(gonePid()).arg(now - 100).arg(id);
        const QString live = QString(".old-%1-%2-bbbbbbbbbbbb-%3").arg(self()).arg(now).arg(id);
        const QJsonObject setup{{"command", QJsonArray{"true"}}};
        const auto sweep = [&] { // any stage sweeps
            const QString other = makePack(source(), "io.example.other", "1.0.0");
            QVERIFY(addPack(other, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
            QVERIFY(removePack("io.example.other", packs(), trustStore(), answering(Answer::Yes)).ok());
        };
        // The target is missing: the old pack comes back.
        makePack(packs(), dead, "1.0.0"); // the folder is named dead; the pack inside says id
        QDir(packs() + "/" + dead).removeRecursively();
        makePack(source(), id, "1.0.0");
        QVERIFY(QDir().rename(source() + "/" + id, packs() + "/" + dead));
        makeOld(packs() + "/" + dead);
        sweep();
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.0.0"));
        QVERIFY(!QFileInfo::exists(packs() + "/" + dead));
        // The target is a new copy whose setup never finished (no marker): the old pack replaces it.
        QDir(packs() + "/" + id).removeRecursively();
        makePack(packs(), id, "1.0.0");
        QVERIFY(QDir().rename(packs() + "/" + id, packs() + "/" + dead));
        makePack(packs(), id, "1.1.0", false, setup);
        sweep();
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.0.0"));
        QVERIFY(QDir(packs()).entryList({".staging-*"}, QDir::Dirs | QDir::Hidden).isEmpty()); // the half-made folder is gone
        // The target is whole (no setup): the backup is deleted.
        QDir(packs() + "/" + id).removeRecursively();
        makePack(packs(), id, "1.1.0");
        makePack(packs(), dead, "1.0.0");
        sweep();
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.1.0"));
        QVERIFY(!QFileInfo::exists(packs() + "/" + dead));
        // A live owner's backup stays, however old its folder is.
        makePack(packs(), live, "1.0.0");
        makeOld(packs() + "/" + live);
        sweep();
        QVERIFY(QFileInfo(packs() + "/" + live + "/pack.json").isFile());
    }

    // #418: the half-made folder is moved aside, not deleted first: one that cannot be deleted (a 0555 folder a setup made) no
    // longer costs the old pack.
    void anUpdateStoppedWithALockedFolderStillGetsTheOldPackBack() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        if (geteuid() == 0)
            QSKIP("root can delete a read-only folder");
        const QString marker = packs() + "/" + id + "/.venv/setup-started"; // the sandbox writes only the pack's folder
        const QJsonObject setup{{"command", QJsonArray{"sh", "-c", "mkdir .venv/d && touch .venv/d/f && chmod 555 .venv/d && touch .venv/setup-started && sleep 30"}}};
        makePack(source(), id, "1.0.1", false, setup);
        PackPrompts prompts = answering(Answer::Yes);
        prompts.cancelled = [&] { return QFileInfo::exists(marker); };
        QCOMPARE(updatePacks(id, {}, packs(), trustStore(), prompts).error().code, QString("cancelled"));
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.0.0"));
        QVERIFY(!QFileInfo::exists(packs() + "/" + id + "/d"));
        QProcess::execute("chmod", {"-R", "u+w", packs()}); // so the scratch folder can go
    }

    // #418: a link at the target is never followed or deleted through: the old pack is kept as `.rescue-*`.
    void aLinkAtTheTargetIsNeverFollowedBySweep() {
        const QString id = "io.example.tools";
        QTemporaryDir outside;
        write(outside.path() + "/keep.txt", "dev work");
        QDir().mkpath(packs());
        QVERIFY(QFile::link(outside.path(), packs() + "/" + id));
        const QString dead = QString(".old-%1-%2-aaaaaaaaaaaa-%3").arg(gonePid()).arg(QDateTime::currentSecsSinceEpoch() - 100).arg(id);
        makePack(packs(), dead, "1.0.0");
        QVERIFY(addPack(makePack(source(), "io.example.other", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(QFileInfo(outside.path() + "/keep.txt").isFile());
        QVERIFY(QFileInfo(packs() + "/" + id).isSymLink());
        QVERIFY(!QFileInfo::exists(packs() + "/" + dead));
        QVERIFY(!QDir(packs()).entryList({".rescue-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
    }

    // #418: two dead backups of one folder: the oldest comes back, and the other goes once the target is whole.
    void ofTwoDeadBackupsTheOldestComesBack() {
        const QString id = "io.example.tools";
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        // The folder is named dead, the pack inside says id (makePack(packs(), dead) would write the folder name as its id, which is no
        // pack id: summarizePack failed and `->version` read what the stack held, "1.0.0" in nearly every run).
        const auto backup = [&](const QString &name, const QString &version) {
            QVERIFY(QDir().mkpath(packs()));
            QVERIFY(QDir().rename(makePack(source(), id, version), packs() + "/" + name));
        };
        backup(QString(".old-%1-%2-aaaaaaaaaaaa-%3").arg(gonePid()).arg(now - 300).arg(id), "1.0.0");
        backup(QString(".old-%1-%2-bbbbbbbbbbbb-%3").arg(gonePid()).arg(now - 100).arg(id), "1.5.0");
        QVERIFY(addPack(makePack(source(), "io.example.other", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        const Result<PackSummary> restored = summarizePack(packs() + "/" + id);
        QVERIFY2(restored.ok(), qPrintable(restored.ok() ? QString() : restored.error().message));
        QCOMPARE(restored->version, QString("1.0.0"));
        QVERIFY(QDir(packs()).entryList({".old-*"}, QDir::Dirs | QDir::Hidden).isEmpty()); // the newer one goes once the target is whole
    }

    // #418: a pack removed after a crash does not come back from its dead backup.
    void aRemovedPackDoesNotComeBackFromABackup() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        makePack(packs(), QString(".old-%1-%2-aaaaaaaaaaaa-%3").arg(gonePid()).arg(QDateTime::currentSecsSinceEpoch() - 100).arg(id), "0.9.0");
        QVERIFY(removePack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(addPack(makePack(source(), id, "1.1.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.1.0"));
    }

    // #418: a failed update of the same version puts its trust back when the old pack is back; a changed version loses none.
    void aFailedUpdateKeepsTheTrustOfTheOldPack() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
        makePack(source(), id, "1.0.0", false, {{"command", QJsonArray{"false"}}}); // same version, new setup that fails
        QCOMPARE(updatePacks(id, {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("setup_failed"));
        QVERIFY(trustStore().isTrusted(id, "1.0.0")); // before anything loads the pack again
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.0.0"));
    }

    // #673: a pid that was handed out again is told by its start ticks, a pid from before a reboot by the boot id; no clock is read.
    void aReusedPidIsNotTheOwner() {
        QProcess sleeper; // a live process the folders claim to belong to
        sleeper.start("sleep", {"30"});
        QVERIFY(sleeper.waitForStarted());
        const qint64 pid = sleeper.processId();
        const qint64 ticks = ticksOf(pid);
        QVERIFY(ticks > 0);
        const QString same = QString(".staging-%1-%2-%3-aaaaaaaaaaaa").arg(pid).arg(ticks).arg(bootId());
        const QString reused = QString(".staging-%1-%2-%3-bbbbbbbbbbbb").arg(pid).arg(ticks - 1).arg(bootId()); // started earlier than this process
        const QString rebooted = QString(".staging-%1-%2-%3-cccccccccccc").arg(pid).arg(ticks).arg(QString(32, 'f'));
        const QString oldBackup = QString(".old-%1-%2-%3-%4-dddddddddddd-io.example.tools").arg(pid).arg(ticks - 1).arg(bootId()).arg(QDateTime::currentSecsSinceEpoch());
        for (const QString &name : {same, reused, rebooted})
            QDir().mkpath(packs() + "/" + name + "/work");
        QVERIFY(QDir().rename(makePack(source(), "io.example.tools", "1.0.0"), packs() + "/" + oldBackup));
        QVERIFY(addPack(makePack(source(), "io.example.other", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        sleeper.kill();
        sleeper.waitForFinished();
        QVERIFY(QFileInfo(packs() + "/" + same + "/work").isDir()); // the very process: kept
        QVERIFY(!QFileInfo::exists(packs() + "/" + reused));
        QVERIFY(!QFileInfo::exists(packs() + "/" + rebooted));
        QCOMPARE(summarizePack(packs() + "/io.example.tools")->version, QString("1.0.0")); // the reused pid's backup was put back
    }

    // #673: a backup whose delete failed half way is renamed away before the delete, so it never looks like the oldest backup.
    void aBackupThatCannotBeDeletedIsNeverTakenForABackupAgain() {
        if (geteuid() == 0)
            QSKIP("root can delete a read-only folder");
        const QString id = "io.example.tools";
        const QString dir = makePack(source(), id, "1.0.0");
        QVERIFY(addPack(dir, {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        write(packs() + "/" + id + "/locked/f", "x");
        QVERIFY(QFile::setPermissions(packs() + "/" + id + "/locked", QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        write(dir + "/tool/run.sh", "#!/bin/sh\necho changed\n");
        QCOMPARE(updatePacks(id, {}, packs(), trustStore(), answering(Answer::Yes))->size(), 1);
        QVERIFY(QDir(packs()).entryList({".old-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
        QCOMPARE(QDir(packs()).entryList({".trash-*"}, QDir::Dirs | QDir::Hidden).size(), 1); // swept once this command is gone
        QProcess::execute("chmod", {"-R", "u+w", packs()}); // so the scratch folder can go
    }

    // #673: a backup with no readable pack.json is no pack: it is not put over the target, and is kept as `.rescue-`.
    void aBackupWithoutAPackJsonIsNotRestored() {
        const QString id = "io.example.tools";
        QDir().mkpath(packs() + "/." + "old-" + gonePid() + "-" + QString::number(QDateTime::currentSecsSinceEpoch()) + "-aaaaaaaaaaaa-" + id + "/half");
        QVERIFY(addPack(makePack(source(), "io.example.other", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(!QFileInfo::exists(packs() + "/" + id));
        QVERIFY(!QDir(packs()).entryList({".rescue-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
    }

    // #673: the trust of a changed-version update's old pack is untouched by a failed setup, and the new version gets none.
    void aFailedUpdateToAnotherVersionLosesNoTrust() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        makePack(source(), id, "1.1.0", false, {{"command", QJsonArray{"false"}}});
        QCOMPARE(updatePacks(id, {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("setup_failed"));
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
        QVERIFY(!trustStore().isTrusted(id, "1.1.0"));
    }

    // #673: a pack untrusted between the question and the swap stays untrusted when the same-version update fails.
    void anUntrustedPackStaysUntrustedAfterAFailedUpdate() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        makePack(source(), id, "1.0.0", false, {{"command", QJsonArray{"false"}}});
        PackPrompts prompts = answering(Answer::Yes);
        prompts.runSetup = [&](const PackSummary &) { // the person untrusts it while the question is open
            trustStore().revoke(id, "1.0.0");
            return Answer::Yes;
        };
        QCOMPARE(updatePacks(id, {}, packs(), trustStore(), prompts).error().code, QString("setup_failed"));
        QVERIFY(!trustStore().isListed(id, "1.0.0"));
        // And one that was never trusted stays so.
        QVERIFY(!trustStore().isTrusted(id, "1.0.0"));
    }

    // #673: what is put back is the digest the yes recorded, not one computed from the folder now: a secrets edit made between
    // the yes and the update still asks again.
    void aFailedUpdatePutsBackTheRecordedDigest() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        const QString folder = packs() + "/" + id;
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        const QString recorded = trustStore().sealedDigest(id, "1.0.0");
        QVERIFY(!recorded.isEmpty() || packSecretsDigestOfFolder(folder).isEmpty());
        QJsonObject pack = QJsonDocument::fromJson(readAll(folder + "/pack.json")).object();
        pack["settings"] = QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}}};
        writeJson(folder + "/pack.json", pack); // edited in place after the yes
        QVERIFY(packSecretsDigestOfFolder(folder) != recorded);
        makePack(source(), id, "1.0.0", false, {{"command", QJsonArray{"false"}}});
        QCOMPARE(updatePacks(id, {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("setup_failed"));
        QVERIFY(trustStore().isListed(id, "1.0.0"));
        QCOMPARE(trustStore().sealedDigest(id, "1.0.0"), recorded);
    }

    // #673: an update that cannot even set the old pack aside has changed nothing, trust included.
    void aFailedFirstMoveKeepsTheTrust() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        write(source() + "/" + id + "/tool/run.sh", "#!/bin/sh\necho changed\n");
        PackPrompts prompts = answering(Answer::Yes);
        bool asked = false; // cancelled() is also asked before staging, which must still work
        prompts.install = [&](const PackSummary &) { asked = true; return Answer::Yes; };
        prompts.cancelled = [&] { // the first time after the question: under the lock, just before the first move
            if (asked)
                QFile::setPermissions(packs(), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
            return false;
        };
        if (geteuid() != 0) // the lock is let go while the folder is still read-only
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Could not remove our own lock file"));
        const Result<QList<PackAdded>> updated = updatePacks(id, {}, packs(), trustStore(), prompts);
        QFile::setPermissions(packs(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        if (geteuid() == 0)
            QSKIP("root can write a read-only folder");
        QCOMPARE(updated.error().code, QString("permission_denied"));
        QVERIFY2(updated.error().message.startsWith("Cannot replace"), qPrintable(updated.error().message)); // not the staging
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
        QCOMPARE(readAll(packs() + "/" + id + "/tool/run.sh"), QByteArray("#!/bin/sh\n"));
    }

    // An update puts back the pack it set aside even when that pack's pack.json is broken: only the sweep's dead backups are
    // checked for one (a delete that failed half way).
    void aFailedUpdateOfABrokenPackPutsItBack() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        write(packs() + "/" + id + "/pack.json", "{ broken");
        makePack(source(), id, "1.1.0", false, {{"command", QJsonArray{"false"}}});
        QCOMPARE(updatePacks(id, {}, packs(), trustStore(), answering(Answer::Yes)).error().code, QString("setup_failed"));
        QCOMPARE(readAll(packs() + "/" + id + "/pack.json"), QByteArray("{ broken"));
        QCOMPARE(leftovers(), QStringList{id});
    }

    // Spec 6.8: a trust that cannot be taken back (trust.json cannot be written) stops a same-version update before its new code
    // is in place; the old pack and its trust stay.
    void anUpdateWhoseTrustCannotBeTakenBackChangesNothing() {
        if (geteuid() == 0)
            QSKIP("root can write a read-only folder");
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        write(source() + "/" + id + "/tool/run.sh", "#!/bin/sh\necho changed\n"); // new code, same version
        QVERIFY(QFile::setPermissions(m_dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner)); // trust.json's folder
        const Result<QList<PackAdded>> updated = updatePacks(id, {}, packs(), trustStore(), answering(Answer::Yes));
        QFile::setPermissions(m_dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QVERIFY(!updated.ok());
        QCOMPARE(updated.error().code, QString("permission_denied"));
        QCOMPARE(readAll(packs() + "/" + id + "/tool/run.sh"), QByteArray("#!/bin/sh\n"));
        QCOMPARE(leftovers(), QStringList{id});
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
    }

    // The previous build's names (#418, before #673): its `.old-<pid>-<epoch>-<random>-<folder>` is a dead owner's whatever the
    // pid (that build swaps under the same lock), and its `.staging-<pid>-<epoch>-<random>` goes by its pid and start time, never by age.
    void thePreviousBuildsLeftoversAreSettled() {
        const QString id = "io.example.tools";
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 me = QCoreApplication::applicationPid();
        QVERIFY(QDir().mkpath(packs()));
        QVERIFY(QDir().rename(makePack(source(), id, "1.0.0"), packs() + QString("/.old-%1-%2-aaaaaaaaaaaa-%3").arg(me).arg(now - 100).arg(id)));
        const QString live = QString(".staging-%1-%2-bbbbbbbbbbbb").arg(me).arg(now);
        const QString gone = QString(".staging-%1-%2-cccccccccccc").arg(deadPid()).arg(now);
        const QString reused = QString(".staging-%1-%2-dddddddddddd").arg(me).arg(now - 10 * 365 * 86400); // made before this process
        for (const QString &name : {live, gone, reused}) {
            QDir().mkpath(packs() + "/" + name + "/work");
            makeOld(packs() + "/" + name);
        }
        QVERIFY(addPack(makePack(source(), "io.example.other", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.0.0"));
        QVERIFY(QDir(packs()).entryList({".old-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
        QVERIFY(QFileInfo(packs() + "/" + live + "/work").isDir()); // a live owner's, two hours old
        QVERIFY(!QFileInfo::exists(packs() + "/" + gone));
        QVERIFY(!QFileInfo::exists(packs() + "/" + reused));
    }

    // An owner that could not read its own start ticks or boot id wrote 0 for them: it is not known, so a live one's staging
    // folder is not swept (its clone runs without the lock); a pid no process has is gone all the same.
    void anOwnerWrittenWithoutTicksOrBootIsNotTakenForGone() {
        QProcess sleeper;
        sleeper.start("sleep", {"30"});
        QVERIFY(sleeper.waitForStarted());
        const qint64 pid = sleeper.processId();
        const QString noTicks = QString(".staging-%1-0-%2-aaaaaaaaaaaa").arg(pid).arg(bootId());
        const QString noBoot = QString(".staging-%1-%2-0-bbbbbbbbbbbb").arg(pid).arg(ticksOf(pid));
        const QString gone = QString(".staging-%1-0-0-cccccccccccc").arg(deadPid());
        for (const QString &name : {noTicks, noBoot, gone})
            QDir().mkpath(packs() + "/" + name + "/work");
        QVERIFY(addPack(makePack(source(), "io.example.other", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        sleeper.kill();
        sleeper.waitForFinished();
        QVERIFY(QFileInfo(packs() + "/" + noTicks + "/work").isDir());
        QVERIFY(QFileInfo(packs() + "/" + noBoot + "/work").isDir());
        QVERIFY(!QFileInfo::exists(packs() + "/" + gone));
    }

    // A yes given while a same-version update's setup runs (Settings, another command: trust.json is not under the packs lock) is
    // newer than the one the update took away: a failed update leaves it as it is, digest included.
    void aYesGivenDuringAFailedUpdateIsKept() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        makePack(source(), id, "1.0.0", false, {{"command", QJsonArray{"sh", "-c", "echo trust-now; exit 1"}}});
        PackPrompts prompts = answering(Answer::Yes);
        bool given = false;
        prompts.log = [&](const QString &line) {
            if (line == "trust-now")
                given = !hasErrors(TrustStore(trustFile()).trust(id, "1.0.0", "feedface"));
        };
        QCOMPARE(updatePacks(id, {}, packs(), trustStore(), prompts).error().code, QString("setup_failed"));
        QVERIFY(given);
        QVERIFY(trustStore().isListed(id, "1.0.0"));
        QCOMPARE(trustStore().sealedDigest(id, "1.0.0"), QString("feedface"));
    }

    // A staged copy is only looked at: summarizing it does not replace the digest trust compares the installed pack of that id
    // and version with (it has no source record, or one it carries), so a declined update leaves the trusted pack trusted.
    void aDeclinedUpdateLeavesTheInstalledPackTrusted() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        write(source() + "/" + id + "/tool/run.sh", "#!/bin/sh\necho changed\n"); // new code, same version
        PackPrompts prompts = answering(Answer::Yes);
        prompts.install = [](const PackSummary &) { return Answer::No; };
        const Result<QList<PackAdded>> updated = updatePacks(id, {}, packs(), trustStore(), prompts);
        QVERIFY(updated.ok() && updated->isEmpty());
        QVERIFY(trustStore().isTrusted(id, "1.0.0"));
    }

    // Nor does a staged copy adopt its digest into the session's trust file for a yes given before digests were kept: only the
    // installed pack, when it is loaded, may.
    void aStagedCopyLeavesTheTrustFileAlone() {
        const QString id = "io.example.staged";
        const QByteArray before = R"({"packs": {"io.example.staged": ["1.0.0"]}})";
        write(TrustStore::defaultPath(), before); // the session's own file (the test's home), which a load adopts digests into
        PackSource from;
        from.location = makePack(source(), id, "1.0.0");
        const Result<std::shared_ptr<PreparedPack>> prepared = preparePack(from, packs(), answering(Answer::Yes));
        QVERIFY(prepared.ok());
        QCOMPARE(readAll(TrustStore::defaultPath()), before);
        QVERIFY(packSecretsDigest(id, "1.0.0").isEmpty());
        QFile::remove(TrustStore::defaultPath());
    }

    // Security (#497): a staged copy of the same id and version that declares the secrets the person approved, and carries the
    // installed pack's source record, does not make the installed pack, edited in place to read a secret nobody approved, read as
    // trusted (it did: summarizing the copy replaced the digest trust compares with, while the registry kept the edited pack).
    void aStagedCopyDoesNotVouchForAnEditedPack() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        const QString folder = packs() + "/" + id;
        QJsonObject pack = QJsonDocument::fromJson(readAll(folder + "/pack.json")).object();
        pack["settings"] = QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}}};
        writeJson(folder + "/pack.json", pack);
        Registry registry;
        loadPacks(registry, {packs()});
        QVERIFY(!trustStore().isTrusted(id, "1.0.0"));
        const QString decoy = makePack(source() + "/decoy", id, "1.0.0"); // what was approved, and where it came from
        QVERIFY(QFile::copy(folder + "/.fugue-source.json", decoy + "/.fugue-source.json"));
        PackSource from;
        from.location = decoy;
        QCOMPARE(preparePack(from, packs(), answering(Answer::Yes)).error().code, QString("pack_exists"));
        QVERIFY(!trustStore().isTrusted(id, "1.0.0"));
    }

    // The previous build wrote no setup marker: its backup is put back only where the pack folder is missing. A working pack
    // with a setup and no marker (installed before markers, or a stopped re-setup) is never replaced by it.
    void aPreviousBuildsBackupNeverReplacesAPack() {
        const QString id = "io.example.tools";
        makePack(packs(), id, "1.1.0", false, {{"command", QJsonArray{"true"}}}); // no marker
        QVERIFY(QDir().rename(makePack(source(), id, "1.0.0"),
                              packs() + QString("/.old-%1-%2-aaaaaaaaaaaa-%3").arg(deadPid()).arg(QDateTime::currentSecsSinceEpoch() - 100).arg(id)));
        QVERIFY(addPack(makePack(source(), "io.example.other", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.1.0"));
        QVERIFY(QDir(packs()).entryList({".old-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
        QCOMPARE(QDir(packs()).entryList({".rescue-*"}, QDir::Dirs | QDir::Hidden).size(), 1);
    }

    // A trust that cannot be put back after a failed same-version update is said in the error, not dropped.
    void aTrustThatCannotBePutBackIsSaid() {
        if (geteuid() == 0)
            QSKIP("root can write a read-only folder");
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        makePack(source(), id, "1.0.0", false, {{"command", QJsonArray{"sh", "-c", "echo lock-trust; exit 1"}}});
        PackPrompts prompts = answering(Answer::Yes);
        prompts.log = [&](const QString &line) { // during the setup, after the trust was forgotten
            if (line == "lock-trust")
                QFile::setPermissions(m_dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        };
        const Result<QList<PackAdded>> updated = updatePacks(id, {}, packs(), trustStore(), prompts);
        QFile::setPermissions(m_dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QCOMPARE(updated.error().code, QString("setup_failed"));
        QVERIFY2(updated.error().message.contains("its trust could not be put back"), qPrintable(updated.error().message));
        QCOMPARE(summarizePack(packs() + "/" + id)->version, QString("1.0.0"));
    }

    // Adopting a digest is best effort inside every load: a busy trust file is skipped at once, not waited for.
    void aBusyTrustFileDoesNotStallALoad() {
        const QString id = "io.example.old";
        QDir(packsDir()).removeRecursively();
        makePack(packsDir(), id, "1.0.0");
        const QByteArray before = R"({"packs": {"io.example.old": ["1.0.0"]}})";
        write(TrustStore::defaultPath(), before);
        {
            QLockFile held(TrustStore::defaultPath() + ".lock");
            QVERIFY(held.tryLock());
            QElapsedTimer timer;
            timer.start();
            Registry registry;
            loadPacks(registry, {packsDir()});
            QVERIFY2(timer.elapsed() < 2500, qPrintable(QString::number(timer.elapsed()))); // the wait was 5 s; nothing else here is slow
            QCOMPARE(readAll(TrustStore::defaultPath()), before);
        }
        Registry registry;
        loadPacks(registry, {packsDir()});
        QVERIFY(readAll(TrustStore::defaultPath()).contains("secrets")); // adopted at the next load
        QDir(packsDir()).removeRecursively();
        QFile::remove(TrustStore::defaultPath());
    }

    // A removed pack whose trust cannot be forgotten is still removed, with a warning, and its keys still go.
    void aRemoveWhoseTrustCannotBeForgottenStillRemoves() {
        if (geteuid() == 0)
            QSKIP("root can write a read-only folder");
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(trustPack(id, packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(QFile::setPermissions(m_dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const Result<PackSummary> removed = removePack(id, packs(), trustStore(), answering(Answer::Yes));
        QFile::setPermissions(m_dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QVERIFY(removed.ok());
        QVERIFY(!removed.warnings().isEmpty());
        QVERIFY(removed.warnings().first().message.contains("was removed, but its trust was not forgotten"));
        QVERIFY(!QFileInfo::exists(packs() + "/" + id));
    }

    // A yes covers the pack the question showed: pack.json edited in place while it was open (a new secret) is not covered.
    void aYesCoversThePackShownNotAnEditMadeMeanwhile() {
        const QString id = "io.example.tools";
        QVERIFY(addPack(makePack(source(), id, "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        const QString folder = packs() + "/" + id;
        const QString shown = packSecretsDigestOfFolder(folder);
        PackPrompts prompts = answering(Answer::Yes);
        prompts.trust = [&](const PackSummary &) {
            QJsonObject pack = QJsonDocument::fromJson(readAll(folder + "/pack.json")).object();
            pack["settings"] = QJsonArray{QJsonObject{{"name", "token_1"}, {"type", "text"}, {"secret", true}}};
            writeJson(folder + "/pack.json", pack);
            return Answer::Yes;
        };
        QVERIFY(trustPack(id, packs(), trustStore(), prompts).ok());
        QCOMPARE(trustStore().sealedDigest(id, "1.0.0"), shown);
        QVERIFY(packSecretsDigestOfFolder(folder) != shown);
    }

    // #418: one command at a time changes the packs folder.
    void aPackIsNotChangedWhileAnotherCommandHoldsTheLock() {
        QVERIFY(addPack(makePack(source(), "io.example.tools", "1.0.0"), {}, packs(), trustStore(), answering(Answer::Yes)).ok());
        {
            QLockFile held(packs() + "/.packs.lock");
            held.setStaleLockTime(0);
            QVERIFY(held.tryLock());
            PackPrompts prompts = answering(Answer::Yes);
            prompts.lockWaitMs = 50;
            const Result<PackSummary> busy = removePack("io.example.tools", packs(), trustStore(), prompts);
            QCOMPARE(busy.error().code, QString("pack_busy"));
            QVERIFY(busy.error().message.contains(QString::number(QCoreApplication::applicationPid())));
            QVERIFY(QFileInfo(packs() + "/io.example.tools").isDir());
        }
        QVERIFY(removePack("io.example.tools", packs(), trustStore(), answering(Answer::Yes)).ok());
        QVERIFY(leftovers().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPackManager)
#include "test_pack_manager.moc"
