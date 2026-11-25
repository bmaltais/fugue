// Pack secrets (spec 6.4, 6.7, #402): the `settings` of pack.json and what a bad declaration says, the store
// (0600, atomic, never the values in a list), the masking, and the pack summary the trust prompt shows.
//
// Seams: parsePackSecrets and loadPack for the declaration, PackSecretStore on a scratch file, summarizePack.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <thread>
#include <vector>

#include "engine/pack_loader.h"
#include "engine/pack_manager.h"
#include "engine/pack_secrets.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

QJsonObject secretEntry(const QString &name, const QJsonObject &extra = {}) {
    QJsonObject entry{{"name", name}, {"type", "text"}, {"secret", true}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        entry.insert(it.key(), it.value());
    return entry;
}

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(bytes);
}

// A pack of one command node declaring `settings`.
QString makePack(const QString &root, const QJsonValue &settings) {
    const QString dir = root + "/io.test.keyed";
    QJsonObject pack{{"manifest", 1}, {"id", "io.test.keyed"}, {"version", "1.0.0"}, {"name", "Keyed"}, {"nodes", QJsonArray{"call"}}};
    if (!settings.isUndefined())
        pack.insert("settings", settings);
    write(dir + "/pack.json", QJsonDocument(pack).toJson());
    write(dir + "/call/node.json",
          QJsonDocument(QJsonObject{{"manifest", 1}, {"id", "io.test.keyed/call"}, {"version", "1.0.0"}, {"name", "Call"},
                                    {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                                    {"inputs", QJsonArray{}}, {"outputs", QJsonArray{}}, {"needs", QJsonObject{{"network", true}}}})
              .toJson());
    write(dir + "/call/run.sh", "#!/bin/bash\n");
    return dir;
}

} // namespace

class TestPackSecrets : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void aDeclarationOfSecretsReadsWithLabelsAndDescriptions() {
        const auto specs = parsePackSecrets(QJsonArray{secretEntry("token_1", {{"label", "API key"}, {"description", "For the service."}}),
                                                      secretEntry("webhook")});
        QVERIFY(specs.ok());
        QCOMPARE(packSecretNames(*specs), (QStringList{"token_1", "webhook"}));
        QCOMPARE(specs->at(0).label, QString("API key"));
        QCOMPARE(specs->at(0).description, QString("For the service."));
        QCOMPARE(specs->at(1).label, QString("webhook")); // the name stands in for a missing label
        QVERIFY(parsePackSecrets(QJsonValue(QJsonValue::Undefined))->isEmpty());
    }

    void aBadDeclarationIsOneErrorPerProblem_data() {
        QTest::addColumn<QJsonValue>("settings");
        QTest::newRow("not a list") << QJsonValue(QJsonObject{});
        QTest::newRow("not an object") << QJsonValue(QJsonArray{"token_1"});
        QTest::newRow("no name") << QJsonValue(QJsonArray{QJsonObject{{"secret", true}}});
        QTest::newRow("name in capitals") << QJsonValue(QJsonArray{secretEntry("Token")});
        QTest::newRow("name with a dash") << QJsonValue(QJsonArray{secretEntry("my-key")});
        QTest::newRow("name too long") << QJsonValue(QJsonArray{secretEntry(QString(41, 'a'))});
        QTest::newRow("not secret") << QJsonValue(QJsonArray{QJsonObject{{"name", "plain"}, {"type", "text"}}});
        QTest::newRow("secret false") << QJsonValue(QJsonArray{secretEntry("k", {{"secret", false}})});
        QTest::newRow("other type") << QJsonValue(QJsonArray{secretEntry("k", {{"type", "number"}})});
        QTest::newRow("label not text") << QJsonValue(QJsonArray{secretEntry("k", {{"label", 3}})});
        QTest::newRow("twice") << QJsonValue(QJsonArray{secretEntry("k"), secretEntry("k")});
        QJsonArray many;
        for (int i = 0; i < 17; ++i)
            many.append(secretEntry(QStringLiteral("k%1").arg(i)));
        QTest::newRow("too many") << QJsonValue(many);
    }
    void aBadDeclarationIsOneErrorPerProblem() {
        QFETCH(QJsonValue, settings);
        const auto specs = parsePackSecrets(settings);
        QVERIFY(!specs.ok());
        QCOMPARE(specs.error().code, QString("invalid_document"));
        QVERIFY2(specs.error().message.contains("pack.json"), qPrintable(specs.error().message));
    }

    void aPackWithABadDeclarationIsDisabledWithTheReasonAndAGoodOneLoads() {
        Registry registry;
        const LoadedPack bad = loadPack(registry, makePack(m_dir.filePath("bad"), QJsonArray{secretEntry("Bad Name")}));
        QVERIFY(!bad.enabled);
        QVERIFY2(bad.errors.first().message.contains("Bad Name"), qPrintable(bad.errors.first().message));

        Registry good;
        const LoadedPack loaded = loadPack(good, makePack(m_dir.filePath("good"), QJsonArray{secretEntry("token_1"), secretEntry("token_2")}));
        QVERIFY(loaded.enabled);
        QCOMPARE(packSecretNames(loaded.secrets), (QStringList{"token_1", "token_2"}));
        // Every node of the pack carries the names, for the runner.
        QCOMPARE(good.nodeType("io.test.keyed/call")->packSecrets, (QStringList{"token_1", "token_2"}));
    }

    void thePackSummaryNamesTheSecretsAndASummaryWithOtherSecretsIsAnotherPack() {
        const Result<PackSummary> summary = summarizePack(makePack(m_dir.filePath("sum"), QJsonArray{secretEntry("token_1")}));
        QVERIFY(summary.ok());
        QCOMPARE(summary->secrets, QStringList{"token_1"});
        PackSummary more = *summary;
        more.secrets << "token_2";
        QVERIFY(samePack(*summary, *summary));
        QVERIFY(!samePack(*summary, more)); // a yes covers the secrets that were shown, no others
    }

    void theManifestMarksASecretThatNeedsAnOrigin() {
        const auto specs = parsePackSecrets(QJsonArray{secretEntry("a", {{"origin", "required"}}), secretEntry("b", {{"origin", "optional"}}), secretEntry("c")});
        QVERIFY(specs.ok());
        QVERIFY(specs->at(0).originRequired);
        QVERIFY(!specs->at(1).originRequired && !specs->at(2).originRequired);
        QVERIFY(!parsePackSecrets(QJsonArray{secretEntry("a", {{"origin", "always"}})}).ok());
        Registry registry;
        const LoadedPack loaded = loadPack(registry, makePack(m_dir.filePath("origin"), QJsonArray{secretEntry("a", {{"origin", "required"}}), secretEntry("b")}));
        QVERIFY(loaded.enabled);
        QCOMPARE(registry.nodeType("io.test.keyed/call")->packSecretsOriginRequired, QStringList{"a"});
    }

    void anOriginIsSchemeHostAndPortAndNothingElse() {
        QCOMPARE(normalizeOrigin("https://API.Example.com"), QString("https://api.example.com"));
        QCOMPARE(normalizeOrigin("https://api.example.com:443/"), QString("https://api.example.com"));
        QCOMPARE(normalizeOrigin("http://127.0.0.1:8080"), QString("http://127.0.0.1:8080"));
        QCOMPARE(normalizeOrigin("http://[::1]:80"), QString("http://[::1]"));
        QCOMPARE(normalizeOrigin("https://api.example.com:8443"), QString("https://api.example.com:8443"));
        for (const QString bad : {"", "api.example.com", "ftp://x.example.com", "https://u:p@x.example.com", "https://x.example.com/path",
                                  "https://x.example.com/?q=1", "https://x.example.com/#f", "https://", "https://x.example.com:99999"})
            QVERIFY2(normalizeOrigin(bad).isEmpty(), qPrintable(bad));
    }

    void theStoreKeepsValuesPerPackAndSourceWith0600AndListsOnlyNamesAndOrigins() {
        const QString path = m_dir.filePath("config/pack-secrets.json");
        PackSecretStore store(path);
        const QString key = packSecretKey("io.test.keyed", "/src/a");
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(store.namesSet(key).isEmpty());
        QVERIFY(store.set(key, "token_1", "made-up-value-1", "https://API.example.com").isEmpty());
        QVERIFY(store.set(key, "token_2", "made-up-value-2").isEmpty());
        QVERIFY(store.set(packSecretKey("io.test.other", ""), "token_1", "made-up-value-3").isEmpty());
        QCOMPARE(QFileInfo(path).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther
                                                  | QFileDevice::ExeOwner),
                 QFileDevice::Permissions());
        QVERIFY(QFileInfo(path).permissions().testFlag(QFileDevice::ReadOwner));

        QCOMPARE(store.namesSet(key), (QStringList{"token_1", "token_2"}));
        QCOMPARE(store.originOf(key, "token_1"), QString("https://api.example.com"));
        QVERIFY(store.originOf(key, "token_2").isEmpty());
        // Only the declared names come out, only for the key asked: another source of the same id gets none.
        const auto values = store.values(key, {"token_1"});
        QVERIFY(values.ok());
        QCOMPARE(values->value("token_1").value, QString("made-up-value-1"));
        QCOMPARE(values->value("token_1").origin, QString("https://api.example.com"));
        QCOMPARE(values->size(), 1);
        QVERIFY(store.values(key, {"nothing"})->isEmpty());
        QVERIFY(store.values(packSecretKey("io.test.keyed", "/src/b"), {"token_1"})->isEmpty());
        QVERIFY(store.values(packSecretKey("io.test.keyed", ""), {"token_1"})->isEmpty());
        QCOMPARE(store.namesSetForOtherSources("io.test.keyed", packSecretKey("io.test.keyed", "/src/b")), (QStringList{"token_1", "token_2"}));
        QVERIFY(store.namesSetForOtherSources("io.test.keyed", key).isEmpty());

        QVERIFY(store.set(key, "token_1", "replaced-value-9").isEmpty()); // replaced whole: the old origin goes with the old value
        QCOMPARE(store.values(key, {"token_1"})->value("token_1").value, QString("replaced-value-9"));
        QVERIFY(store.originOf(key, "token_1").isEmpty());
        QVERIFY(store.clear(key, "token_1").isEmpty());
        QCOMPARE(store.namesSet(key), QStringList{"token_2"});
        QVERIFY(store.clear(key, "token_1").isEmpty()); // clearing what is not set is no error
        QVERIFY(store.forgetPack("io.test.keyed").isEmpty());
        QVERIFY(store.namesSet(key).isEmpty());
        QCOMPARE(store.namesSet(packSecretKey("io.test.other", "")), QStringList{"token_1"}); // another pack's stay
        QCOMPARE(QDir(QFileInfo(path).path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"pack-secrets.json"});
    }

    void aSecretThatNeedsAnOriginIsRefusedWithoutOneOrWithABadOne() {
        PackSecretStore store(m_dir.filePath("needs/pack-secrets.json"));
        QCOMPARE(store.set("k", "token_1", "made-up-value-1", {}, true).first().code, QString("usage"));
        QCOMPARE(store.set("k", "token_1", "made-up-value-1", "https://x.example.com/path", true).first().code, QString("usage"));
        QVERIFY(!QFileInfo::exists(store.path()));
        QVERIFY(store.set("k", "token_1", "made-up-value-1", "https://x.example.com", true).isEmpty());
    }

    void aStoreThatCannotBeReadIsAnErrorAndIsNeverOverwritten() {
        const QString path = m_dir.filePath("broken/pack-secrets.json");
        write(path, "{ not json");
        PackSecretStore store(path);
        QVERIFY(!store.values("k", {"token_1"}).ok());
        QVERIFY(hasErrors(store.set("k", "token_1", "made-up-value-1")));
        QCOMPARE(QFile(path).size(), qint64(10)); // the file the person had is untouched
    }

    void aValueMustBeOneLineWithNoNulAndAtMost4096BytesAndANameMustFit() {
        PackSecretStore store(m_dir.filePath("refuse/pack-secrets.json"));
        QCOMPARE(store.set("k", "token_1", "").first().code, QString("usage"));
        QCOMPARE(store.set("k", "Bad Name", "value-value").first().code, QString("usage"));
        QCOMPARE(store.set("k", "token_1", "two\nlines").first().code, QString("usage"));
        QCOMPARE(store.set("k", "token_1", "carriage\rreturn").first().code, QString("usage"));
        QCOMPARE(store.set("k", "token_1", QString("nul") + QChar(0) + "byte").first().code, QString("usage"));
        QCOMPARE(store.set("k", "token_1", QString(4097, 'a')).first().code, QString("usage"));
        QVERIFY(!QFileInfo::exists(store.path()));
        QVERIFY(store.set("k", "token_1", QString(4096, 'a')).isEmpty());
    }

    void twoWritersAtOnceLoseNoValue() {
        const QString path = m_dir.filePath("race/pack-secrets.json");
        std::vector<std::thread> writers;
        std::atomic<int> failed{0};
        for (int i = 0; i < 8; ++i)
            writers.emplace_back([path, i, &failed] {
                PackSecretStore store(path);
                if (!store.set("k", QStringLiteral("n%1").arg(i), QStringLiteral("made-up-value-%1").arg(i)).isEmpty())
                    ++failed;
            });
        for (std::thread &writer : writers)
            writer.join();
        QCOMPARE(failed.load(), 0);
        QCOMPARE(PackSecretStore(path).namesSet("k").size(), 8);
    }

    void maskingTakesTheValueInEveryFormItAppearsInAndLeavesShortOnes() {
        QCOMPARE(maskSecrets("key=made up&key and made%20up%26key and ab", {"made up&key", "ab"}), QString("key=*** and *** and ab"));
        QCOMPARE(maskSecrets("a/made%20up%26key/b", {"made up&key"}), QString("a/***/b"));
        QCOMPARE(maskSecrets("lower made%2fup%2fkey+x and made+up", {"made/up/key", "made up"}), QString("lower ***+x and ***")); // lower-case hex, `+`
        QCOMPARE(maskSecrets("{\"k\":\"say \\\"hi\\\" now\"}", {"say \"hi\" now"}), QString("{\"k\":\"***\"}")); // JSON-escaped
        QCOMPARE(maskSecrets("abc", {"abc"}), QString("abc")); // under four characters nothing is masked
    }

    void aSummaryTellsWhichSecretsAreAlreadySetAndWhichOnlyForAnotherSource() {
        const QString folder = makePack(m_dir.filePath("prompt"), QJsonArray{secretEntry("token_1"), secretEntry("token_2")});
        const QString id = "io.test.keyed";
        PackSecretStore store; // the session's own, a scratch home under test
        QVERIFY(store.set(packSecretKey(id, "/elsewhere"), "token_2", "made-up-value-5").isEmpty());
        Result<PackSummary> summary = summarizePack(folder);
        QVERIFY(summary.ok());
        QVERIFY(summary->secretsSet.isEmpty());
        QCOMPARE(summary->secretsOtherSource, QStringList{"token_2"});
        QVERIFY(store.set(packSecretKey(id, ""), "token_1", "made-up-value-6").isEmpty());
        summary = summarizePack(folder);
        QCOMPARE(summary->secretsSet, QStringList{"token_1"});
        QVERIFY(store.forgetPack(id).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPackSecrets)
#include "test_pack_secrets.moc"
