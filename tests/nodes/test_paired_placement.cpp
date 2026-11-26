// Where a run whose models are not here goes (#491, #581): per node the host that takes it, the nodes that cannot go anywhere and
// why, and the host that does not answer. No disk, no provider; the probe is a fake. Seam: engine/paired_placement.h.

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

#include "engine/compiler.h"
#include "engine/group_library.h"
#include "engine/paired_placement.h"
#include "engine/pack_loader.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

const QString hash = "sha256:" + QString(64, QLatin1Char('c'));
// The rest of what Load Models needs, which are here: the workflow must compile, as the scheduler would have it.
const QString others = QStringLiteral(R"("text_encoder": {"resource": "fugue/text-encoder", "hash": "sha256:%1", "name": "t.safetensors"}, "vae": {"resource": "fugue/vae", "hash": "sha256:%2", "name": "v.safetensors"})")
                           .arg(QString(64, QLatin1Char('d')), QString(64, QLatin1Char('e')));

// A loader holding the model, a stage node reading it (unpinned or pinned), and a node that is no provider node.
Document workflow(const QString &pin = {}, const QString &graphPin = {}) {
    const QByteArray json = QStringLiteral(R"({"fugue": 1, "graph": {%1"nodes": {
      "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "%2", "name": "m.safetensors"}, %4}},
      "enc": {"type": "fugue/text-encode", "version": "1.0.0" %3, "inputs": {"model": {"from": "models.model"}, "prompt": "a cat"}},
      "num": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 1}},
      "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "enc.conditioning"}}}}}})")
                                .arg(graphPin.isEmpty() ? QString() : QStringLiteral("\"provider\": \"%1\", ").arg(graphPin), hash,
                                     pin.isEmpty() ? QString() : QStringLiteral(", \"provider\": \"%1\"").arg(pin), others)
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

// What the last text-encode asked with, and how a test makes the requirement depend on it.
Values &seenInputs() {
    static Values seen;
    return seen;
}
std::function<QJsonObject(const Values &)> &propertiesOf() {
    static std::function<QJsonObject(const Values &)> made = [](const Values &) { return QJsonObject(); };
    return made;
}

PairedPlacementRequest request(bool paired = true) {
    PairedPlacementRequest made;
    made.registry = &registry();
    made.instances = {instance("desk", "omarchy")};
    if (paired)
        made.instances << instance("gx10", "fugue-helper", "http://gx10.example.ts.net:8787");
    // What the stage kernels ask for, with the inputs a node holds (the real kernels are read by the scheduler's own tests).
    made.requirementOf = [](const NodeType &type, const Values &inputs) -> std::optional<Requirement> {
        static const QMap<QString, QString> capabilities{{"fugue/text-encode", "text.encode"}, {"fugue/sampler", "latent.sample"},
                                                         {"fugue/vae-decode", "latent.decode"}, {"fugue/upscale-with-model", "image.upscale"}};
        if (!capabilities.contains(type.id))
            return std::nullopt;
        Requirement requirement;
        requirement.capability = capabilities.value(type.id);
        if (type.id == QLatin1String("fugue/text-encode"))
            seenInputs() = inputs;
        requirement.properties = propertiesOf()(inputs);
        return requirement;
    };
    made.remembered = [](const ProviderInstance &) { return std::optional<Description>(); }; // nothing kept: no disk
    return made;
}

// A host's description: one ready model with no handle per capability (a helper takes its model files in the job, 8.10), of these
// properties, and whether it reads this machine's files (a paired host does not).
Description offering(const QStringList &capabilities, const QJsonObject &properties = {}, bool sharesFiles = false) {
    Description made;
    made.sharesFilesystem = sharesFiles;
    for (const QString &capability : capabilities) {
        OfferedModel model;
        model.status = ModelStatus::Ready;
        model.properties = properties;
        made.capabilities << CapabilityOffer{capability, {model}};
    }
    return made;
}

// The probe is a fake: the hosts named in `offers` answer with these capabilities, the others are silent.
struct FakeHosts {
    QMap<QString, QStringList> offers;
    QJsonObject properties;  // of every model they offer
    bool sharesFiles = false; // they read this machine's files
    int asked = 0;

    FakeHosts() = default;
    explicit FakeHosts(QMap<QString, QStringList> answering) : offers(std::move(answering)) {}

    PairedProbe probe() {
        return [this](const QList<ProviderInstance> &helpers, std::function<void(const QMap<QString, Description> &)> done) {
            ++asked;
            QMap<QString, Description> answered;
            for (const ProviderInstance &helper : helpers) {
                if (offers.contains(helper.name))
                    answered.insert(helper.name, offering(offers.value(helper.name), properties, sharesFiles));
            }
            done(answered);
        };
    }
};

PairedVerdict verdict(const Document &document, const ReadinessPlan &plan, const PairedPlacementRequest &asked, FakeHosts &hosts) {
    std::optional<PairedVerdict> got;
    decidePairedPlacement(document, plan, asked, hosts.probe(), [&got](const PairedVerdict &made) { got = made; });
    if (!got)
        qFatal("the verdict did not come");
    return *got;
}

} // namespace

class TestPairedPlacement : public QObject {
    Q_OBJECT

private slots:
    void aHostThatAnswersAndOffersTheCapabilityTakesTheNode() {
        FakeHosts hosts({{"gx10", {"text.encode"}}});
        const PairedVerdict made = verdict(workflow(), missing(), request(), hosts);
        QVERIFY(made.applies && made.runsOn() && made.allowsRun());
        QCOMPARE(made.hostOf, (QMap<QString, QString>{{"enc", "gx10"}}));
        QCOMPARE(made.hosts(), QStringList{"gx10"});
        QVERIFY(made.blocked.isEmpty() && made.silent.isEmpty());
        QCOMPARE(hosts.asked, 1);
        FakeHosts pinned({{"gx10", {"text.encode"}}});
        QCOMPARE(verdict(workflow("gx10"), missing(), request(), pinned).hostOf.value("enc"), QStringLiteral("gx10")); // pinned to the host itself
    }

    // #587: a required input left for the app's own values (`--input`) does not stop the placement, as it does not stop the run.
    void aRequiredInputLeftUnsetIsNoReasonToPlaceNothing() {
        Document document = workflow();
        document.graph.nodes["enc"].inputs.remove("prompt");
        QVERIFY(!compile(document, registry())); // the plain compile refuses it: missing_input
        FakeHosts hosts({{"gx10", {"text.encode"}}});
        const PairedVerdict made = verdict(document, missing(), request(), hosts);
        QCOMPARE(made.hostOf, (QMap<QString, QString>{{"enc", "gx10"}}));
        QVERIFY(made.runsOn());
    }

    // #581: a host that is silent is never named as the host; the run may still start (a cached re-run works).
    void aSilentHostIsNotNamedAndDoesNotStopTheRun() {
        FakeHosts hosts;
        const PairedVerdict made = verdict(workflow(), missing(), request(), hosts);
        QCOMPARE(made.silent, QStringList{"gx10"});
        QVERIFY(made.hostOf.isEmpty() && made.blocked.isEmpty());
        QVERIFY(!made.runsOn());
        QVERIFY(made.allowsRun());
    }

    // #584: only a host that does not read this machine's files takes a node whose model files are not here.
    void aHostThatSharesThisMachinesFilesIsNoPlaceForTheModel() {
        FakeHosts hosts({{"gx10", {"text.encode"}}});
        hosts.sharesFiles = true;
        const PairedVerdict made = verdict(workflow(), missing(), request(), hosts);
        QVERIFY(made.hostOf.isEmpty() && made.silent.isEmpty());
        QCOMPARE(made.blocked.size(), 1);
        QCOMPARE(made.blocked.first().node, QStringLiteral("enc"));
        QVERIFY(!made.allowsRun());
    }

    // #584: the requirement is the kernel's for the inputs the node holds (a literal, a default), and its properties must be the
    // model's: a host whose model lacks them cannot take the node, one that has them can.
    void theRequirementIsReadWithTheNodesInputsAndItsPropertiesMustHold() {
        propertiesOf() = [](const Values &inputs) { return QJsonObject{{"prompt", inputs.value("prompt").text()}}; };
        const auto reset = qScopeGuard([] { propertiesOf() = [](const Values &) { return QJsonObject(); }; });
        FakeHosts fits({{"gx10", {"text.encode"}}});
        fits.properties = QJsonObject{{"prompt", "a cat"}};
        QCOMPARE(verdict(workflow(), missing(), request(), fits).hostOf.value("enc"), QStringLiteral("gx10"));
        QCOMPARE(seenInputs().value("prompt").text(), QStringLiteral("a cat"));          // a literal the node holds
        QCOMPARE(seenInputs().value("edit_format").text(), QStringLiteral("default"));   // the spec's default for one it does not
        QVERIFY(seenInputs().value("model").isEmpty());                                  // a link's value is not known before the run
        FakeHosts lacks({{"gx10", {"text.encode"}}});
        lacks.properties = QJsonObject{{"prompt", "a dog"}};
        const PairedVerdict refused = verdict(workflow(), missing(), request(), lacks);
        QCOMPARE(refused.blocked.size(), 1);
        QVERIFY(refused.hostOf.isEmpty() && refused.silent.isEmpty());
    }

    // #584: the scheduler runs the compiled graph, so a node inside a group instance is placed (under its path id), reads the inputs
    // the instance gives it, and takes the instance's pin.
    void aNodeInsideAGroupInstanceIsPlacedLikeAnyOther() {
        const auto grouped = [](const QString &pin) {
            const QByteArray json = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
              "g": {"type": "local/enc", "version": "1.0.0" %3, "inputs": {"prompt": "from the instance"}},
              "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "g.conditioning"}}}}},
              "groups": {"local/enc": {"version": "1.0.0", "name": "Enc",
                "inputs": [{"name": "prompt", "targets": ["enc.prompt"]}],
                "outputs": [{"name": "conditioning", "from": "enc.conditioning"}],
                "nodes": {"models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "%1", "name": "m.safetensors"}, %2}},
                          "enc": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}}}}}}})")
                                        .arg(hash, others, pin.isEmpty() ? QString() : QStringLiteral(", \"provider\": \"%1\"").arg(pin))
                                        .toUtf8();
            const Result<Document> loaded = Document::fromJson(QJsonDocument::fromJson(json).object());
            if (!loaded)
                qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
            return loaded.value();
        };
        FakeHosts hosts({{"gx10", {"text.encode"}}});
        const PairedVerdict placed = verdict(grouped({}), missing(), request(), hosts);
        QCOMPARE(placed.hostOf, (QMap<QString, QString>{{"g/enc", "gx10"}}));
        QCOMPARE(seenInputs().value("prompt").text(), QStringLiteral("from the instance"));
        FakeHosts none({{"gx10", {"image.generate"}}});
        const PairedVerdict refused = verdict(grouped({}), missing(), request(), none); // a model missing inside a group is not left out
        QCOMPARE(refused.blocked.size(), 1);
        QCOMPARE(refused.blocked.first().node, QStringLiteral("g/enc"));
        FakeHosts pinnedHere;
        const PairedVerdict pinned = verdict(grouped("desk"), missing(), request(), pinnedHere); // the instance's pin reaches the node in it
        QCOMPARE(pinnedHere.asked, 0);
        QCOMPARE(pinned.blocked.first().node, QStringLiteral("g/enc"));
        QVERIFY(pinned.blocked.first().message.contains("pinned to desk"));
        // A library group named only by its type id (no definition in `groups`) is placed as well: like a run, the verdict compiles it
        // from the library's definition.
        QTemporaryDir packs;
        QVERIFY(saveToLibrary(grouped({}), "local/enc", registry(), packs.filePath("local")).ok());
        Registry library;
        nodes::addBuiltinNodeTypes(library);
        loadPacks(library, {packs.path()});
        Document bare = grouped({});
        bare.groups.clear();
        PairedPlacementRequest fromLibrary = request();
        fromLibrary.registry = &library;
        QCOMPARE(verdict(bare, missing(), fromLibrary, hosts).hostOf, (QMap<QString, QString>{{"g/enc", "gx10"}}));
    }

    // The pins decide before the probe: an app they block never waits for a host.
    void aNodePinnedToAProviderOnThisMachineIsBlockedWithoutAsking() {
        for (const Document &pinned : {workflow("desk"), workflow({}, "desk")}) { // its own pin, or the workflow's
            FakeHosts hosts;
            const PairedVerdict made = verdict(pinned, missing(), request(), hosts);
            QCOMPARE(hosts.asked, 0);
            QCOMPARE(made.blocked.size(), 1);
            QCOMPARE(made.blocked.first().code, QStringLiteral("model_missing"));
            QCOMPARE(made.blocked.first().node, QStringLiteral("enc"));
            QVERIFY(made.blocked.first().message.contains("pinned to desk"));
            QVERIFY(made.blocked.first().message.contains("m.safetensors"));
            QVERIFY(!made.allowsRun() && !made.runsOn());
        }
        FakeHosts hosts({{"gx10", {"text.encode"}}});
        QVERIFY(verdict(workflow("gx10", "desk"), missing(), request(), hosts).runsOn()); // a node's own pin wins over the workflow's
    }

    void aHostThatAnswersWithoutTheCapabilityBlocksTheNode() {
        FakeHosts hosts({{"gx10", {"image.generate"}}});
        const PairedVerdict made = verdict(workflow(), missing(), request(), hosts);
        QCOMPARE(hosts.asked, 1);
        QCOMPARE(made.blocked.size(), 1);
        QCOMPARE(made.blocked.first().node, QStringLiteral("enc"));
        QVERIFY(made.blocked.first().message.contains("gx10 does not offer text.encode"));
        QVERIFY(made.hostOf.isEmpty() && made.silent.isEmpty());
    }

    // What a host last said never places a node and never overrules a live answer; it refuses only for a host that does not answer.
    void aRememberedAnswerOnlyRefusesForAHostThatIsSilent() {
        PairedPlacementRequest lacking = request();
        lacking.remembered = [](const ProviderInstance &) { return std::optional<Description>(offering({"image.generate"})); };
        FakeHosts gained({{"gx10", {"text.encode"}}}); // (a) it offers it now: the stale refusal is overturned
        const PairedVerdict now = verdict(workflow(), missing(), lacking, gained);
        QCOMPARE(gained.asked, 1);
        QCOMPARE(now.hostOf.value("enc"), QStringLiteral("gx10"));
        QVERIFY(now.blocked.isEmpty());

        FakeHosts silent; // (b) silent, and it never offered it: blocked
        const PairedVerdict refused = verdict(workflow(), missing(), lacking, silent);
        QCOMPARE(silent.asked, 1);
        QCOMPARE(refused.blocked.size(), 1);
        QVERIFY(refused.blocked.first().message.contains("gx10 did not offer text.encode when it last answered")); // the scheduler's words
        QVERIFY(refused.silent.isEmpty());

        FakeHosts quiet; // (c) silent, nothing remembered (or it offered it): silent, never the host
        const PairedVerdict none = verdict(workflow(), missing(), request(), quiet);
        QCOMPARE(none.silent, QStringList{"gx10"});
        QVERIFY(none.hostOf.isEmpty() && none.blocked.isEmpty());
        PairedPlacementRequest offered = request();
        offered.remembered = [](const ProviderInstance &) { return std::optional<Description>(offering({"text.encode"})); };
        FakeHosts quiet2;
        QCOMPARE(verdict(workflow(), missing(), offered, quiet2).silent, QStringList{"gx10"});
    }

    // The scheduler's own choice per node: the first helper in the user's order that answered, offers the capability and respects a pin.
    void severalHelpersAreChosenPerNodeInTheUsersOrder() {
        PairedPlacementRequest two = request();
        two.instances << instance("gx11", "fugue-helper", "http://gx11.example:8787");
        FakeHosts hosts({{"gx10", {"image.generate"}}, {"gx11", {"text.encode"}}});
        QCOMPARE(verdict(workflow(), missing(), two, hosts).hostOf.value("enc"), QStringLiteral("gx11")); // gx10 answered but lacks it
        FakeHosts both({{"gx10", {"text.encode"}}, {"gx11", {"text.encode"}}});
        QCOMPARE(verdict(workflow(), missing(), two, both).hostOf.value("enc"), QStringLiteral("gx10"));
        FakeHosts second({{"gx10", {"text.encode"}}, {"gx11", {"text.encode"}}});
        QCOMPARE(verdict(workflow("gx11"), missing(), two, second).hostOf.value("enc"), QStringLiteral("gx11")); // a pin to the second
        FakeHosts firstSilent({{"gx11", {"text.encode"}}});
        const PairedVerdict made = verdict(workflow(), missing(), two, firstSilent); // the first does not answer, the second does
        QCOMPARE(made.hostOf.value("enc"), QStringLiteral("gx11"));
        QVERIFY(made.runsOn() && made.silent.isEmpty());
        FakeHosts none({{"gx10", {"image.generate"}}, {"gx11", {"image.generate"}}});
        QCOMPARE(verdict(workflow(), missing(), two, none).blocked.size(), 1);
        FakeHosts oneLacks({{"gx10", {"image.generate"}}}); // gx11 is silent: it may still take it
        const PairedVerdict waiting = verdict(workflow(), missing(), two, oneLacks);
        QCOMPARE(waiting.silent, QStringList{"gx11"});
        QVERIFY(waiting.blocked.isEmpty());
        FakeHosts pinnedLacks({{"gx10", {"image.generate"}}, {"gx11", {"text.encode"}}});
        QCOMPARE(verdict(workflow("gx10"), missing(), two, pinnedLacks).blocked.size(), 1);
    }

    // The scheduler decides per node, from the node's own Resource inputs: a model reaches the sampler, not what reads the picture it makes.
    void onlyTheNodesThatHoldTheModelThemselvesAreChecked() {
        const QByteArray json = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
          "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "%1", "name": "m.safetensors"}, %2}},
          "enc": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}, "prompt": "a cat"}},
          "samp": {"type": "fugue/sampler", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}, "conditioning": {"from": "enc.conditioning"}, "latent": {"from": "lat.latent"}}},
          "dec": {"type": "fugue/vae-decode", "version": "1.0.0", "inputs": {"latent": {"from": "samp.latent"}}},
          "lat": {"type": "fugue/empty-latent", "version": "1.0.0", "inputs": {}},
          "up": {"type": "fugue/upscale-with-model", "version": "1.0.0", "provider": "desk", "inputs": {"image": {"from": "dec.image"}, "model": {"resource": "fugue/upscale-model", "hash": "sha256:%3", "name": "u.pth"}}},
          "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "up.image"}}}}}})").arg(hash, others, QString(64, QLatin1Char('f'))).toUtf8();
        const Result<Document> loaded = Document::fromJson(QJsonDocument::fromJson(json).object());
        QVERIFY(loaded);
        FakeHosts all({{"gx10", {"text.encode", "latent.sample"}}});
        const PairedVerdict placed = verdict(loaded.value(), missing(), request(), all); // up runs here, with its own model
        QVERIFY(placed.runsOn());
        QCOMPARE(placed.hostOf, (QMap<QString, QString>{{"enc", "gx10"}, {"samp", "gx10"}}));
        FakeHosts noSampler({{"gx10", {"text.encode"}}}); // samp (which holds the model) is blocked, dec and up are not
        const PairedVerdict blockedOne = verdict(loaded.value(), missing(), request(), noSampler);
        QCOMPARE(blockedOne.blocked.size(), 1);
        QCOMPARE(blockedOne.blocked.first().node, QStringLiteral("samp"));
        QVERIFY(!blockedOne.runsOn());
    }

    // #557: a node that only a lazy input reaches (a Switch's side) may never run, so its missing model blocks nothing; the one that
    // is also read through an input that is not lazy is certain to run and is still placed.
    void aNodeOnlyALazyInputReachesIsNotPlaced() {
        const QByteArray json = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
          "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "%1", "name": "m.safetensors"}, %2}},
          "a": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}, "prompt": "a cat"}},
          "b": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"model": {"from": "models.model"}, "prompt": "a dog"}},
          "pick": {"type": "fugue/switch", "version": "1.0.0", "inputs": {"condition": true, "on_true": {"from": "a.conditioning"}, "on_false": {"from": "b.conditioning"}}},
          "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "pick.value"}}},
          "also": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "b.conditioning"}}}}}})").arg(hash, others).toUtf8();
        const Result<Document> loaded = Document::fromJson(QJsonDocument::fromJson(json).object());
        QVERIFY(loaded);
        FakeHosts none({{"gx10", {"latent.sample"}}}); // nobody offers text.encode
        const PairedVerdict made = verdict(loaded.value(), missing(), request(), none);
        QCOMPARE(made.blocked.size(), 1);
        QCOMPARE(made.blocked.first().node, QStringLiteral("b")); // a sink reads it, so it always runs
    }

    // #557: the same rule for what a run is refused on (`fugue app run`, the Needs panel, Get ready): a model only a lazy input reaches
    // is not asked for; the one a node that is certain to run holds still is.
    void aModelOnlyALazyInputReachesIsNotInTheReadinessRequest() {
        const QString other = "sha256:" + QString(64, QLatin1Char('f'));
        const QByteArray json = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
          "m1": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "%1", "name": "m.safetensors"}, %3}},
          "m2": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "%2", "name": "n.safetensors"}, %3}},
          "a": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"model": {"from": "m1.model"}, "prompt": "a cat"}},
          "b": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"model": {"from": "m2.model"}, "prompt": "a dog"}},
          "pick": {"type": "fugue/switch", "version": "1.0.0", "inputs": {"condition": false, "on_true": {"from": "a.conditioning"}, "on_false": {"from": "b.conditioning"}}},
          "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "pick.value"}}},
          "also": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "b.conditioning"}}}}}})")
                                    .arg(hash, other, others).toUtf8();
        const Result<Document> loaded = Document::fromJson(QJsonDocument::fromJson(json).object());
        QVERIFY(loaded);
        QStringList hashes;
        for (const QJsonValue model : requestFor(loaded.value(), registry()).models)
            hashes << model.toObject().value("hash").toString();
        QVERIFY(!hashes.contains(hash));   // only a, which only the Switch's lazy side reads
        QVERIFY(hashes.contains(other));   // b is read by a sink too
    }

    void aModelWithNoSourceCountsAsMissingToo() {
        ReadinessPlan plan;
        plan.unobtainable << QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "m.safetensors"}};
        plan.problems << makeError(QStringLiteral("model_missing"), QStringLiteral("no source"));
        FakeHosts hosts;
        QCOMPARE(verdict(workflow("desk"), plan, request(), hosts).blocked.size(), 1);
    }

    // The gate: only models missing, and a helper to take them. Nothing is asked or said otherwise.
    void nothingIsPlacedUnlessOnlyModelsAreMissingAndAHelperIsConfigured() {
        FakeHosts hosts({{"gx10", {"text.encode"}}});
        ReadinessPlan withPack = missing();
        withPack.packs << ReadinessPack();
        for (const PairedVerdict &made : {verdict(workflow("desk"), withPack, request(), hosts),                    // a pack is missing too
                                          verdict(workflow("desk"), ReadinessPlan(), request(), hosts),            // nothing is missing
                                          verdict(workflow("desk"), missing(), request(false), hosts)}) {          // no paired helper
            QVERIFY(!made.applies && !made.allowsRun());
            QVERIFY(made.hostOf.isEmpty() && made.blocked.isEmpty() && made.silent.isEmpty());
        }
        PairedPlacementRequest unknown = request();
        unknown.requirementOf = {};
        QVERIFY(verdict(workflow("desk"), missing(), unknown, hosts).blocked.isEmpty()); // no kernel map: nothing is known of the nodes
        PairedPlacementRequest disabled = request();
        disabled.instances.last().enabled = false;
        QVERIFY(!verdict(workflow(), missing(), disabled, hosts).applies);
        QCOMPARE(hosts.asked, 0);
    }

    // The Needs panel's plan: the models are the host's when the app runs there; else the blocked nodes are listed; a silent host leaves it.
    void thePlanUnderTheVerdictMarksTheModelsOrListsTheBlockedNodes() {
        FakeHosts answering({{"gx10", {"text.encode"}}});
        const ReadinessPlan placed = planUnder(missing(), verdict(workflow(), missing(), request(), answering));
        QCOMPARE(placed.models.first().onHost, QStringLiteral("gx10"));
        QVERIFY(placed.problems.isEmpty());
        FakeHosts lacking({{"gx10", {"image.generate"}}});
        const ReadinessPlan blocked = planUnder(missing(), verdict(workflow(), missing(), request(), lacking));
        QVERIFY(blocked.models.first().onHost.isEmpty());
        QCOMPARE(blocked.problems.size(), 1);
        FakeHosts silent;
        const ReadinessPlan quiet = planUnder(missing(), verdict(workflow(), missing(), request(), silent));
        QVERIFY(quiet.models.first().onHost.isEmpty() && quiet.problems.isEmpty());
    }

    // The window asks again only when the key changes: not for the disk space the plan reads live, but for a model, a pin or a provider.
    void theKeyChangesWithWhatTheVerdictDependsOnAndNotWithFreeSpace() {
        const PairedPlacementRequest asked = request();
        const QByteArray base = pairedPlacementKey(workflow(), missing(), asked.instances);
        ReadinessPlan roomier = missing();
        roomier.freeBytes = 123456789;
        roomier.downloadBytes = 42;
        QCOMPARE(pairedPlacementKey(workflow(), roomier, asked.instances), base);
        QVERIFY(pairedPlacementKey(workflow(), ReadinessPlan(), asked.instances) != base);          // a model is no longer missing
        QVERIFY(pairedPlacementKey(workflow("desk"), missing(), asked.instances) != base);          // a pin
        QVERIFY(pairedPlacementKey(workflow({}, "desk"), missing(), asked.instances) != base);      // the workflow's pin
        QList<ProviderInstance> more = asked.instances;
        more << instance("gx11", "fugue-helper", "http://gx11.example:8787");
        QVERIFY(pairedPlacementKey(workflow(), missing(), more) != base);                           // a helper added
        more = asked.instances;
        more.last().enabled = false;
        QVERIFY(pairedPlacementKey(workflow(), missing(), more) != base);                           // or switched off
        ReadinessPlan withProblem = missing();
        withProblem.problems << makeError(QStringLiteral("model_missing"), QStringLiteral("no source"));
        QVERIFY(pairedPlacementKey(workflow(), withProblem, asked.instances) != base);              // the gate's input
    }

    // The scheduler fails a pin to a name that is no provider (`unknown_provider`): the verdict does not call that node placed.
    void aPinToAnUnknownProviderBlocksTheNode() {
        FakeHosts hosts({{"gx10", {"text.encode"}}});
        const PairedVerdict made = verdict(workflow("nowhere"), missing(), request(), hosts);
        QCOMPARE(hosts.asked, 0);
        QCOMPARE(made.blocked.size(), 1);
        QVERIFY(made.blocked.first().message.contains("no provider is named nowhere"));
        QVERIFY(!made.runsOn());
    }

    void aKeptProbeAsksOncePerQuestionAndForgetsASilentHost() {
        FakeHosts hosts({{"gx10", {"text.encode"}}});
        PairedProbe kept = keptPairedProbe(hosts.probe(), std::chrono::minutes(1));
        PairedPlacementRequest asked = request();
        for (int i = 0; i < 3; ++i) {
            std::optional<PairedVerdict> got;
            decidePairedPlacement(workflow(), missing(), asked, kept, [&got](const PairedVerdict &made) { got = made; });
            QVERIFY(got && got->runsOn());
        }
        QCOMPARE(hosts.asked, 1);

        FakeHosts silent;
        PairedProbe forgetful = keptPairedProbe(silent.probe(), std::chrono::minutes(1));
        for (int i = 0; i < 2; ++i)
            decidePairedPlacement(workflow(), missing(), asked, forgetful, [](const PairedVerdict &) {});
        QCOMPARE(silent.asked, 2); // a host that did not answer is asked again

        FakeHosts expired({{"gx10", {"text.encode"}}});
        PairedProbe none = keptPairedProbe(expired.probe(), std::chrono::milliseconds(0));
        for (int i = 0; i < 2; ++i)
            decidePairedPlacement(workflow(), missing(), asked, none, [](const PairedVerdict &) {});
        QCOMPARE(expired.asked, 2);
    }

    // A question asked while the same one is out waits for it: one probe, both answered.
    void aQuestionAskedWhileTheSameIsOutSharesItsProbe() {
        int asked = 0;
        std::function<void(const QMap<QString, Description> &)> reply;
        PairedProbe held = [&](const QList<ProviderInstance> &, std::function<void(const QMap<QString, Description> &)> done) {
            ++asked;
            reply = std::move(done);
        };
        PairedProbe kept = keptPairedProbe(held, std::chrono::minutes(1));
        int answers = 0;
        for (int i = 0; i < 2; ++i)
            decidePairedPlacement(workflow(), missing(), request(), kept, [&answers](const PairedVerdict &made) { answers += made.runsOn(); });
        QCOMPARE(asked, 1);
        reply({{"gx10", offering({"text.encode"})}});
        QCOMPARE(answers, 2);
    }
};

QTEST_GUILESS_MAIN(TestPairedPlacement)
#include "test_paired_placement.moc"
