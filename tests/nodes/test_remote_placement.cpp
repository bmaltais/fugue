// Whether a run whose models are not here can succeed on the paired helper (#491), told from the document before anything runs:
// the nodes that cannot go there are named. No disk, no provider. Seam: engine/remote_placement.h.

#include <QJsonDocument>
#include <QtTest>

#include "engine/remote_placement.h"
#include "nodes/builtin.h"

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

Registry &registry() {
    static Registry made;
    static const bool filled = (nodes::addBuiltinNodeTypes(made), true);
    Q_UNUSED(filled)
    return made;
}

RemotePlacement placement(bool paired = true) {
    RemotePlacement made;
    made.registry = &registry();
    made.instances = {instance("desk", "omarchy")};
    if (paired)
        made.instances << instance("gx10", "fugue-helper", "http://gx10.example:8787");
    made.capabilityOf = [](const QString &type) {
        static const QMap<QString, QString> capabilities{{"fugue/text-encode", "text.encode"}, {"fugue/sampler", "latent.sample"},
                                                         {"fugue/vae-decode", "latent.decode"}, {"fugue/upscale-with-model", "image.upscale"}};
        return capabilities.value(type);
    };
    return made;
}

} // namespace

class TestRemotePlacement : public QObject {
    Q_OBJECT

private slots:
    void aNodeTheHostCanTakeIsNotRefused() {
        QVERIFY(remoteRunProblems(workflow(), missing(), placement()).isEmpty());
        RemotePlacement offered = placement();
        offered.hostCapabilities = {{"gx10", {"text.encode"}}};
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
        images.hostCapabilities = {{"gx10", {"image.generate"}}};
        const Errors problems = remoteRunProblems(workflow(), missing(), images);
        QCOMPARE(problems.size(), 1);
        QCOMPARE(problems.first().node, QStringLiteral("enc"));
        QVERIFY(problems.first().message.contains("gx10 does not offer text.encode"));
    }

    // The scheduler decides per node, from the node's own Resource inputs: a model reaches the sampler, not what reads the picture it makes.
    void onlyTheNodesThatHoldTheModelThemselvesAreChecked() {
        const QByteArray json = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
          "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "%1", "name": "m.safetensors"}}},
          "enc": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}, "prompt": "a cat"}},
          "samp": {"type": "fugue/sampler", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}, "conditioning": {"from": "enc.conditioning"}}},
          "dec": {"type": "fugue/vae-decode", "version": "1.0.0", "inputs": {"latent": {"from": "samp.latent"}}},
          "up": {"type": "fugue/upscale-with-model", "version": "1.0.0", "provider": "desk", "inputs": {"image": {"from": "dec.image"}}}}}})").arg(hash).toUtf8();
        const Result<Document> loaded = Document::fromJson(QJsonDocument::fromJson(json).object());
        QVERIFY(loaded);
        QVERIFY(remoteRunProblems(loaded.value(), missing(), placement()).isEmpty()); // up runs here, with its own model
        RemotePlacement images = placement();
        images.hostCapabilities = {{"gx10", {"text.encode"}}}; // no latent.sample: samp (which holds the model) is refused, dec and up are not
        const Errors problems = remoteRunProblems(loaded.value(), missing(), images);
        QCOMPARE(problems.size(), 1);
        QCOMPARE(problems.first().node, QStringLiteral("samp"));
    }

    // Every paired helper counts: refused only when none offers the capability, and a pin to the second is judged by the second.
    void everyPairedHelperIsAsked() {
        RemotePlacement two = placement();
        two.instances << instance("gx11", "fugue-helper", "http://gx11.example:8787");
        two.hostCapabilities = {{"gx10", {"image.generate"}}, {"gx11", {"text.encode"}}};
        QVERIFY(remoteRunProblems(workflow(), missing(), two).isEmpty());
        QVERIFY(remoteRunProblems(workflow("gx11"), missing(), two).isEmpty());
        QCOMPARE(remoteRunProblems(workflow("gx10"), missing(), two).size(), 1);
        two.hostCapabilities = {{"gx10", {"image.generate"}}, {"gx11", {"image.generate"}}};
        QCOMPARE(remoteRunProblems(workflow(), missing(), two).size(), 1);
        two.hostCapabilities = {{"gx10", {"image.generate"}}}; // gx11 is not known: it answers for itself
        QVERIFY(remoteRunProblems(workflow(), missing(), two).isEmpty());
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
