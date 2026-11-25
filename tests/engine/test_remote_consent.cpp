// Where an instance runs, what a node sends there, and the answers kept for off-machine services (spec 8.2, #404).

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/remote_consent.h"
#include "scheduler_support.h"

using namespace fugue;
using namespace fugue::test;

namespace {

ProviderInstance instance(const QString &name, const QString &type, const QString &address = {}) {
    ProviderInstance made;
    made.name = name;
    made.type = type;
    made.address = address;
    return made;
}

QJsonObject node(const QString &type, const QJsonObject &inputs) { return QJsonObject{{"type", type}, {"inputs", inputs}}; }

QJsonObject generateGraph() {
    return QJsonObject{{"g", node("fugue/generate", {{"prompt", "a cat"}})}, {"s", node("fugue/save", {{"value", QJsonObject{{"from", "g.text"}}}})}};
}

} // namespace

class TestRemoteConsent : public QObject {
    Q_OBJECT

    QTemporaryDir m_data; // XDG_DATA_HOME of the test: the provider modules are looked for in <it>/fugue/providers

    void installModule(const QString &id, const QString &transport, const QString &address) {
        const QString folder = m_data.filePath("fugue/providers/" + id);
        QDir().mkpath(folder);
        QJsonObject manifest{{"manifest", 1}, {"id", id}, {"version", "0.1.0"}, {"name", id}, {"transport", transport},
                             {"capabilities", QJsonArray{"image.generate"}}};
        if (transport == "stdio")
            manifest.insert("start", QJsonObject{{"command", QJsonArray{"./toy"}}});
        else
            manifest.insert("address", address);
        QFile file(folder + "/provider.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(manifest).toJson());
    }

private slots:
    void initTestCase() { qputenv("XDG_DATA_HOME", m_data.path().toUtf8()); }

    // An http module's own default address is used when the instance has none (providers.cpp): that is where the data goes.
    void aModulesDefaultAddressIsTheOneJudgedAndFingerprinted() {
        installModule("io.example.cloud", "http", "https://api.example.com/v1");
        installModule("io.example.lan", "http", "http://192.168.1.9:7860");
        installModule("io.example.mine", "http", "http://127.0.0.1:{port}");
        installModule("io.example.prog", "stdio", {});
        const ProviderInstance cloud = instance("Cloud", "io.example.cloud");
        QCOMPARE(placementOf(cloud), Placement::OffMachine);
        QCOMPARE(hostOf(cloud), QStringLiteral("api.example.com"));
        QCOMPARE(consentFingerprint(cloud), QStringLiteral("io.example.cloud|https://api.example.com/v1"));
        QCOMPARE(placementOf(instance("Cloud", "io.example.cloud", "http://127.0.0.1:9")), Placement::ThisMachine); // the instance's own wins
        QCOMPARE(placementOf(instance("Lan", "io.example.lan")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("Mine", "io.example.mine")), Placement::ThisMachine);
        // A stdio module is a program run here: this machine, even if the program itself calls a cloud API (spec 8.2).
        QCOMPARE(placementOf(instance("Prog", "io.example.prog")), Placement::ThisMachine);
        QVERIFY(anyOffMachine({cloud}));
    }

    void placementFollowsTheTypeAndTheAddress() {
        QCOMPARE(placementOf(instance("This machine", "omarchy")), Placement::ThisMachine);
        QCOMPARE(placementOf(instance("Local", "http", "http://127.0.0.1:8080/v1")), Placement::ThisMachine);
        QCOMPARE(placementOf(instance("Local", "http", "localhost:1234")), Placement::ThisMachine);
        // The person's own network: paired hosts, private, link-local and CGNAT addresses, .local names.
        QCOMPARE(placementOf(instance("gx10", "omarchy", "https://gx10.example.ts.net")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("helper", "fugue-helper", "http://gx10.example.ts.net:8787")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("helper", "fugue-helper", "http://10.0.0.7:8787")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("Box", "http", "https://box.tail1234.ts.net")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("helper", "fugue-helper")), Placement::ThisMachine);
        // A fugue-helper has no pairing step: a public address is a service off this machine like any other.
        QCOMPARE(placementOf(instance("helper", "fugue-helper", "https://helper.example.com:8787")), Placement::OffMachine);
        QCOMPARE(placementOf(instance("Box", "http", "http://192.168.1.20:7860")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("Tail", "http", "http://100.101.102.103:7860")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("Mac", "http", "http://studio.local:7860")), Placement::OwnHost);
        // A fal instance with only a key goes to fal's queue by default: off-machine, so the question is asked (#407).
        const ProviderInstance fal = instance("fal.ai", "fal");
        QCOMPARE(placementOf(fal), Placement::OffMachine);
        QCOMPARE(effectiveAddress(fal), QStringLiteral("https://queue.fal.run"));
        QCOMPARE(hostOf(fal), QStringLiteral("queue.fal.run"));
        QCOMPARE(consentFingerprint(fal), QStringLiteral("fal|https://queue.fal.run|https://rest.fal.ai"));
        QVERIFY(anyOffMachine({fal}));
        QCOMPARE(placementOf(instance("fal.ai", "fal", "http://127.0.0.1:9")), Placement::ThisMachine); // a test's fake serves both
        // The key and the pictures also go to the storage host: a loopback queue with fal's own storage is off-machine, and
        // changing the storage host is another thing to agree to.
        ProviderInstance split = instance("fal.ai", "fal", "http://127.0.0.1:9");
        split.settings.insert("rest_address", "https://rest.fal.ai");
        QCOMPARE(placementOf(split), Placement::OffMachine);
        ProviderInstance typed = instance("fal.ai", "fal", "https://queue.fal.run"); // the typed default still has fal's storage
        QCOMPARE(consentFingerprint(typed), consentFingerprint(fal));
        ProviderInstance moved = fal;
        moved.settings.insert("rest_address", "https://storage.example.com");
        QVERIFY(consentFingerprint(moved) != consentFingerprint(fal));
        ProviderInstance ownStorage = instance("fal.ai", "fal", "http://127.0.0.1:9");
        ownStorage.settings.insert("rest_address", "http://127.0.0.1:10");
        QCOMPARE(placementOf(ownStorage), Placement::ThisMachine);
        // A public name or address is off this machine, whatever the scheme.
        QCOMPARE(placementOf(instance("Cloud", "http", "https://api.example.com/v1")), Placement::OffMachine);
        QCOMPARE(placementOf(instance("Cloud", "http", "8.8.8.8:80")), Placement::OffMachine);
        QCOMPARE(placementOf(instance("Module", "acme-cloud", "https://acme.example.com")), Placement::OffMachine);
        // A paired-host type is never off-machine, and a module with no address runs as a program here.
        QCOMPARE(placementOf(instance("Odd", "omarchy", "https://anything.example.com")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("Module", "acme-local")), Placement::ThisMachine);
    }

    void aRunSaysWhatItSendsToAnOffMachineService() {
        World world;
        world.type("fugue/pictures",
                   "'inputs': [{'name': 'image', 'type': 'image'}, {'name': 'mask', 'type': 'image', 'role': 'mask', 'optional': true},"
                   "{'name': 'prompt', 'type': 'text', 'optional': true}, {'name': 'size', 'type': 'text', 'choices': ['a', 'b'], 'default': 'a'}],"
                   "'outputs': []",
                   nullptr);
        const NodeType *type = world.registry.nodeTypeFor("fugue/pictures", 1);
        QVERIFY(type);
        CompiledNode written;
        written.inputs.insert("image", InputValue::link({"a", "image"}));
        written.inputs.insert("prompt", InputValue::literal("a cat"));
        QCOMPARE(sendsOf(*type, written), (QStringList{"pictures", "prompts"}));
        written.inputs.insert("mask", InputValue::link({"b", "image"}));
        written.inputs.insert("prompt", InputValue::literal("  "));
        QCOMPARE(sendsOf(*type, written), (QStringList{"pictures", "masks"})); // blank text is nothing, a choice is a setting
        QCOMPARE(sendsText({"pictures", "masks", "prompts"}), QStringLiteral("pictures, masks and prompts"));
    }

    void anAnswerIsKeptPerInstanceAndForgottenWithTheInstanceOrItsAddress() {
        QTemporaryDir dir;
        RemoteConsent consent(dir.filePath("state/remote-consent.json"));
        const ProviderInstance cloud = instance("Cloud", "http", "https://api.example.com/v1/");
        const ProviderInstance other = instance("Other", "http", "https://other.example.com");
        QVERIFY(!consent.isAllowed(cloud));
        QVERIFY(consent.allow(cloud).isEmpty());
        QVERIFY(RemoteConsent(dir.filePath("state/remote-consent.json")).isAllowed(cloud)); // kept on disk
        QCOMPARE(QFile::permissions(dir.filePath("state/remote-consent.json")) & (QFile::ReadGroup | QFile::ReadOther), QFileDevice::Permissions());
        QVERIFY(!consent.isAllowed(other));
        // A trailing slash and the case of the address do not make another service; another address does.
        QVERIFY(consent.isAllowed(instance("Cloud", "http", "HTTPS://api.example.com/v1")));
        QVERIFY(!consent.isAllowed(instance("Cloud", "http", "https://api.example.net/v1")));
        QCOMPARE(consent.allowedNames({cloud, other}), QSet<QString>{"Cloud"});
        // Removing the instance (or changing its address) forgets the answer for good: putting it back asks again.
        QVERIFY(consent.prune({instance("Cloud", "http", "https://api.example.net/v1"), other}).isEmpty());
        QVERIFY(!consent.isAllowed(cloud));
        QVERIFY(consent.allow(cloud).isEmpty());
        QVERIFY(consent.prune({other}).isEmpty());
        QVERIFY(!consent.isAllowed(cloud));
        // A yes is for what was shown: its fingerprint, not whatever the instance is at answer time.
        QVERIFY(consent.allow("Cloud", consentFingerprint(instance("Cloud", "http", "https://shown.example.com"))).isEmpty());
        QVERIFY(!consent.isAllowed(cloud));
        QCOMPARE(RemoteConsent::matching(QJsonObject{{"Cloud", consentFingerprint(cloud)}, {"Other", "x"}}, {cloud, other}), QSet<QString>{"Cloud"});
        // A write that cannot happen is reported.
        QVERIFY(!RemoteConsent("/proc/nope/remote-consent.json").allow(cloud).isEmpty());
    }

    void aCheckSaysWhereEachNodeGoesAndARunRefusesWithoutTheYes() {
        World world;
        const auto provider = world.provider("Cloud", {"flux"}, "https://api.example.com/v1");
        world.providers.last().instance.type = QStringLiteral("http");
        const CompiledGraph graph = world.graph(generateGraph());

        const GraphCheck check = checkGraph(graph, world.environment());
        QCOMPARE(check.routes.value("g").instance, QStringLiteral("Cloud"));
        QCOMPARE(check.routes.value("g").sends, QStringList{"prompts"});
        QCOMPARE(check.states.value("g"), NodeState::Stale);
        const QList<ProviderInstance> instances{world.providers.first().instance};
        const QList<RemoteUse> uses = remoteUses(check.routes, {"g", "s"}, instances);
        QCOMPARE(uses.size(), 1);
        QCOMPARE(uses.first().host, QStringLiteral("api.example.com"));
        QCOMPARE(uses.first().sends, QStringList{"prompts"});
        QVERIFY(notAllowed(uses, {"Cloud"}).isEmpty());
        QCOMPARE(consentNeededError(uses).code, QStringLiteral("consent_needed"));

        // Not allowed: the node fails before anything is submitted.
        const Ran refused = world.run(graph);
        QCOMPARE(refused.outcome.status, RunStatus::Failed);
        QCOMPARE(refused.outcome.errors.first().code, QStringLiteral("consent_needed"));
        QCOMPARE(refused.outcome.errors.first().node, QStringLiteral("g"));
        QVERIFY(provider->jobs().isEmpty());

        // Allowed: the job goes to the service.
        RunOptions options;
        options.remoteAllowed = {"Cloud"};
        Run run(graph, world.folder(), world.environment(), options);
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->finish(provider->jobs().first(), {{"result.txt", "a picture"}});
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);
    }
};

QTEST_GUILESS_MAIN(TestRemoteConsent)
#include "test_remote_consent.moc"
