// Where an instance runs, what a node sends there, and the answers kept for off-machine services (spec 8.2, #404).

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

private slots:
    void placementFollowsTheTypeAndTheAddress() {
        QCOMPARE(placementOf(instance("This machine", "omarchy")), Placement::ThisMachine);
        QCOMPARE(placementOf(instance("Local", "http", "http://127.0.0.1:8080/v1")), Placement::ThisMachine);
        QCOMPARE(placementOf(instance("Local", "http", "localhost:1234")), Placement::ThisMachine);
        // The person's own network: paired hosts, private, link-local and CGNAT addresses, .local names.
        QCOMPARE(placementOf(instance("gx10", "omarchy", "https://gx10.example.ts.net")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("helper", "fugue-helper", "http://gx10.example:8787")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("Box", "http", "http://192.168.1.20:7860")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("Tail", "http", "http://100.101.102.103:7860")), Placement::OwnHost);
        QCOMPARE(placementOf(instance("Mac", "http", "http://studio.local:7860")), Placement::OwnHost);
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
