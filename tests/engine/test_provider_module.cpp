// Provider modules (spec 8.8): a folder with a provider.json is a provider, with no change to
// Fugue. The manifest, the listing, and a tiny third-party provider (tests/provider_module_support)
// loaded through createProvider over both transports.
//
// Seam: createProvider(instance, modulesRoot) and the Provider interface (8.11). The example
// module is a folder made here, with the fake program copied in as its own file.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include <csignal>

#include "../test_paths.h"
#include "engine/provider_module.h"
#include "engine/providers.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::await;

namespace {

QStringList logLines(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
}

QStringList ids(const QList<ProviderModule> &modules) {
    QStringList found;
    for (const ProviderModule &module : modules)
        found << module.id;
    return found;
}

} // namespace

class TestProviderModule : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_root;
    QString m_log;

    // The example module: a folder named after its id, with the program in it and a manifest.
    QString install(const QString &id, const QJsonObject &manifest, bool withProgram = true) {
        const QString folder = QDir(m_root).filePath(id);
        QDir().mkpath(folder);
        if (withProgram) {
            const QString program = QDir(folder).filePath("toy");
            QFile::remove(program);
            QFile::copy(fugueFakeModuleProvider(), program);
            QFile(program).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        QJsonObject full = manifest;
        full.insert("manifest", 1);
        full.insert("id", id);
        if (!full.contains("version"))
            full.insert("version", "0.1.0");
        QFile file(QDir(folder).filePath("provider.json"));
        if (!file.open(QIODevice::WriteOnly))
            qFatal("cannot write the manifest");
        file.write(QJsonDocument(full).toJson());
        return folder;
    }

    QJsonObject stdioManifest() {
        return {{"name", "Toy engine"}, {"transport", "stdio"},
                {"start", QJsonObject{{"command", QJsonArray{"./toy", "--stdio"}}}},
                {"capabilities", QJsonArray{"image.generate"}}};
    }

    QJsonObject httpManifest() {
        return {{"name", "Toy http"}, {"transport", "http"},
                {"start", QJsonObject{{"command", QJsonArray{"./toy", "--http", "{port}"}}, {"idle_timeout_s", 0}}},
                {"address", "http://127.0.0.1:{port}"}};
    }

    std::shared_ptr<Provider> make(const QString &type, const QString &address = {}) {
        ProviderInstance instance;
        instance.name = QStringLiteral("Toy");
        instance.type = type;
        instance.address = address;
        const Result<std::shared_ptr<Provider>> provider = createProvider(instance, m_root);
        if (!provider) {
            qWarning("%s", qPrintable(provider.error().message));
            return nullptr;
        }
        return provider.value();
    }

    // Submits, waits for the end of the job, and gives its status; `result` is taken when done.
    JobStatus run(Provider &provider, const QJsonObject &params, JobResult *result = nullptr) {
        JobRequest request;
        request.capability = QStringLiteral("image.generate");
        request.model = QJsonObject{{"name", "toy"}};
        request.params = params;
        request.outputDir = m_dir.filePath("out");
        QDir().mkpath(request.outputDir);
        // A status can come before submit's reply is read here: keep the last one of every job.
        QHash<QString, JobStatus> seen;
        QString job;
        connect(&provider, &Provider::statusChanged, this,
                [&](const QString &id, const JobStatus &status) { seen.insert(id, status); });
        const Result<QString> submitted =
            await<QString>([&](Reply<QString> reply) { provider.submit(request, std::move(reply)); });
        JobStatus last;
        if (!submitted) {
            last.state = QStringLiteral("error");
            last.error = submitted.error();
            return last;
        }
        job = submitted.value();
        if (!QTest::qWaitFor([&] { return seen.value(job).isTerminal(); }, 5000))
            last.state = QStringLiteral("timed out");
        else
            last = seen.value(job);
        provider.disconnect(this);
        if (result && last.state == QLatin1String("done")) {
            const Result<JobResult> got = await<JobResult>([&](Reply<JobResult> reply) { provider.result(job, std::move(reply)); });
            if (got)
                *result = got.value();
        }
        return last;
    }

private slots:
    void init() {
        m_root = m_dir.filePath(QStringLiteral("providers-%1").arg(QTest::currentTestFunction()));
        QDir().mkpath(m_root);
        m_log = m_dir.filePath(QStringLiteral("module-%1.log").arg(QTest::currentTestFunction()));
        qputenv("FAKE_MODULE_LOG", m_log.toLocal8Bit());
    }

    void aManifestIsReadAndChecked() {
        const QString folder = m_dir.filePath("io.example.toy");
        const Result<ProviderModule> module = parseProviderModule(
            QJsonObject{{"manifest", 1}, {"id", "io.example.toy"}, {"version", "1.2.3"}, {"transport", "stdio"},
                        {"start", QJsonObject{{"command", QJsonArray{"./bin/toy", "--stdio"}}, {"idle_timeout_s", 30}}},
                        {"capabilities", QJsonArray{"image.generate"}},
                        {"settings", QJsonArray{QJsonObject{{"name", "key"}, {"secret", true}}}}},
            folder);
        QVERIFY(module.ok());
        QCOMPARE(module->name, QStringLiteral("io.example.toy")); // defaults to the id
        QCOMPARE(module->program(), folder + "/bin/toy");         // its own file, against its folder
        QCOMPARE(module->arguments(), QStringList{"--stdio"});
        QCOMPARE(module->idleTimeoutS, 30);
        QCOMPARE(module->capabilities, QStringList{"image.generate"});
        QCOMPARE(module->settings.size(), 1);

        const auto bad = [&](const QJsonObject &changes, const QString &folderName = QStringLiteral("io.example.toy")) {
            QJsonObject manifest{{"manifest", 1}, {"id", "io.example.toy"}, {"version", "1"}, {"transport", "stdio"},
                                 {"start", QJsonObject{{"command", QJsonArray{"toy"}}}}};
            for (auto it = changes.begin(); it != changes.end(); ++it)
                manifest.insert(it.key(), it.value());
            const Result<ProviderModule> result = parseProviderModule(manifest, m_dir.filePath(folderName));
            return result.ok() ? QString() : result.error().message;
        };
        QVERIFY(bad({}).isEmpty());
        QVERIFY(bad({{"manifest", 2}}).contains("manifest"));
        QVERIFY(bad({{"version", ""}}).contains("version"));
        QVERIFY(bad({{"transport", "carrier-pigeon"}}).contains("transport"));
        QVERIFY(bad({{"id", "../evil"}}, "evil").contains("id"));
        QVERIFY(bad({}, "other-name").contains("named after the id"));
        QVERIFY(bad({{"start", QJsonObject{}}}).contains("start.command"));
        QVERIFY(bad({{"transport", "http"}, {"start", QJsonObject{}}}).contains("address"));
        QVERIFY(bad({{"transport", "http"}, {"address", "http://127.0.0.1:{port}"}, {"start", QJsonObject{}}}).contains("{port}"));
    }

    void installedModulesAreListedAfterTheBuiltInOnesAndOneBadFolderHidesNoOther() {
        install("b.good", stdioManifest());
        install("a.good", httpManifest());
        QDir(m_root).mkpath("not-a-module"); // no provider.json: not a module
        QDir(m_root).mkpath("broken");
        QFile broken(m_root + "/broken/provider.json");
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{ nope");
        broken.close();
        install("omarchy", stdioManifest()); // the id of a provider that comes with Fugue

        const Result<QList<ProviderModule>> all = providerModules(m_root);
        QVERIFY(all.ok());
        QCOMPARE(ids(all.value()), (QStringList{"omarchy", "http", "fugue-helper", "fal", "gemini", "replicate", "cloudflare", "a.good", "b.good"}));
        QVERIFY(all->first().isBuiltin());
        QCOMPARE(all.warnings().size(), 2); // broken, and the one that took omarchy's id
        QVERIFY(all.warnings().first().severity == Error::Severity::Warning);
    }

    void anUnknownTypeNamesTheModulesThereAre() {
        install("io.example.toy", stdioManifest());
        ProviderInstance instance;
        instance.name = QStringLiteral("X");
        instance.type = QStringLiteral("nope");
        const Result<std::shared_ptr<Provider>> provider = createProvider(instance, m_root);
        QVERIFY(!provider.ok());
        QCOMPARE(provider.error().code, QStringLiteral("unknown_provider"));
        QStringList valid;
        for (const QJsonValue &item : provider.error().valid)
            valid << item.toString();
        QCOMPARE(valid, (QStringList{"cloudflare", "fal", "fugue-helper", "gemini", "http", "io.example.toy", "omarchy", "replicate"}));
    }

    void aThirdPartyStdioModuleLoadsWithNoChangeToFugue() {
        install("io.example.toy", stdioManifest());
        const auto toy = make("io.example.toy");
        QVERIFY(toy);

        // Its own describe, read over the protocol: models, status, limits, defaults.
        const Result<Description> described = await<Description>([&](Reply<Description> reply) { toy->describe(std::move(reply)); });
        QVERIFY2(described.ok(), qPrintable(described.error().message));
        QCOMPARE(described->engine, QStringLiteral("toy-engine"));
        QVERIFY(described->sharesFilesystem);
        const CapabilityOffer *offer = described->offer("image.generate");
        QVERIFY(offer);
        QCOMPARE(offer->models.size(), 2);
        QCOMPARE(offer->models.at(0).handle.value("name").toString(), QStringLiteral("toy"));
        QCOMPARE(offer->models.at(0).title, QStringLiteral("Toy model"));
        QCOMPARE(offer->models.at(0).status, ModelStatus::Ready);
        QCOMPARE(offer->models.at(0).defaults.value("steps").toInt(), 2); // from the capability
        QCOMPARE(offer->models.at(0).limits.value("max_width").toInt(), 64);
        QCOMPARE(offer->models.at(0).properties.value("edit").toBool(true), false);
        QCOMPARE(offer->models.at(1).status, ModelStatus::Loading);
        QCOMPARE(offer->models.at(1).reason, QStringLiteral("It is warming up."));

        // The registry picks it like any provider (8.3).
        Candidate candidate;
        candidate.instance.name = QStringLiteral("Toy");
        candidate.description = described;
        Requirement requirement;
        requirement.capability = QStringLiteral("image.generate");
        const Result<Selection> chosen = selectProvider({candidate}, requirement);
        QVERIFY(chosen.ok());
        QCOMPARE(chosen->engine, QStringLiteral("toy-engine"));
        QCOMPARE(chosen->model.handle.value("name").toString(), QStringLiteral("toy"));

        // A job: status pushed, the file in the job's folder, the values used.
        JobResult result;
        const JobStatus status = run(*toy, QJsonObject{{"seed", 7}}, &result);
        QCOMPARE(status.state, QStringLiteral("done"));
        const QString image = result.outputs.value("image").toObject().value("path").toString();
        QVERIFY(QFile::exists(image));
        QVERIFY(image.startsWith(m_dir.filePath("out")));
        QCOMPARE(result.used.value("seed").toInt(), 7);
        QCOMPARE(logLines(m_log), (QStringList{"start", "describe", "submit toy"})); // one process for both

        // The module's own error code is a failed node; its message comes along.
        const JobStatus failed = run(*toy, QJsonObject{{"fail", "toy_broke"}});
        QCOMPARE(failed.state, QStringLiteral("error"));
        QCOMPARE(failed.error.message, QStringLiteral("the toy failed as asked"));

        // Stopped, it gives its memory back and asks the module to shut down.
        const Result<Nothing> stopped = await<Nothing>([&](Reply<Nothing> reply) { toy->shutdown(std::move(reply)); });
        QVERIFY(stopped.ok());
        QVERIFY(logLines(m_log).contains("shutdown"));
    }

    void aStdioModuleThatIsNotThereIsUnreachableNotACrash() {
        install("io.example.toy", stdioManifest(), /*withProgram=*/false);
        const auto toy = make("io.example.toy");
        QVERIFY(toy);
        const Result<Description> described = await<Description>([&](Reply<Description> reply) { toy->describe(std::move(reply)); });
        QVERIFY(!described.ok());
        QCOMPARE(described.error().code, QStringLiteral("provider_unreachable"));
        QVERIFY2(described.error().message.contains("Toy engine"), qPrintable(described.error().message));
    }

    void anHttpModuleFugueStartsIsStartedOnAFreePortAndStoppedWithTheProvider() {
        install("io.example.toyhttp", httpManifest());
        const auto toy = make("io.example.toyhttp");
        QVERIFY(toy);
        QVERIFY(logLines(m_log).isEmpty()); // creating it starts nothing

        const Result<Description> described = await<Description>([&](Reply<Description> reply) { toy->describe(std::move(reply)); });
        QVERIFY2(described.ok(), qPrintable(described.error().message));
        QVERIFY(!described->sharesFilesystem); // an http module is sent data unless it says it shares
        QCOMPARE(described->offer("image.generate")->models.size(), 2);

        JobResult result;
        const JobStatus status = run(*toy, QJsonObject{{"seed", 3}}, &result);
        QCOMPARE(status.state, QStringLiteral("done"));
        const QString image = result.outputs.value("image").toObject().value("path").toString();
        QVERIFY(image.startsWith(m_dir.filePath("out")));
        QFile file(image);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().startsWith("\x89PNG"));
        QCOMPARE(result.used.value("seed").toInt(), 3);
        QCOMPARE(logLines(m_log).count("start"), 1); // describe and the job shared the process

        const Result<Nothing> stopped = await<Nothing>([&](Reply<Nothing> reply) { toy->shutdown(std::move(reply)); });
        QVERIFY(stopped.ok());
        QTRY_VERIFY(logLines(m_log).contains("shutdown"));
    }

    void anIdleHttpModuleIsStoppedAndStartedAgainByTheNextJob() {
        QJsonObject manifest = httpManifest();
        manifest.insert("start", QJsonObject{{"command", QJsonArray{"./toy", "--http", "{port}"}}, {"idle_timeout_s", 1}});
        install("io.example.toyhttp", manifest);
        const auto toy = make("io.example.toyhttp");
        QVERIFY(toy);
        QCOMPARE(run(*toy, {}).state, QStringLiteral("done"));
        // Wait on the module's own stop event, not on a time: the idle timer fires one second after the
        // last job, and under load the first job can take longer than that, so the number of starts so
        // far is read after the stop, and the next job must add exactly one.
        QTRY_VERIFY_WITH_TIMEOUT(logLines(m_log).contains("shutdown"), 30000);
        const qsizetype startsBefore = logLines(m_log).count("start");
        QVERIFY(startsBefore >= 1);
        QCOMPARE(run(*toy, {}).state, QStringLiteral("done"));
        QTRY_COMPARE(logLines(m_log).count("start"), startsBefore + 1);
        const Result<Nothing> stopped = await<Nothing>([&](Reply<Nothing> reply) { toy->shutdown(std::move(reply)); });
        QVERIFY(stopped.ok());
    }

    void anHttpModuleTheUserRunsIsReachedAtTheInstancesAddress() {
        // No start block: the module is a server the user runs, and the instance names where.
        install("io.example.toyhttp", QJsonObject{{"transport", "http"}, {"address", "http://127.0.0.1:1"}});
        QTcpServer finder;
        QVERIFY(finder.listen(QHostAddress::LocalHost, 0));
        const quint16 port = finder.serverPort();
        finder.close();
        QProcess server;
        server.start(fugueFakeModuleProvider(), {"--http", QString::number(port)});
        QVERIFY(server.waitForStarted());
        QTest::qWait(300);

        const auto toy = make("io.example.toyhttp", QStringLiteral("http://127.0.0.1:%1").arg(port));
        QVERIFY(toy);
        const Result<Description> described = await<Description>([&](Reply<Description> reply) { toy->describe(std::move(reply)); });
        QVERIFY2(described.ok(), qPrintable(described.error().message));
        QCOMPARE(described->engine, QStringLiteral("toy-engine"));
        QCOMPARE(run(*toy, {}).state, QStringLiteral("done"));
        server.kill();
        server.waitForFinished(2000);
    }

    void anHttpModuleThatDoesNotStartFailsDescribeWithItsLog() {
        install("io.example.toyhttp",
                QJsonObject{{"transport", "http"},
                            {"start", QJsonObject{{"command", QJsonArray{"./toy", "--nonsense"}}, {"start_timeout_s", 5}}},
                            {"address", "http://127.0.0.1:{port}"}});
        const auto toy = make("io.example.toyhttp");
        QVERIFY(toy);
        const Result<Description> described = await<Description>([&](Reply<Description> reply) { toy->describe(std::move(reply)); });
        QVERIFY(!described.ok());
        QCOMPARE(described.error().code, QStringLiteral("provider_unreachable"));
        QVERIFY2(described.error().message.contains("usage: fake_module_provider"), qPrintable(described.error().message));
    }
};

QTEST_GUILESS_MAIN(TestProviderModule)
#include "test_provider_module.moc"
