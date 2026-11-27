// `fugue cache status` and `fugue cache clear` (spec 13.1, 10.6, #141) as the real binary, over
// an XDG_CACHE_HOME of the test's own: no test reads or clears the owner's ~/.cache/fugue.
// The cache manager's rules (what is kept, the limits) are tested in tests/engine/test_cache_manager.cpp;
// here: the options, the output and the exit codes.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "../test_paths.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"

using namespace fugue;

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QString firstCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
};

} // namespace

class TestCliCache : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    QString cache() const { return m_dir.filePath("cache/fugue"); }

    Outcome fugue(const QStringList &args, bool json = true) const {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.insert("XDG_DATA_HOME", m_dir.filePath("data"));
        env.insert("XDG_STATE_HOME", m_dir.filePath("state"));
        env.insert("XDG_CACHE_HOME", m_dir.filePath("cache"));
        env.insert("XDG_CONFIG_HOME", m_dir.filePath("config"));
        env.insert("HOME", m_dir.filePath("home"));
        process.setProcessEnvironment(env);
        process.start(fugueBinary(), json ? QStringList(args) << "--json" : args);
        process.closeWriteChannel();
        Outcome outcome;
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            return outcome;
        }
        outcome.exitCode = process.exitCode();
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    void storeResult(int n, const QString &workflow, const QString &node) {
        FileHashes hashes(m_dir.filePath("hashes.json"));
        ResultCache results(cache() + "/results");
        const QString dir = m_dir.filePath(QString("nodes/%1").arg(n));
        QDir().mkpath(dir);
        const QByteArray data = QByteArray(2000, 'a') + QByteArray::number(n);
        QFile file(dir + "/latent.bin");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(data);
        file.close();
        const Values outputs{{"latent", QJsonObject{{"data", "fugue/latent"}, {"path", file.fileName()}, {"hash", sha256(data)}}}};
        QVERIFY(results.store(sha256(QByteArray::number(n)), "fugue/fake", outputs, dir, UsedBy{workflow, node}, hashes).ok());
    }

    void storeThumbnail(const QString &name, int bytes) {
        QDir().mkpath(cache() + "/thumbnails");
        QFile file(cache() + "/thumbnails/" + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(bytes, 'x'));
    }

private slots:
    void init() {
        QDir(m_dir.path()).removeRecursively();
        QDir().mkpath(m_dir.path());
    }

    void statusOfAnEmptyCacheIsZeroes() {
        const Outcome out = fugue({"cache", "status"});
        QCOMPARE(out.exitCode, 0);
        QCOMPARE(out.data().value("total_bytes").toInt(), 0);
        QCOMPARE(out.data().value("kinds").toArray().size(), 3);
    }

    void statusShowsEachKindAndTheResultsPerWorkflowAndNode() {
        storeResult(1, "/w/one.fugue", "blur");
        storeResult(2, "/w/two.fugue", "blur");
        storeThumbnail("a-512.png", 700);
        const auto folder = RunFolder::create(cache() + "/runs", "r1");
        QVERIFY(folder.ok());
        folder.value().writeState(QJsonObject{{"status", "ok"}, {"ended", "2026-11-20T12:00:00.000Z"}});

        const Outcome out = fugue({"cache", "status"});
        QCOMPARE(out.exitCode, 0);
        const QJsonArray kinds = out.data().value("kinds").toArray();
        QCOMPARE(kinds.at(0).toObject().value("kind").toString(), QStringLiteral("results"));
        QCOMPARE(kinds.at(0).toObject().value("items").toInt(), 2);
        QCOMPARE(kinds.at(1).toObject().value("bytes").toInt(), 700);
        QCOMPARE(kinds.at(2).toObject().value("items").toInt(), 1);
        const QJsonArray workflows = out.data().value("workflows").toArray();
        QCOMPARE(workflows.size(), 2);
        QCOMPARE(workflows.at(0).toObject().value("nodes").toArray().at(0).toObject().value("node").toString(), QStringLiteral("blur"));

        const Outcome text = fugue({"cache", "status"}, false);
        QVERIFY2(text.out.contains("thumbnails"), text.out.constData());
        QVERIFY2(text.out.contains("/w/one.fugue"), text.out.constData());
    }

    void clearWithNoKindClearsTheResultsAndNothingElse() {
        storeResult(1, "/w/one.fugue", "blur");
        storeThumbnail("a-512.png", 700);
        const Outcome out = fugue({"cache", "clear"});
        QCOMPARE(out.exitCode, 0);
        QCOMPARE(out.data().value("kinds").toArray().size(), 1);
        QCOMPARE(out.data().value("removed").toInt(), 1);
        QVERIFY(QFile::exists(cache() + "/thumbnails/a-512.png"));
    }

    void dryRunRemovesNothingAndSaysWhatItWould() {
        storeResult(1, "/w/one.fugue", "blur");
        storeThumbnail("a-512.png", 700);
        const Outcome out = fugue({"cache", "clear", "--kind", "all", "--dry-run"});
        QCOMPARE(out.exitCode, 0);
        QVERIFY(out.data().value("dry_run").toBool());
        QCOMPARE(out.data().value("removed").toInt(), 2);
        QVERIFY(QFile::exists(cache() + "/thumbnails/a-512.png"));
        QCOMPARE(fugue({"cache", "status"}).data().value("kinds").toArray().at(0).toObject().value("items").toInt(), 1);
    }

    void clearTakesKindWorkflowNodeAndAge() {
        storeResult(1, "/w/one.fugue", "blur");
        storeResult(2, "/w/one.fugue", "sharpen");
        storeResult(3, "/w/two.fugue", "blur");
        QCOMPARE(fugue({"cache", "clear", "--workflow", "/w/one.fugue", "--node", "blur"}).data().value("removed").toInt(), 1);
        // Nothing is older than a day.
        QCOMPARE(fugue({"cache", "clear", "--older-than", "1d"}).data().value("removed").toInt(), 0);
        QCOMPARE(fugue({"cache", "clear", "--kind", "results", "--older-than", "1h", "--dry-run"}).data().value("removed").toInt(), 0);
        QCOMPARE(fugue({"cache", "clear", "--kind", "results"}).data().value("removed").toInt(), 2);
    }

    void aBadOptionIsAUsageError() {
        for (const QStringList &args : {QStringList{"cache", "clear", "--kind", "pictures"},
                                        QStringList{"cache", "clear", "--older-than", "soon"},
                                        QStringList{"cache", "clear", "--kind", "thumbnails", "--workflow", "/w/a.fugue"},
                                        QStringList{"cache", "clear", "--kind", "runs", "--node", "blur"}}) {
            const Outcome out = fugue(args);
            QCOMPARE(out.exitCode, 1);
            QCOMPARE(out.firstCode(), QStringLiteral("usage"));
        }
    }
};

QTEST_GUILESS_MAIN(TestCliCache)
#include "test_cli_cache.moc"
