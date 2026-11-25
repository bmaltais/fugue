// Provider instances (spec 8.2) and choosing one for a capability (8.3 to 8.5).

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/helper_provider.h"
#include "engine/omarchy_provider.h"
#include "engine/providers.h"
#include "omarchy_fixture.h"

using namespace fugue;

namespace {

QJsonObject handle(const QString &name) {
    return QJsonObject{{"resource", "fugue/image-model"}, {"name", name}};
}

OfferedModel model(const QString &name, ModelStatus status = ModelStatus::Ready,
                   const QJsonObject &properties = {}) {
    OfferedModel offered;
    offered.handle = handle(name);
    offered.status = status;
    offered.properties = properties;
    if (status != ModelStatus::Ready) {
        offered.reason = QStringLiteral("%1 is stopped.").arg(name);
        offered.hint = QStringLiteral("Start it: omarchy-agent-local start %1").arg(name);
    }
    return offered;
}

Candidate candidate(const QString &name, QList<CapabilityOffer> offers, bool enabled = true) {
    Candidate result;
    result.instance.name = name;
    result.instance.type = QStringLiteral("fake");
    result.instance.enabled = enabled;
    result.description.value().engine = QStringLiteral("sdcpp");
    result.description.value().capabilities = std::move(offers);
    return result;
}

CapabilityOffer generate(QList<OfferedModel> models) {
    return CapabilityOffer{QStringLiteral("image.generate"), std::move(models)};
}

Requirement wants(const QString &capability) {
    Requirement requirement;
    requirement.capability = capability;
    return requirement;
}

// A local instance first, then the paired helper, as the default order has them.
QList<Candidate> localAndHost(bool hostAnswers) {
    Candidate local = candidate("This machine", {generate({model("any")})});
    Candidate host = candidate("gx10", {generate({model("any")})});
    host.instance.type = QStringLiteral("fugue-helper");
    host.instance.address = QStringLiteral("http://gx10.example:8787");
    host.description.value().sharesFilesystem = false;
    if (!hostAnswers)
        host.description = Result<Description>(makeError("provider_unreachable", "cannot reach it: timed out."));
    return {local, host};
}

QStringList strings(const QJsonArray &array) {
    QStringList list;
    for (const QJsonValue &value : array)
        list.append(value.toString());
    return list;
}

} // namespace

class TestProviderSelection : public QObject {
    Q_OBJECT

private slots:
    void instancesKeepTheUsersOrderAndTheirSettings() {
        // The example of spec 8.2, plus a module setting and a disabled instance.
        const QJsonArray providers{
            QJsonObject{{"name", "This machine"}, {"type", "omarchy"}},
            QJsonObject{{"name", "Studio box"}, {"type", "omarchy"},
                        {"address", "https://studio.example.ts.net"}, {"enabled", false}, {"poll_ms", 250}},
            QJsonObject{{"name", "My endpoint"}, {"type", "http"},
                        {"address", "https://api.example.com/v1"}, {"key", "k"}},
        };
        const auto parsed = parseProviderInstances(QJsonObject{{"providers", providers}});
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.value().size(), 3);
        QCOMPARE(parsed.value().at(0).name, QStringLiteral("This machine"));
        QVERIFY(parsed.value().at(0).enabled);
        QVERIFY(parsed.value().at(0).address.isEmpty());
        QCOMPARE(parsed.value().at(1).address, QStringLiteral("https://studio.example.ts.net"));
        QVERIFY(!parsed.value().at(1).enabled);
        QCOMPARE(parsed.value().at(1).settings.value("poll_ms").toInt(), 250);
        QCOMPARE(parsed.value().at(2).key, QStringLiteral("k"));
        QVERIFY(!parsed.value().at(2).settings.contains("key"));
    }

    void aRemoteHelperEntryHasAnAddressAndAKey() {
        const Result<QList<ProviderInstance>> parsed = parseProviderInstances(QJsonObject{
            {"providers", QJsonArray{QJsonObject{{"name", "gx10"}, {"type", "fugue-helper"},
                                                 {"address", "http://100.84.169.13:8787"}, {"key", "secret"}}}}});
        QVERIFY(parsed);
        QCOMPARE(parsed->first().address, QStringLiteral("http://100.84.169.13:8787"));
        QCOMPARE(parsed->first().key, QStringLiteral("secret"));
        QVERIFY(parsed->first().settings.isEmpty());
        QVERIFY(createProvider(parsed->first()));
    }

    void theSelectionSaysWhetherTheProviderSharesTheFilesystem() {
        Candidate here = candidate("Fugue helper", {CapabilityOffer{"latent.sample", {model("")}}});
        Candidate there = candidate("gx10", {CapabilityOffer{"latent.sample", {model("")}}});
        there.description.value().sharesFilesystem = false;
        there.description.value().freeMemoryBytes = 96000000000;
        Requirement requirement = wants("latent.sample");
        const Result<Selection> first = selectProvider({here, there}, requirement);
        QVERIFY(first);
        QVERIFY(first->sharesFilesystem);
        requirement.nodePin = QStringLiteral("gx10");
        const Result<Selection> pinned = selectProvider({here, there}, requirement);
        QVERIFY(pinned);
        QCOMPARE(pinned->instance, QStringLiteral("gx10"));
        QVERIFY(!pinned->sharesFilesystem);
        // With the local one disabled, the host serves "any ready model".
        here.instance.enabled = false;
        requirement.nodePin.clear();
        const Result<Selection> only = selectProvider({here, there}, requirement);
        QVERIFY(only);
        QVERIFY(!only->sharesFilesystem);
    }

    // A node's preference (a mask, 14.4): any ready model that has it wins over an earlier one that
    // has not, nothing having it fails nothing, and a named model is still the named model; the
    // models that have it are in the selection for the node's message.
    void aPreferredPropertyChoosesAmongReadyModelsAndNeverFails() {
        const QJsonObject masks{{"inputs", QJsonArray{"mask"}}};
        const QList<Candidate> candidates{
            candidate("This machine", {generate({model("plain")})}),
            candidate("Studio", {generate({model("stopped", ModelStatus::Unavailable, masks), model("fills", ModelStatus::Ready, masks)})}),
        };
        Requirement requirement = wants("image.generate");
        QCOMPARE(selectProvider(candidates, requirement)->model.handle, handle("plain"));
        QVERIFY(selectProvider(candidates, requirement)->preferredOffers.isEmpty());

        requirement.preferred = masks;
        const auto chosen = selectProvider(candidates, requirement);
        QVERIFY(chosen.ok());
        QCOMPARE(chosen->instance, QStringLiteral("Studio"));
        QCOMPARE(chosen->model.handle, handle("fills"));
        QCOMPARE(chosen->preferredOffers, (QStringList{"Studio: fills"}));

        // A model asked for by name is that one, with the others that could take the mask named.
        requirement.model = handle("plain");
        const auto named = selectProvider(candidates, requirement);
        QVERIFY(named.ok());
        QCOMPARE(named->model.handle, handle("plain"));
        QCOMPARE(named->preferredOffers, (QStringList{"Studio: fills"}));

        // Nothing has it: the first ready model, as without the preference.
        requirement.model = {};
        const auto none = selectProvider({candidates.first()}, requirement);
        QVERIFY(none.ok());
        QCOMPARE(none->model.handle, handle("plain"));
        QVERIFY(none->preferredOffers.isEmpty());
    }

    void instancesNeedAUniqueNameAndAType() {
        const QJsonArray providers{
            QJsonObject{{"name", "A"}, {"type", "omarchy"}},
            QJsonObject{{"name", "A"}, {"type", "omarchy"}},
            QJsonObject{{"name", "B"}},
        };
        const auto parsed = parseProviderInstances(QJsonObject{{"providers", providers}});
        QVERIFY(!parsed.ok());
        QCOMPARE(parsed.errors().size(), 2);
        for (const Error &error : parsed.errors())
            QCOMPARE(error.code, QStringLiteral("invalid_document"));
    }

    void withoutAProvidersFileOrPairedHostsThisMachineIsTheOnlyInstance() {
        QTemporaryDir dir;
        const OmarchyPaths nothing{dir.filePath("state"), dir.filePath("config")};
        const auto loaded = loadProviderInstances(dir.filePath("providers.json"), nothing);
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.value().size(), 1);
        QCOMPARE(loaded.value().first().name, QStringLiteral("This machine"));
        QCOMPARE(loaded.value().first().type, QStringLiteral("omarchy"));
        QVERIFY(loaded.value().first().enabled);
        QVERIFY(loaded.value().first().address.isEmpty());
    }

    void withoutAProvidersFileEachPairedHostIsAnInstanceToo() {
        // The spec 8.2 default: this machine, then every host network.json says this machine
        // is paired with, by name, at the address its models are reached through. A host
        // that is offline stays, so a pin on it says it is offline rather than unknown; a
        // host that is not paired is not used.
        QTemporaryDir home;
        const OmarchyPaths paths = test::writeOmarchyState(home.path());
        const auto loaded = loadProviderInstances(home.filePath("config/fugue/providers.json"), paths);
        QVERIFY(loaded.ok());
        QStringList names;
        for (const ProviderInstance &instance : loaded.value()) {
            names << instance.name;
            QCOMPARE(instance.type, QStringLiteral("omarchy"));
            QVERIFY(instance.enabled);
            QVERIFY(instance.key.isEmpty()); // the host's key stays in Omarchy's own file
        }
        QCOMPARE(names, (QStringList{"This machine", "lab", "studio"}));
        QCOMPARE(loaded.value().at(1).address, QStringLiteral("https://lab.tail1234.ts.net"));
        QCOMPARE(loaded.value().at(2).address, QStringLiteral("https://studio.tail1234.ts.net"));
        // Nothing is written: the file is the user's, made once they change something (#51).
        QVERIFY(!QFile::exists(home.filePath("config/fugue/providers.json")));
    }

    void anExistingProvidersFileWinsEntirely() {
        QTemporaryDir home;
        const OmarchyPaths paths = test::writeOmarchyState(home.path());
        QFile file(home.filePath("providers.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"providers": [{"name": "Only this", "type": "omarchy"}]})");
        file.close();
        const auto loaded = loadProviderInstances(file.fileName(), paths);
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.value().size(), 1);
        QCOMPARE(loaded.value().first().name, QStringLiteral("Only this"));
    }

    void aNetworkFileThatCannotBeReadLeavesThisMachine() {
        QTemporaryDir home;
        const OmarchyPaths paths = test::writeOmarchyState(home.path());
        QVERIFY(test::writeFixtureFile(paths.stateDir + "/network.json", "{half"));
        const auto loaded = loadProviderInstances(home.filePath("providers.json"), paths);
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.value().size(), 1);
        QCOMPARE(loaded.value().first().name, QStringLiteral("This machine"));
    }

    void aProvidersFileThatIsNotJsonIsAnError() {
        QTemporaryDir dir;
        QFile file(dir.filePath("providers.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{nope");
        file.close();
        const auto loaded = loadProviderInstances(file.fileName());
        QVERIFY(!loaded.ok());
        QCOMPARE(loaded.errors().first().code, QStringLiteral("invalid_json"));
    }

    void anOmarchyInstanceMakesTheOmarchyProvider() {
        ProviderInstance instance;
        instance.name = QStringLiteral("This machine");
        instance.type = QStringLiteral("omarchy");
        const auto made = createProvider(instance);
        QVERIFY(made.ok());
        QVERIFY(made.value());

        instance.type = QStringLiteral("io.example.engine");
        const auto unknown = createProvider(instance);
        QVERIFY(!unknown.ok());
        QCOMPARE(unknown.errors().first().code, QStringLiteral("unknown_provider"));
        QCOMPARE(strings(unknown.errors().first().valid), (QStringList{"fugue-helper", "omarchy"}));

        instance.type = QStringLiteral("fugue-helper");
        const auto helper = createProvider(instance);
        QVERIFY(helper.ok());
        QVERIFY(qobject_cast<HelperProvider *>(helper.value().get()));
    }

    void theDefaultsEndWithTheHelperWhenOneIsInstalled() {
        // Last, so "any ready model" stays omarchy's; models on disk only the helper serves.
        QTemporaryDir state;
        const OmarchyPaths paths{state.path(), state.path()};
        qputenv("FUGUE_HELPER", FUGUE_FAKE_HELPER);
        const QList<ProviderInstance> withHelper = defaultProviderInstances(paths);
        qputenv("FUGUE_HELPER", "");
        const QList<ProviderInstance> without = defaultProviderInstances(paths);
        QCOMPARE(without.size(), 1);
        QCOMPARE(withHelper.size(), 2);
        QCOMPARE(withHelper.first().type, QStringLiteral("omarchy"));
        QCOMPARE(withHelper.last().name, QStringLiteral("Fugue helper"));
        QCOMPARE(withHelper.last().type, QStringLiteral("fugue-helper"));
    }

    void autoTakesTheFirstEnabledReadyInstanceInTheUsersOrder() {
        Candidate unreachable = candidate("Down", {generate({model("a")})});
        unreachable.description = Result<Description>(Errors{makeError("provider_unreachable", "Down does not answer.")});
        const QList<Candidate> candidates{
            candidate("Off", {generate({model("a")})}, false),
            unreachable,
            candidate("Loading", {generate({model("b", ModelStatus::Loading)})}),
            candidate("Ready", {generate({model("c")})}),
            candidate("Later", {generate({model("d")})}),
        };
        const auto chosen = selectProvider(candidates, wants("image.generate"));
        QVERIFY(chosen.ok());
        QCOMPARE(chosen.value().instance, QStringLiteral("Ready"));
        QCOMPARE(chosen.value().engine, QStringLiteral("sdcpp"));
        QCOMPARE(chosen.value().model.handle.value("name").toString(), QStringLiteral("c"));
    }

    void autoNeedsTheRequiredProperties() {
        const QList<Candidate> candidates{
            candidate("A", {generate({model("plain", ModelStatus::Ready, {{"input", QJsonArray{"text"}}})})}),
            candidate("B", {generate({model("vision", ModelStatus::Ready,
                                            {{"input", QJsonArray{"text", "image"}}, {"edit", true}})})}),
        };
        Requirement requirement = wants("image.generate");
        requirement.properties = QJsonObject{{"input", QJsonArray{"image"}}, {"edit", true}};
        const auto chosen = selectProvider(candidates, requirement);
        QVERIFY(chosen.ok());
        QCOMPARE(chosen.value().instance, QStringLiteral("B"));
    }

    void aRequestedModelIsFoundByNameOrByHash() {
        OfferedModel hashed = model("other-name");
        hashed.handle.insert("hash", "sha256:" + QString(64, 'a'));
        const QList<Candidate> candidates{
            candidate("A", {generate({model("klein")})}),
            candidate("B", {generate({model("z-image"), hashed})}),
        };
        Requirement byName = wants("image.generate");
        byName.model = handle("z-image");
        QCOMPARE(selectProvider(candidates, byName).value().instance, QStringLiteral("B"));

        Requirement byHash = wants("image.generate");
        byHash.model = QJsonObject{{"resource", "fugue/image-model"}, {"hash", "sha256:" + QString(64, 'a')},
                                   {"name", "renamed.gguf"}};
        const auto chosen = selectProvider(candidates, byHash);
        QVERIFY(chosen.ok());
        QCOMPARE(chosen.value().model.handle.value("name").toString(), QStringLiteral("other-name"));
    }

    void aNodePinBeatsTheWorkflowPinWhichBeatsAuto() {
        const QList<Candidate> candidates{
            candidate("First", {generate({model("a")})}),
            candidate("Second", {generate({model("b")})}),
            candidate("Third", {generate({model("c")})}),
        };
        Requirement requirement = wants("image.generate");
        requirement.workflowPin = QStringLiteral("Second");
        QCOMPARE(selectProvider(candidates, requirement).value().instance, QStringLiteral("Second"));
        requirement.nodePin = QStringLiteral("Third");
        QCOMPARE(selectProvider(candidates, requirement).value().instance, QStringLiteral("Third"));
    }

    void aPinThatIsNotConfiguredIsUnknownProvider() {
        const QList<Candidate> candidates{candidate("This machine", {generate({model("a")})}),
                                          candidate("Studio box", {generate({model("b")})})};
        Requirement requirement = wants("image.generate");
        requirement.nodePin = QStringLiteral("Studio");
        const auto chosen = selectProvider(candidates, requirement);
        QVERIFY(!chosen.ok());
        const Error error = chosen.errors().first();
        QCOMPARE(error.code, QStringLiteral("unknown_provider"));
        QCOMPARE(strings(error.valid), (QStringList{"This machine", "Studio box"}));
        QVERIFY(!error.hint.isEmpty());
    }

    void aPinnedInstanceThatCannotServeNeverFallsBackToAuto() {
        const QList<Candidate> candidates{
            candidate("Pinned", {CapabilityOffer{"image.edit", {model("klein")}}}),
            candidate("Other", {generate({model("a")})}),
        };
        Requirement requirement = wants("image.generate");
        requirement.workflowPin = QStringLiteral("Pinned");
        const auto chosen = selectProvider(candidates, requirement);
        QVERIFY(!chosen.ok());
        QCOMPARE(chosen.errors().first().code, QStringLiteral("capability_unavailable"));
        QVERIFY(chosen.errors().first().message.contains("Pinned"));
    }

    void aPinnedDisabledInstanceSaysToEnableIt() {
        const QList<Candidate> candidates{candidate("Studio box", {generate({model("a")})}, false)};
        Requirement requirement = wants("image.generate");
        requirement.nodePin = QStringLiteral("Studio box");
        const auto chosen = selectProvider(candidates, requirement);
        QCOMPARE(chosen.errors().first().code, QStringLiteral("capability_unavailable"));
        QVERIFY(chosen.errors().first().hint.contains("Enable"));
        QVERIFY(chosen.errors().first().hint.contains("Studio box"));
    }

    void aPinnedUnreachableInstanceReportsWhy() {
        Candidate down = candidate("Studio box", {});
        down.description = Result<Description>(Errors{
            makeError("provider_unreachable", "Studio box does not answer.", "Turn it on.")});
        Requirement requirement = wants("image.generate");
        requirement.nodePin = QStringLiteral("Studio box");
        const auto chosen = selectProvider({down}, requirement);
        QCOMPARE(chosen.errors().first().code, QStringLiteral("provider_unreachable"));
        QCOMPARE(chosen.errors().first().hint, QStringLiteral("Turn it on."));
    }

    void nothingOfferingTheCapabilitySaysHowToFixIt() {
        const QList<Candidate> candidates{
            candidate("This machine", {CapabilityOffer{"image.edit", {model("klein")}}}),
            candidate("Studio box", {generate({model("a")})}, false),
        };
        const auto chosen = selectProvider(candidates, wants("image.generate"));
        QVERIFY(!chosen.ok());
        const Error error = chosen.errors().first();
        QCOMPARE(error.code, QStringLiteral("capability_unavailable"));
        QVERIFY(error.message.contains("image.generate"));
        QVERIFY(error.hint.contains("Start a model"));
        QVERIFY(error.hint.contains("providers.json"));
        QVERIFY(error.hint.contains("Enable Studio box"));
    }

    void onlyStoppedModelsSayWhichToStart() {
        const QList<Candidate> candidates{
            candidate("This machine", {generate({model("klein", ModelStatus::Unavailable)})}),
        };
        const auto chosen = selectProvider(candidates, wants("image.generate"));
        const Error error = chosen.errors().first();
        QCOMPARE(error.code, QStringLiteral("capability_unavailable"));
        QVERIFY(error.hint.contains("omarchy-agent-local start klein"));
    }

    // 8.5: the error always says how to fix it, also when the model itself gives no hint.
    void aPinnedInstanceWithOnlyAStoppedModelThatHasNoHintStillSaysWhatToDo() {
        OfferedModel stopped = model("flux", ModelStatus::Unavailable);
        stopped.hint.clear();
        Requirement requirement = wants("image.generate");
        requirement.nodePin = QStringLiteral("Studio");
        const auto selection = selectProvider({candidate("Studio", {generate({stopped})})}, requirement);
        QVERIFY(!selection.ok());
        QCOMPARE(selection.error().code, QStringLiteral("capability_unavailable"));
        QVERIFY2(selection.error().hint.contains("flux") && selection.error().hint.contains("Studio"),
                 qPrintable(selection.error().hint));
        QVERIFY(selection.error().hint.contains("pin"));
    }

    void aRequestedModelThatIsNotRunningIsModelNotRunning() {
        const QList<Candidate> candidates{
            candidate("This machine", {generate({model("klein", ModelStatus::Unavailable), model("z-image")})}),
        };
        Requirement requirement = wants("image.generate");
        requirement.model = handle("klein");
        const auto chosen = selectProvider(candidates, requirement);
        QVERIFY(!chosen.ok());
        const Error error = chosen.errors().first();
        QCOMPARE(error.code, QStringLiteral("model_not_running"));
        QVERIFY(error.message.contains("klein"));
        QVERIFY(error.hint.contains("omarchy-agent-local start klein"));
        QCOMPARE(strings(error.valid), QStringList{"z-image"});
    }

    void aRequestedModelNobodyHasIsModelMissing() {
        const QList<Candidate> candidates{candidate("This machine", {generate({model("z-image")})})};
        Requirement requirement = wants("image.generate");
        requirement.model = handle("klein");
        const auto chosen = selectProvider(candidates, requirement);
        QCOMPARE(chosen.errors().first().code, QStringLiteral("model_missing"));
        QCOMPARE(chosen.errors().first().severity, Error::Severity::Error);
        QCOMPARE(strings(chosen.errors().first().valid), QStringList{"z-image"});
    }
    // --- model files that are not on this machine (#395) ---

    void modelFilesElsewhereGoToThePairedHelper() {
        Requirement requirement = wants("image.generate");
        requirement.modelsElsewhere = true;
        const auto chosen = selectProvider(localAndHost(true), requirement);
        QVERIFY(chosen);
        QCOMPARE(chosen->instance, QStringLiteral("gx10"));
        QVERIFY(!chosen->sharesFilesystem);
    }

    void modelFilesHereKeepTheOrderEvenWithAPairedHelper() {
        const auto chosen = selectProvider(localAndHost(true), wants("image.generate"));
        QVERIFY(chosen);
        QCOMPARE(chosen->instance, QStringLiteral("This machine"));
    }

    void modelFilesElsewhereAndTheHelperGoneIsAClearError() {
        Requirement requirement = wants("image.generate");
        requirement.modelsElsewhere = true;
        const auto chosen = selectProvider(localAndHost(false), requirement);
        QVERIFY(!chosen);
        QCOMPARE(chosen.errors().first().code, QStringLiteral("provider_unreachable"));
        QVERIFY(chosen.errors().first().message.contains("gx10"));
        QVERIFY(chosen.errors().first().message.contains("timed out"));
    }

    void modelFilesElsewhereWithNoPairedHelperKeepTheOldAnswer() {
        const QList<Candidate> candidates{candidate("This machine", {generate({model("any")})})};
        Requirement requirement = wants("image.generate");
        requirement.modelsElsewhere = true;
        const auto chosen = selectProvider(candidates, requirement);
        QVERIFY(chosen);
        QCOMPARE(chosen->instance, QStringLiteral("This machine"));
    }
};

QTEST_GUILESS_MAIN(TestProviderSelection)
#include "test_provider_selection.moc"
