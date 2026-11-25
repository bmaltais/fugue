// Whether a run whose models are not here can succeed on the paired helper (#491), told from the document before anything runs:
// the nodes that cannot go there are named. No disk, no provider. Seam: engine/remote_placement.h.

#include <QJsonDocument>
#include <QtTest>

#include "engine/remote_placement.h"

using namespace fugue;

namespace {

const QString hash = "sha256:" + QString(64, QLatin1Char('c'));

// A loader holding the model, a stage node reading it (unpinned or pinned), and a node that is no provider node.
Document workflow(const QString &pin = {}, const QString &graphPin = {}) {
    const QByteArray json = QStringLiteral(R"({"fugue": 1, "graph": {%1"nodes": {
      "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "%2", "name": "m.safetensors"}}},
      "enc": {"type": "fugue/text-encode", "version": "1.0.0" %3, "inputs": {"model": {"from": "models.model"}, "prompt": "a cat"}},
      "num": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 1}}}}})")
                                .arg(graphPin.isEmpty() ? QString() : QStringLiteral("\"provider\": \"%1\", ").arg(graphPin), hash,
                                     pin.isEmpty() ? QString() : QStringLiteral(", \"provider\": \"%1\"").arg(pin))
                                .toUtf8();
    const Result<Document> loaded = Document::fromJson(QJsonDocument::fromJson(json).object());
    if (!loaded)
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

ReadinessPlan missing() {
    ReadinessModel model;
    model.name = QStringLiteral("m.safetensors");
    model.entry = QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", model.name}};
    ReadinessPlan plan;
    plan.models << model;
    return plan;
}

ProviderInstance instance(const QString &name, const QString &type, const QString &address = {}) {
    ProviderInstance made;
    made.name = name;
    made.type = type;
    made.address = address;
    return made;
}

RemotePlacement placement(bool paired = true) {
    RemotePlacement made;
    made.instances = {instance("desk", "omarchy")};
    if (paired)
        made.instances << instance("gx10", "fugue-helper", "http://gx10.example:8787");
    made.capabilityOf = [](const QString &type) { return type == QLatin1String("fugue/text-encode") ? QStringLiteral("text.encode") : QString(); };
    return made;
}

} // namespace

class TestRemotePlacement : public QObject {
    Q_OBJECT

private slots:
    void aNodeTheHostCanTakeIsNotRefused() {
        QVERIFY(remoteRunProblems(workflow(), missing(), placement()).isEmpty());
        RemotePlacement offered = placement();
        offered.hostCapabilities = QStringList{"text.encode"};
        QVERIFY(remoteRunProblems(workflow(), missing(), offered).isEmpty());
        QVERIFY(remoteRunProblems(workflow("gx10"), missing(), offered).isEmpty()); // pinned to the host itself
    }

    void aNodePinnedToAProviderOnThisMachineIsNamed() {
        for (const Document &pinned : {workflow("desk"), workflow({}, "desk")}) { // its own pin, or the workflow's
            const Errors problems = remoteRunProblems(pinned, missing(), placement());
            QCOMPARE(problems.size(), 1);
            QCOMPARE(problems.first().code, QStringLiteral("model_missing"));
            QCOMPARE(problems.first().node, QStringLiteral("enc"));
            QVERIFY(problems.first().message.contains("pinned to desk"));
            QVERIFY(problems.first().message.contains("m.safetensors"));
        }
        // A node's own pin wins over the workflow's.
        QVERIFY(remoteRunProblems(workflow("gx10", "desk"), missing(), placement()).isEmpty());
    }

    void aNodeTheHostDoesNotOfferTheCapabilityOfIsNamed() {
        RemotePlacement images = placement();
        images.hostCapabilities = QStringList{"image.generate"};
        const Errors problems = remoteRunProblems(workflow(), missing(), images);
        QCOMPARE(problems.size(), 1);
        QCOMPARE(problems.first().node, QStringLiteral("enc"));
        QVERIFY(problems.first().message.contains("gx10 does not offer text.encode"));
    }

    void aModelWithNoSourceCountsAsMissingToo() {
        ReadinessPlan plan;
        plan.unobtainable << QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "m.safetensors"}};
        QCOMPARE(remoteRunProblems(workflow("desk"), plan, placement()).size(), 1);
    }

    void nothingChangesWithoutAHelperOrAMissingModelOrAKernelMap() {
        QVERIFY(remoteRunProblems(workflow("desk"), missing(), placement(false)).isEmpty());
        QVERIFY(remoteRunProblems(workflow("desk"), ReadinessPlan(), placement()).isEmpty());
        RemotePlacement unknown = placement();
        unknown.capabilityOf = {};
        QVERIFY(remoteRunProblems(workflow("desk"), missing(), unknown).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestRemotePlacement)
#include "test_remote_placement.moc"
