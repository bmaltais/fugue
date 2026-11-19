// The provider interface (spec 8.11) through the fake provider other engine tests use.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/fake_provider.h"
#include "provider_support.h"

using namespace fugue;
using fugue::test::await;

namespace {

Description imageDescription() {
    OfferedModel model;
    model.handle = QJsonObject{{"resource", "fugue/image-model"}, {"name", "fake-model"}};
    model.status = QStringLiteral("ready");
    Description description;
    description.engine = QStringLiteral("fake");
    description.capabilities.append(CapabilityOffer{QStringLiteral("image.generate"), {model}});
    return description;
}

} // namespace

class TestFakeProvider : public QObject {
    Q_OBJECT

private slots:
    void describeAnswersWithItsDescription() {
        FakeProvider provider(imageDescription());
        const auto described = await<Description>([&](auto reply) { provider.describe(reply); });
        QVERIFY(described.ok());
        QCOMPARE(described.value().engine, QStringLiteral("fake"));
        QVERIFY(described.value().offer("image.generate"));
        QVERIFY(!described.value().offer("image.edit"));
        QCOMPARE(described.value().offer("image.generate")->status(), QStringLiteral("ready"));
    }

    void repliesNeverArriveInsideTheCall() {
        FakeProvider provider(imageDescription());
        bool replied = false;
        provider.describe([&](const Result<Description> &) { replied = true; });
        QVERIFY(!replied);
        QTRY_VERIFY(replied);
    }

    void finishedJobHasItsFilesInTheCallersDirectory() {
        QTemporaryDir dir;
        FakeProvider provider(imageDescription());
        QList<JobStatus> stream;
        connect(&provider, &Provider::statusChanged, this,
                [&](const QString &, const JobStatus &status) { stream.append(status); });

        JobRequest request;
        request.capability = QStringLiteral("image.generate");
        request.params = QJsonObject{{"prompt", "a cat"}, {"seed", 7}};
        request.outputDir = dir.path();
        const auto submitted = await<QString>([&](auto reply) { provider.submit(request, reply); });
        QVERIFY(submitted.ok());
        const QString job = submitted.value();
        QCOMPARE(provider.submitted(job).params.value("prompt").toString(), QStringLiteral("a cat"));

        provider.progress(job, 0.5, QStringLiteral("sampling"));
        provider.finish(job, {{"image.png", QByteArray("pixels")}}, QJsonObject{{"seed", 7}});
        QTRY_COMPARE(stream.size(), 2);
        QCOMPARE(stream.at(0).state, QStringLiteral("running"));
        QCOMPARE(stream.at(0).progress, 0.5);
        QCOMPARE(stream.at(1).state, QStringLiteral("done"));
        QVERIFY(stream.at(1).isTerminal());

        const auto result = await<JobResult>([&](auto reply) { provider.result(job, reply); });
        QVERIFY(result.ok());
        const QString path = result.value().outputs.value("image").toObject().value("path").toString();
        QCOMPARE(path, QDir(dir.path()).filePath("image.png"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("pixels"));
        QCOMPARE(result.value().used.value("seed").toInt(), 7);
    }

    void cancelEndsTheJobCancelled() {
        QTemporaryDir dir;
        FakeProvider provider(imageDescription());
        JobRequest request;
        request.capability = QStringLiteral("image.generate");
        request.outputDir = dir.path();
        const QString job = await<QString>([&](auto reply) { provider.submit(request, reply); }).value();
        const auto cancelled = await<QString>([&](auto reply) { provider.cancel(job, reply); });
        QCOMPARE(cancelled.value(), QStringLiteral("cancelled"));
        const auto status = await<JobStatus>([&](auto reply) { provider.status(job, reply); });
        QCOMPARE(status.value().state, QStringLiteral("cancelled"));
        const auto result = await<JobResult>([&](auto reply) { provider.result(job, reply); });
        QVERIFY(!result.ok());
    }

    void describeCanFailLikeAnUnreachableProvider() {
        FakeProvider provider(imageDescription());
        provider.setDescribeErrors({makeError("provider_unreachable", "Nothing answers.")});
        const auto described = await<Description>([&](auto reply) { provider.describe(reply); });
        QVERIFY(!described.ok());
        QCOMPARE(described.errors().first().code, QStringLiteral("provider_unreachable"));
    }
};

QTEST_GUILESS_MAIN(TestFakeProvider)
#include "test_fake_provider.moc"
