// The fugue-helper provider (spec 8.7, 8.10): started on demand, kept warm for an idle timeout,
// stopped to give memory back; a crash kills only the helper; out of memory is typed.
//
// Seam: the Provider interface (8.11) of HelperProvider, against a fake helper program that
// speaks the stdio protocol of 8.8 (tests/helper_support/fake_fugue_helper.cpp).

#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <csignal>
#include <optional>

#include "engine/helper_provider.h"
#include "engine/model_offers.h"
#include "engine/providers.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::await;

namespace {

constexpr int kGraceMs = 300;

bool alive(qint64 pid) { return pid > 0 && ::kill(pid_t(pid), 0) == 0; }

QStringList logLines(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
}

} // namespace

class TestHelperProvider : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_log;

    std::unique_ptr<HelperProvider> provider(const QJsonValue &idleSeconds = 60, const QString &program = FUGUE_FAKE_HELPER) {
        ProviderInstance instance;
        instance.name = QStringLiteral("Fugue helper");
        instance.type = QStringLiteral("fugue-helper");
        instance.settings.insert("idle_timeout_s", idleSeconds);
        return std::make_unique<HelperProvider>(instance, program, kGraceMs);
    }

    JobRequest encode(const QJsonObject &fake = {}) {
        JobRequest request;
        request.capability = QStringLiteral("text.encode");
        request.params = QJsonObject{{"prompt", "a lighthouse"},
                                     {"context", QJsonObject{{"llm_path", "/models/qwen.gguf"}}},
                                     {"fake", fake}};
        request.outputDir = m_dir.path();
        return request;
    }

    // Submits and waits for the job to end; its last status.
    JobStatus run(HelperProvider &helper, const JobRequest &request, QString *jobId = nullptr,
                  QList<JobStatus> *seen = nullptr, int timeoutMs = 5000) {
        QString job;
        JobStatus last;
        bool over = false;
        const auto connection = connect(&helper, &Provider::statusChanged, this,
                                        [&](const QString &id, const JobStatus &status) {
                                            if (id != job)
                                                return;
                                            if (seen)
                                                seen->append(status);
                                            last = status;
                                            over = status.isTerminal();
                                        });
        const Result<QString> submitted =
            await<QString>([&](Reply<QString> reply) { helper.submit(request, std::move(reply)); });
        if (!submitted) {
            disconnect(connection);
            last.state = QStringLiteral("error");
            last.error = submitted.error();
            return last;
        }
        job = submitted.value();
        if (jobId)
            *jobId = job;
        if (!QTest::qWaitFor([&] { return over; }, timeoutMs))
            last.state = QStringLiteral("timed out");
        disconnect(connection);
        return last;
    }

private slots:
    void init() {
        m_log = m_dir.filePath(QStringLiteral("helper-%1.log").arg(QTest::currentTestFunction()));
        qputenv("FAKE_HELPER_LOG", m_log.toLocal8Bit());
    }

    void describingDoesNotStartTheHelper() {
        auto helper = provider();
        const Result<Description> description =
            await<Description>([&](Reply<Description> reply) { helper->describe(std::move(reply)); });
        QVERIFY(description.ok());
        QCOMPARE(description->engine, QStringLiteral("sdcpp"));
        QStringList capabilities;
        for (const CapabilityOffer &offer : description->capabilities) {
            capabilities << offer.capability;
            QCOMPARE(offer.status(), ModelStatus::Ready);
            QCOMPARE(offer.models.size(), 1);
            QVERIFY(offer.models.first().handle.isEmpty()); // models come as files in the job
        }
        QCOMPARE(capabilities, (QStringList{"text.encode", "latent.sample", "latent.decode", "latent.encode", "image.upscale"}));
        QCOMPARE(helper->pid(), qint64(0));
        QVERIFY(logLines(m_log).isEmpty());
    }

    void aModelPickerListsNothingOfTheHelpers() {
        // Its one offer per capability takes the model files in the job: not a model to pick.
        QDir().mkpath(QFileInfo(providersPath()).absolutePath());
        QFile file(providersPath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"providers": [{"name": "Fugue helper", "type": "fugue-helper"}]})");
        file.close();
        qputenv("FUGUE_HELPER", FUGUE_FAKE_HELPER);
        std::optional<QList<ListedModel>> listed;
        listOfferedModels({}, {}, [&](QList<ListedModel> models) { listed = models; });
        QTRY_VERIFY(listed.has_value());
        qputenv("FUGUE_HELPER", "");
        QFile::remove(providersPath());
        QVERIFY(listed->isEmpty());
        QVERIFY(logLines(m_log).isEmpty());
    }

    void aJobStartsTheHelperAndTheNextOneReusesIt() {
        auto helper = provider();
        QString job;
        QList<JobStatus> seen;
        const JobStatus status = run(*helper, encode({{"steps", 3}}), &job, &seen);
        QCOMPARE(status.state, QStringLiteral("done"));
        QVERIFY(seen.size() >= 3);
        QCOMPARE(seen.first().state, QStringLiteral("running"));
        QCOMPARE(seen.first().stage, QStringLiteral("sample"));
        QVERIFY(seen.at(seen.size() - 2).progress > seen.first().progress);

        const Result<JobResult> result =
            await<JobResult>([&](Reply<JobResult> reply) { helper->result(job, std::move(reply)); });
        QVERIFY(result.ok());
        const QString path = result->outputs.value("conditioning").toObject().value("path").toString();
        QCOMPARE(path, m_dir.filePath("conditioning.bin"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("prompt").toString(), QStringLiteral("a lighthouse"));

        const qint64 pid = helper->pid();
        QVERIFY(alive(pid));
        QCOMPARE(run(*helper, encode()).state, QStringLiteral("done"));
        QCOMPARE(helper->pid(), pid);
        QCOMPARE(logLines(m_log).filter("start").size(), 1);
    }

    void anIdleHelperIsStoppedAndStartedAgainWhenNeeded() {
        auto helper = provider(0.2);
        QCOMPARE(run(*helper, encode()).state, QStringLiteral("done"));
        const qint64 pid = helper->pid();
        QVERIFY(alive(pid));
        QTRY_COMPARE_WITH_TIMEOUT(helper->pid(), qint64(0), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 3000);
        QVERIFY(logLines(m_log).contains("shutdown")); // asked, not killed

        QCOMPARE(run(*helper, encode()).state, QStringLiteral("done"));
        QVERIFY(helper->pid() != pid);
        QCOMPARE(logLines(m_log).filter("start").size(), 2);
    }

    void theIdleTimeoutMayBeTextAsTheSettingsPageStoresIt() {
        auto helper = provider(QStringLiteral("0.2"));
        QCOMPARE(run(*helper, encode()).state, QStringLiteral("done"));
        QTRY_COMPARE_WITH_TIMEOUT(helper->pid(), qint64(0), 3000);
    }

    void aBusyHelperIsNotStoppedForIdling() {
        auto helper = provider(0.05);
        QCOMPARE(run(*helper, encode({{"steps", 15}})).state, QStringLiteral("done")); // 300 ms > idle
        QCOMPARE(logLines(m_log).filter("start").size(), 1);
    }

    void outOfMemoryIsItsOwnErrorAndOthersAreNodeFailed() {
        auto helper = provider();
        const JobStatus oom = run(*helper, encode({{"fail", "out_of_memory"}}));
        QCOMPARE(oom.state, QStringLiteral("error"));
        QCOMPARE(oom.error.code, QStringLiteral("out_of_memory"));

        const JobStatus other = run(*helper, encode({{"fail", "input_invalid"}}));
        QCOMPARE(other.state, QStringLiteral("error"));
        QCOMPARE(other.error.code, QStringLiteral("node_failed"));
        QVERIFY(other.error.message.contains("the fake failed as asked"));
    }

    void aCrashFailsItsJobAndTheNextJobStartsANewHelper() {
        auto helper = provider();
        const JobStatus crashed = run(*helper, encode({{"crash", true}}));
        QCOMPARE(crashed.state, QStringLiteral("error"));
        QCOMPARE(crashed.error.code, QStringLiteral("node_failed"));
        QVERIFY2(crashed.error.message.contains("crashing as asked"), qPrintable(crashed.error.message));
        QCOMPARE(helper->pid(), qint64(0));

        QCOMPARE(run(*helper, encode()).state, QStringLiteral("done"));
        QCOMPARE(logLines(m_log).filter("start").size(), 2);
    }

    void cancelStopsARunningJob() {
        auto helper = provider();
        QString job;
        JobStatus last;
        connect(helper.get(), &Provider::statusChanged, this, [&](const QString &id, const JobStatus &status) {
            if (id == job)
                last = status;
        });
        job = await<QString>([&](Reply<QString> reply) { helper->submit(encode({{"hold", true}}), std::move(reply)); })
                  .value();
        QTRY_COMPARE(last.state, QStringLiteral("running"));
        const qint64 pid = helper->pid();
        QVERIFY(await<QString>([&](Reply<QString> reply) { helper->cancel(job, std::move(reply)); }).ok());
        QTRY_COMPARE(last.state, QStringLiteral("cancelled"));
        QCOMPARE(helper->pid(), pid); // the helper stopped the job itself
    }

    void aJobThatDoesNotHearCancelIsEndedByStoppingTheHelper() {
        auto helper = provider();
        QString job;
        JobStatus last;
        connect(helper.get(), &Provider::statusChanged, this, [&](const QString &id, const JobStatus &status) {
            if (id == job)
                last = status;
        });
        job = await<QString>([&](Reply<QString> reply) {
                  helper->submit(encode({{"hold", true}, {"ignore_cancel", true}}), std::move(reply));
              }).value();
        QTRY_COMPARE(last.state, QStringLiteral("running"));
        const qint64 pid = helper->pid();
        QVERIFY(await<QString>([&](Reply<QString> reply) { helper->cancel(job, std::move(reply)); }).ok());
        QTRY_COMPARE_WITH_TIMEOUT(last.state, QStringLiteral("cancelled"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 3000);
    }

    void shutdownStopsTheHelperNow() {
        auto helper = provider();
        QCOMPARE(run(*helper, encode()).state, QStringLiteral("done"));
        const qint64 pid = helper->pid();
        QVERIFY(await<Nothing>([&](Reply<Nothing> reply) { helper->shutdown(std::move(reply)); }).ok());
        QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 3000);
        QCOMPARE(helper->pid(), qint64(0));
    }

    void withNoHelperInstalledEveryOperationSaysSo() {
        auto helper = provider(60, QString());
        const Result<Description> description =
            await<Description>([&](Reply<Description> reply) { helper->describe(std::move(reply)); });
        QVERIFY(!description.ok());
        QCOMPARE(description.errors().first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(description.errors().first().hint.contains("fugue-helper"));
        const JobStatus status = run(*helper, encode());
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));
    }

    void aHelperThatCannotStartFailsTheSubmit() {
        auto helper = provider(60, m_dir.filePath("no-such-helper"));
        const JobStatus status = run(*helper, encode());
        QCOMPARE(status.state, QStringLiteral("error"));
        QCOMPARE(status.error.code, QStringLiteral("provider_unreachable"));

        QFile notAProgram(m_dir.filePath("not-a-program"));
        QVERIFY(notAProgram.open(QIODevice::WriteOnly));
        notAProgram.close();
        auto other = provider(60, notAProgram.fileName());
        const JobStatus refused = run(*other, encode());
        QCOMPARE(refused.error.code, QStringLiteral("provider_unreachable"));
        QCOMPARE(other->pid(), qint64(0));
    }

    // By hand, on a machine with a GPU and models (helper/README.md): the real helper through
    // the provider, encode -> sample -> decode, then a second sample on the models still loaded.
    // FUGUE_REAL_HELPER names the helper, FUGUE_REAL_HELPER_REQUEST a JSON file of the params
    // (with their "context").
    void aRealHelperRunsTheStagesWarm() {
        const QString program = qEnvironmentVariable("FUGUE_REAL_HELPER");
        const QString requestFile = qEnvironmentVariable("FUGUE_REAL_HELPER_REQUEST");
        if (program.isEmpty() || requestFile.isEmpty())
            QSKIP("set FUGUE_REAL_HELPER and FUGUE_REAL_HELPER_REQUEST to run the real helper");
        QFile file(requestFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject params = QJsonDocument::fromJson(file.readAll()).object();
        auto helper = provider(60, program);
        const auto stage = [&](const QString &capability, const QJsonObject &inputs, const QString &output,
                               qint64 seed) -> QString {
            JobRequest request;
            request.capability = capability;
            request.params = params;
            request.params.insert("seed", seed);
            request.inputs = inputs;
            request.outputDir = m_dir.filePath(capability + QString::number(seed));
            QDir().mkpath(request.outputDir);
            QElapsedTimer clock;
            clock.start();
            QString job;
            const JobStatus status = run(*helper, request, &job, nullptr, 300000);
            if (status.state != QLatin1String("done"))
                return QStringLiteral("failed: ") + status.error.message;
            const auto result = await<JobResult>([&](Reply<JobResult> reply) { helper->result(job, std::move(reply)); });
            const QString path = result->outputs.value(output).toObject().value("path").toString();
            qInfo("%s in %.1f s: %s", qPrintable(capability), clock.elapsed() / 1000.0, qPrintable(path));
            return path;
        };
        const QString conditioning = stage("text.encode", {}, "conditioning", 42);
        QVERIFY2(QFileInfo::exists(conditioning), qPrintable(conditioning));
        const QString latent = stage("latent.sample", {{"conditioning", QJsonObject{{"path", conditioning}}}}, "latent", 42);
        QVERIFY2(QFileInfo::exists(latent), qPrintable(latent));
        const QString image = stage("latent.decode", {{"latent", QJsonObject{{"path", latent}}}}, "image", 42);
        QVERIFY2(QFileInfo::exists(image), qPrintable(image));
        const qint64 pid = helper->pid();
        const QString again = stage("latent.sample", {{"conditioning", QJsonObject{{"path", conditioning}}}}, "latent", 7);
        QVERIFY2(QFileInfo::exists(again), qPrintable(again));
        QCOMPARE(helper->pid(), pid); // one helper, warm
        if (qEnvironmentVariableIsSet("FUGUE_REAL_HELPER_KEEP"))
            QFile::copy(image, qEnvironmentVariable("FUGUE_REAL_HELPER_KEEP"));
    }

    // #173, spec 14.4: staged inpainting equals one `generate` run, byte for byte. By hand on a
    // GPU, as the test above: the stages through the provider (text.encode, latent.encode of the
    // picture, latent.sample with the mask, the picture and the start latent, latent.decode), then
    // the one-shot helper's `generate` with params.init_image and the mask. FUGUE_REAL_HELPER names
    // the helper, FUGUE_REAL_HELPER_REQUEST a JSON file of the params with their "context" (a
    // width and height too), FUGUE_REAL_HELPER_IMAGE a PNG of that size and FUGUE_REAL_HELPER_MASK
    // a gray PNG of it. Run for FLUX.2 Klein 4B (a start latent and the mask per step) and for an
    // inpaint model (FLUX.1 Fill, SDXL Inpaint: the picture, and the c_concat hook).
    void aRealHelperInpaintsStagedAsInOneRun() {
        const QString program = qEnvironmentVariable("FUGUE_REAL_HELPER");
        const QString requestFile = qEnvironmentVariable("FUGUE_REAL_HELPER_REQUEST");
        const QString picture = qEnvironmentVariable("FUGUE_REAL_HELPER_IMAGE");
        const QString mask = qEnvironmentVariable("FUGUE_REAL_HELPER_MASK");
        if (program.isEmpty() || requestFile.isEmpty() || picture.isEmpty() || mask.isEmpty())
            QSKIP("set FUGUE_REAL_HELPER, _REQUEST, _IMAGE and _MASK to run the real helper");
        QFile file(requestFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject params = QJsonDocument::fromJson(file.readAll()).object();
        params.insert("seed", 42);
        auto helper = provider(60, program);
        const auto stage = [&](const QString &capability, const QJsonObject &inputs, const QString &output) -> QString {
            JobRequest request;
            request.capability = capability;
            request.params = params;
            request.inputs = inputs;
            request.outputDir = m_dir.filePath(capability);
            QDir().mkpath(request.outputDir);
            QString job;
            const JobStatus status = run(*helper, request, &job, nullptr, 600000);
            if (status.state != QLatin1String("done"))
                return QStringLiteral("failed: ") + status.error.message;
            const auto result = await<JobResult>([&](Reply<JobResult> reply) { helper->result(job, std::move(reply)); });
            return result->outputs.value(output).toObject().value("path").toString();
        };
        const auto path = [](const QString &file) { return QJsonObject{{"path", file}}; };
        const QString conditioning = stage("text.encode", {}, "conditioning");
        QVERIFY2(QFileInfo::exists(conditioning), qPrintable(conditioning));
        const QString start = stage("latent.encode", {{"image", path(picture)}}, "latent");
        QVERIFY2(QFileInfo::exists(start), qPrintable(start));
        const QString sampled = stage("latent.sample", {{"conditioning", path(conditioning)}, {"init_latent", path(start)},
                                                        {"image", path(picture)}, {"mask", path(mask)}}, "latent");
        QVERIFY2(QFileInfo::exists(sampled), qPrintable(sampled));
        const QString staged = stage("latent.decode", {{"latent", path(sampled)}}, "image");
        QVERIFY2(QFileInfo::exists(staged), qPrintable(staged));

        // One generate run of the same build: the one-shot helper (no warm models to disturb).
        QJsonObject generate = params;
        const QJsonObject context = generate.take("context").toObject();
        generate.insert("init_image", QJsonObject{{"image", picture}});
        const QString single = m_dir.filePath("single.png");
        QFile requestOut(m_dir.filePath("generate.json"));
        QVERIFY(requestOut.open(QIODevice::WriteOnly));
        requestOut.write(QJsonDocument(QJsonObject{{"op", "generate"}, {"image", single}, {"mask", mask},
                                                   {"params", generate}, {"context", context}}).toJson());
        requestOut.close();
        helper.reset(); // free the GPU first
        QProcess oneShot;
        oneShot.start(program, {requestOut.fileName()});
        QVERIFY(oneShot.waitForFinished(600000));
        const auto bytes = [](const QString &name) {
            QFile in(name);
            return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
        };
        QVERIFY2(!bytes(single).isEmpty(), oneShot.readAllStandardOutput().constData());
        QCOMPARE(bytes(staged), bytes(single));
        if (qEnvironmentVariableIsSet("FUGUE_REAL_HELPER_KEEP"))
            QFile::copy(staged, qEnvironmentVariable("FUGUE_REAL_HELPER_KEEP"));
    }

    void theProgramIsTheCudaBuildOnNvidiaElseVulkan() {
        QTemporaryDir lib;
        const auto touch = [&](const QString &name) {
            QFile file(lib.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        touch("fugue-helper-vulkan");
        QCOMPARE(helperProgram({lib.path()}, true), lib.filePath("fugue-helper-vulkan"));
        touch("fugue-helper-cuda");
        QCOMPARE(helperProgram({lib.path()}, true), lib.filePath("fugue-helper-cuda"));
        QCOMPARE(helperProgram({lib.path()}, false), lib.filePath("fugue-helper-vulkan"));
        QCOMPARE(helperProgram({m_dir.filePath("none")}, true), QString());
    }
};

QTEST_GUILESS_MAIN(TestHelperProvider)
#include "test_helper_provider.moc"
