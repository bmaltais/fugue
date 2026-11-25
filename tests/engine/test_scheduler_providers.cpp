// The scheduler's provider side (spec 8.3 to 8.5, 9.2, 9.3, 9.6) and
// checkGraph (9.1, 10.3).
//
// Seams: Run, runGraph() and checkGraph() again, with FakeProvider instances
// whose jobs move only when the test moves them, and the ResidentModels of
// the environment.

#include <QSemaphore>
#include <QtTest>

#include <atomic>

#include "engine/hash.h"
#include "engine/paths.h"
#include "scheduler_support.h"

using namespace fugue;
using namespace fugue::test;

namespace {

QJsonObject node(const QString &type, const QJsonObject &inputs) {
    return QJsonObject{{"type", type}, {"inputs", inputs}};
}

QJsonObject from(const QString &port) { return QJsonObject{{"from", port}}; }

QJsonObject model(const QString &name) { return QJsonObject{{"resource", "fugue/image-model"}, {"name", name}}; }

// g -> s: one provider node and a sink.
QJsonObject generateGraph(const QString &prompt, const QJsonObject &modelHandle = {}) {
    QJsonObject inputs{{"prompt", prompt}};
    if (!modelHandle.isEmpty())
        inputs.insert("model", modelHandle);
    return QJsonObject{{"g", node("fugue/generate", inputs)}, {"s", node("fugue/save", {{"value", from("g.text")}})}};
}

const QMap<QString, QByteArray> picture{{"result.txt", "a picture"}};

// A node of several jobs in turn (ProviderKernel::next): `parts` jobs, each placed in a part of the
// node's bar, the last one's result the node's. abandon() is counted.
class ChunkedKernel : public FakeGenerateKernel {
public:
    explicit ChunkedKernel(int parts) : m_parts(parts) {}
    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        Result<JobRequest> first = FakeGenerateKernel::request(context, selection);
        first.value().progressSpan = 1.0 / m_parts;
        return first;
    }
    Result<std::optional<JobRequest>> next(const NodeContext &, const Selection &selection, const JobResult &) override {
        if (++m_finished >= m_parts)
            return std::optional<JobRequest>();
        JobRequest request;
        request.capability = QStringLiteral("image.generate");
        request.model = selection.model.handle;
        request.params = QJsonObject{{"prompt", QStringLiteral("part %1").arg(m_finished.load())}};
        request.progressFrom = double(m_finished) / m_parts;
        request.progressSpan = 1.0 / m_parts;
        return std::optional<JobRequest>(request);
    }
    void abandon(const NodeContext &) override { ++m_abandoned; }
    std::atomic<int> m_finished{0};
    std::atomic<int> m_abandoned{0};

private:
    int m_parts;
};

// A stage-like node (#395): it names no model to the provider (any ready one) and is given a model's files in the job.
class StagedKernel : public FakeGenerateKernel {
public:
    Requirement requirement(const NodeType &type, const Values &inputs) const override {
        Requirement requirement = FakeGenerateKernel::requirement(type, inputs);
        requirement.model = {};
        return requirement;
    }
};

// A paired helper: a remote instance that does not share this machine's files.
std::shared_ptr<FakeProvider> pairedHelper(World &world, const QString &name) {
    const auto helper = world.provider(name, {"any"}, "http://gx10.example.ts.net:8787");
    world.providers.last().instance.type = QStringLiteral("fugue-helper");
    Description description = imageDescription({"any"});
    description.sharesFilesystem = false;
    description.engine = QStringLiteral("fake@gx10"); // the host's own engine id, as a real helper's: its results are its own in the cache
    helper->setDescription(description);
    return helper;
}

QJsonObject elsewhere() {
    return QJsonObject{{"resource", "fugue/lora"}, {"hash", sha256(QByteArray("only-on-the-host"))}, {"name", "only-on-the-host.safetensors"}};
}

QJsonObject stagedGraph(const QJsonObject &modelHandle) {
    return QJsonObject{{"g", node("fugue/staged", {{"prompt", "a cat"}, {"model", modelHandle}})},
                       {"s", node("fugue/save", {{"value", from("g.text")}})}};
}

void addStagedType(World &world) {
    world.type("fugue/staged",
               "'inputs': [{'name': 'prompt', 'type': 'text'}, {'name': 'model', 'type': 'fugue/lora', 'optional': true}],"
               "'outputs': [{'name': 'text', 'type': 'text'}, {'name': 'model', 'type': 'text'}]",
               nullptr);
    world.kernels.add("fugue/staged", std::make_shared<StagedKernel>());
}

// A provider whose one job reports a preview frame and then finishes.
class PreviewingProvider : public Provider {
public:
    void describe(Reply<Description> reply) override { later(std::move(reply), Result<Description>(imageDescription({"flux"}))); }
    void submit(const JobRequest &request, Reply<QString> reply) override {
        m_request = request;
        later(std::move(reply), Result<QString>(QStringLiteral("job-1")));
    }
    void status(const QString &, Reply<JobStatus> reply) override { later(std::move(reply), Result<JobStatus>(m_status)); }
    void cancel(const QString &, Reply<QString> reply) override { later(std::move(reply), Result<QString>(m_status.state)); }
    void result(const QString &, Reply<JobResult> reply) override {
        const QString path = m_request.outputDir + "/result.txt";
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write("a picture");
        later(std::move(reply), Result<JobResult>(JobResult{{{"result", QJsonObject{{"path", path}}}}, {}}));
    }

    bool submitted() const { return !m_request.outputDir.isEmpty(); }
    void report(const QString &state, const QByteArray &preview) {
        m_status.state = state;
        m_status.progress = 0.25;
        m_status.preview = preview;
        emit statusChanged(QStringLiteral("job-1"), m_status);
    }

private:
    JobRequest m_request;
    JobStatus m_status;
};

} // namespace

class TestSchedulerProviders : public QObject {
    Q_OBJECT

private slots:
    // What a helper last said is kept on disk (#492): every test starts without it.
    void init() { QFile::remove(stateDir() + QStringLiteral("/paired_helpers.json")); }

    // --- provider nodes (8.3, 8.11) ---

    void aProviderNodeRunsAsAJobOnTheProviderSelected() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        const CompiledGraph graph = world.graph(generateGraph("a cat"));

        const RunFolder folder = world.folder();
        Run run(graph, folder, world.environment());
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        const QString job = provider->jobs().first();
        const JobRequest sent = provider->submitted(job);
        QCOMPARE(sent.capability, QStringLiteral("image.generate"));
        QCOMPARE(sent.params.value("prompt").toString(), QStringLiteral("a cat"));
        QCOMPARE(sent.model.value("name").toString(), QStringLiteral("flux"));
        QCOMPARE(sent.outputDir, folder.path() + "/nodes/g");

        provider->progress(job, 0.5, "sampling");
        provider->finish(job, picture);
        QTRY_VERIFY(run.isFinished());

        const RunOutcome outcome = run.outcome();
        QCOMPARE(outcome.status, RunStatus::Ok);
        QCOMPARE(outcome.outputs.value("g").value("text").text(), QStringLiteral("a picture"));
        QCOMPARE(outcome.outputs.value("g").value("model").text(), QStringLiteral("flux"));
        const QList<QJsonObject> events = folder.events();
        QCOMPARE(trace(events), (QStringList{"run.started", "node.running g", "node.done g", "node.running s",
                                             "node.done s", "run.done ok"}));
        QJsonObject progress;
        for (const QJsonObject &event : events)
            if (event.value("event").toString() == QLatin1String("node.progress"))
                progress = event;
        QCOMPARE(progress.value("node").toString(), QStringLiteral("g"));
        QCOMPARE(progress.value("value").toDouble(), 0.5);
        QCOMPARE(progress.value("stage").toString(), QStringLiteral("sampling"));

        // run.json says which provider, engine and model served the node.
        const QJsonObject served = folder.state().value("providers").toObject().value("g").toObject();
        QCOMPARE(served.value("instance").toString(), QStringLiteral("This machine"));
        QCOMPARE(served.value("engine").toString(), QStringLiteral("fake"));
        QCOMPARE(served.value("model").toObject().value("name").toString(), QStringLiteral("flux"));

        // The same graph again is a cache hit: no second job.
        const Ran again = world.run(graph);
        QCOMPARE(again.outcome.status, RunStatus::Ok);
        QCOMPARE(provider->jobs().size(), 1);
        QCOMPARE(trace(again.events), (QStringList{"run.started", "node.cached g", "node.cached s", "run.done ok"}));
    }

    void theKeyUsesTheEngineAndModelNotTheInstance() {
        World world;
        const auto first = world.provider("This machine", {"flux"});
        const CompiledGraph graph = world.graph(generateGraph("a cat"));
        {
            Run run(graph, world.folder(), world.environment());
            run.start();
            QTRY_COMPARE(first->jobs().size(), 1);
            first->finish(first->jobs().first(), picture);
            QTRY_VERIFY(run.isFinished());
        }

        // The same engine and model at another address: the same key.
        world.providers.clear();
        const auto elsewhere = world.provider("Studio box", {"flux"}, "https://studio.example.ts.net");
        const Ran hit = world.run(graph);
        QCOMPARE(trace(hit.events), (QStringList{"run.started", "node.cached g", "node.cached s", "run.done ok"}));
        QVERIFY(elsewhere->jobs().isEmpty());

        // Another engine is another key, so the node runs.
        world.providers.clear();
        const auto other = world.provider("Other engine", {"flux"});
        other->setDescription(imageDescription({"flux"}, "other"));
        Run run(graph, world.folder(), world.environment());
        run.start();
        QTRY_COMPARE(other->jobs().size(), 1);
        other->finish(other->jobs().first(), picture);
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().ran, QStringList{"g"});
    }

    void aPinnedNodeGoesToItsInstance() {
        World world;
        const auto first = world.provider("This machine", {"flux"});
        const auto second = world.provider("Studio box", {"flux"}, "https://studio.example.ts.net");
        QJsonObject nodes = generateGraph("a cat");
        QJsonObject g = nodes.value("g").toObject();
        g.insert("provider", "Studio box");
        nodes.insert("g", g);

        Run run(world.graph(nodes), world.folder(), world.environment());
        run.start();
        QTRY_COMPARE(second->jobs().size(), 1);
        QVERIFY(first->jobs().isEmpty());
        second->finish(second->jobs().first(), picture);
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);
    }

    void aMissingModelIsAnErrorOnTheNodeWhenTheRunReachesIt() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        QJsonObject nodes = generateGraph("a cat", model("no-such-model"));
        nodes.insert("d", node("fugue/text", {{"text", "beside"}}));
        nodes.insert("t", node("fugue/save", {{"value", from("d.text")}}));

        const Ran ran = world.run(world.graph(nodes));
        QCOMPARE(ran.outcome.status, RunStatus::Failed);
        QCOMPARE(ran.outcome.errors.size(), 1);
        QCOMPARE(ran.outcome.errors.first().code, QStringLiteral("model_missing"));
        QCOMPARE(ran.outcome.errors.first().severity, Error::Severity::Error);
        QCOMPARE(ran.outcome.errors.first().node, QStringLiteral("g"));
        QCOMPARE(traceOf(ran.events, {"g", "s"}),
                 (QStringList{"node.error g model_missing", "node.skipped s upstream_failed"}));
        QCOMPARE(ran.outcome.states.value("t"), NodeState::Cached);
        QVERIFY(provider->jobs().isEmpty());
    }

    void aJobsPreviewFrameBecomesAnEvent() {
        World world;
        auto provider = std::make_shared<PreviewingProvider>();
        ProviderInstance instance;
        instance.name = QStringLiteral("This machine");
        instance.type = QStringLiteral("fake");
        world.providers.append(ProviderEntry{instance, provider});

        const RunFolder folder = world.folder();
        Run run(world.graph(generateGraph("a cat")), folder, world.environment());
        run.start();
        QTRY_VERIFY(provider->submitted());
        // The status may come before the reply that carries the job id.
        provider->report("running", "png bytes");
        QTRY_VERIFY(!trace(folder.events(), true).filter("node.progress").isEmpty());
        provider->report("done", {});
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);

        QJsonObject preview;
        for (const QJsonObject &event : folder.events())
            if (event.value("event").toString() == QLatin1String("node.preview"))
                preview = event;
        QCOMPARE(preview.value("node").toString(), QStringLiteral("g"));
        QFile file(preview.value("image").toObject().value("path").toString());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("png bytes"));
    }

    void aFailedJobIsTheNodesError() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        const RunFolder folder = world.folder();
        Run run(world.graph(generateGraph("a cat")), folder, world.environment());
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->fail(provider->jobs().first(), makeError("provider_unreachable", "The server went away."));
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Failed);
        QCOMPARE(run.outcome().errors.first().code, QStringLiteral("provider_unreachable"));
        QCOMPARE(run.outcome().errors.first().node, QStringLiteral("g"));
        QCOMPARE(traceOf(folder.events(), {"g", "s"}),
                 (QStringList{"node.running g", "node.error g provider_unreachable", "node.skipped s upstream_failed"}));
    }

    // --- lanes (9.2) ---

    void aProviderInstanceRunsOneJobAtATime() {
        World world;
        const auto provider = world.provider("Studio box", {"flux"}, "https://studio.example.ts.net");
        const QJsonObject nodes{{"g1", node("fugue/generate", {{"prompt", "one"}})},
                                {"g2", node("fugue/generate", {{"prompt", "two"}})},
                                {"s1", node("fugue/save", {{"value", from("g1.text")}})},
                                {"s2", node("fugue/save", {{"value", from("g2.text")}})}};
        const RunFolder folder = world.folder();
        Run run(world.graph(nodes), folder, world.environment());
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        // Both were ready in the same pass, and only one was started.
        QCOMPARE(traceOf(folder.events(), {"g1", "g2"}), QStringList{"node.running g1"});
        provider->finish(provider->jobs().at(0), picture);
        QTRY_COMPARE(provider->jobs().size(), 2);
        provider->finish(provider->jobs().at(1), {{"result.txt", "another"}});
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(traceOf(folder.events(), {"g1", "g2"}),
                 (QStringList{"node.running g1", "node.done g1", "node.running g2", "node.done g2"}));
    }

    void twoInstancesRunTheirJobsSideBySide() {
        World world;
        const auto first = world.provider("Studio box", {"flux"}, "https://studio.example.ts.net");
        const auto second = world.provider("Other box", {"flux"}, "https://other.example.ts.net");
        QJsonObject pinned = node("fugue/generate", {{"prompt", "two"}});
        pinned.insert("provider", "Other box");
        const QJsonObject nodes{{"g1", node("fugue/generate", {{"prompt", "one"}})},
                                {"g2", pinned},
                                {"s1", node("fugue/save", {{"value", from("g1.text")}})},
                                {"s2", node("fugue/save", {{"value", from("g2.text")}})}};
        Run run(world.graph(nodes), world.folder(), world.environment());
        run.start();
        QTRY_COMPARE(first->jobs().size(), 1);
        QTRY_COMPARE(second->jobs().size(), 1);
        first->finish(first->jobs().first(), picture);
        second->finish(second->jobs().first(), picture);
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);
    }

    void aJobOnThisMachineTakesTheGpuLane() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        QSemaphore entered;
        QSemaphore released;
        world.kernel("fugue/gpu", [&](const NodeContext &context) -> Result<Values> {
            entered.release();
            released.acquire();
            return Values{{"text", context.input("text")}};
        });
        const QJsonObject nodes{{"a", node("fugue/gpu", {{"text", "x"}})},
                                {"g", node("fugue/generate", {{"prompt", "one"}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})},
                                {"t", node("fugue/save", {{"value", from("g.text")}})}};
        const RunFolder folder = world.folder();
        Run run(world.graph(nodes), folder, world.environment());
        run.start();
        QTRY_VERIFY(entered.available() > 0);
        // The GPU node holds the lane, so the local job has not been started.
        QCOMPARE(traceOf(folder.events(), {"a", "g"}), QStringList{"node.running a"});
        released.release();
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->finish(provider->jobs().first(), picture);
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(traceOf(folder.events(), {"a", "g"}),
                 (QStringList{"node.running a", "node.done a", "node.running g", "node.done g"}));
    }

    void aJobOnARemoteInstanceDoesNotTakeTheGpuLane() {
        World world;
        const auto provider = world.provider("Studio box", {"flux"}, "https://studio.example.ts.net");
        QSemaphore released;
        world.kernel("fugue/gpu", [&](const NodeContext &context) -> Result<Values> {
            released.acquire();
            return Values{{"text", context.input("text")}};
        });
        const QJsonObject nodes{{"a", node("fugue/gpu", {{"text", "x"}})},
                                {"g", node("fugue/generate", {{"prompt", "one"}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})},
                                {"t", node("fugue/save", {{"value", from("g.text")}})}};
        Run run(world.graph(nodes), world.folder(), world.environment());
        run.start();
        // The job is submitted while the GPU node is still running.
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->finish(provider->jobs().first(), picture);
        released.release();
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);
    }

    // --- cancel (9.6) ---

    void cancelCancelsTheProviderJobAndWaitsForIt() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        QStringList states;
        connect(provider.get(), &Provider::statusChanged, this,
                [&](const QString &, const JobStatus &status) { states.append(status.state); });
        const RunFolder folder = world.folder();
        Run run(world.graph(generateGraph("a cat")), folder, world.environment());
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        run.cancel();
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(states, QStringList{"cancelled"});
        QCOMPARE(run.outcome().status, RunStatus::Cancelled);
        QVERIFY(run.outcome().errors.isEmpty());
        QCOMPARE(trace(folder.events()),
                 (QStringList{"run.started", "node.running g", "node.skipped g cancelled", "node.skipped s cancelled",
                              "run.done cancelled"}));
    }

    // --- a node of several jobs (#138) ---

    void aNodeCanRunSeveralJobsInTurnAndItsBarSpansThemAll() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        world.type("fugue/generate-chunks",
                   "'inputs': [{'name': 'prompt', 'type': 'text'}],"
                   "'outputs': [{'name': 'text', 'type': 'text'}, {'name': 'model', 'type': 'text'}]",
                   nullptr);
        const auto kernel = std::make_shared<ChunkedKernel>(2);
        world.kernels.add("fugue/generate-chunks", kernel);
        const QJsonObject nodes{{"g", node("fugue/generate-chunks", {{"prompt", "a cat"}})},
                                {"s", node("fugue/save", {{"value", from("g.text")}})}};
        const RunFolder folder = world.folder();
        Run run(world.graph(nodes), folder, world.environment());
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->progress(provider->jobs().at(0), 0.5, "chunk");
        provider->finish(provider->jobs().at(0), picture);
        QTRY_COMPARE(provider->jobs().size(), 2); // the next job, on the same provider
        QCOMPARE(provider->submitted(provider->jobs().at(1)).params.value("prompt").toString(), QStringLiteral("part 1"));
        QCOMPARE(provider->submitted(provider->jobs().at(1)).outputDir, provider->submitted(provider->jobs().at(0)).outputDir);
        provider->progress(provider->jobs().at(1), 0.5, "chunk");
        provider->finish(provider->jobs().at(1), picture);
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);
        QCOMPARE(kernel->m_abandoned.load(), 0);
        QList<double> values;
        for (const QJsonObject &event : folder.events())
            if (event.value("event").toString() == QLatin1String("node.progress"))
                values.append(event.value("value").toDouble());
        QCOMPARE(values, (QList<double>{0.25, 0.75}));
    }

    void cancellingANodeOfSeveralJobsLetsItCleanUp() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        world.type("fugue/generate-chunks",
                   "'inputs': [{'name': 'prompt', 'type': 'text'}],"
                   "'outputs': [{'name': 'text', 'type': 'text'}, {'name': 'model', 'type': 'text'}]",
                   nullptr);
        const auto kernel = std::make_shared<ChunkedKernel>(3);
        world.kernels.add("fugue/generate-chunks", kernel);
        const QJsonObject nodes{{"g", node("fugue/generate-chunks", {{"prompt", "a cat"}})},
                                {"s", node("fugue/save", {{"value", from("g.text")}})}};
        Run run(world.graph(nodes), world.folder(), world.environment());
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->finish(provider->jobs().at(0), picture);
        QTRY_COMPARE(provider->jobs().size(), 2);
        run.cancel();
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Cancelled);
        QCOMPARE(kernel->m_abandoned.load(), 1);
    }

    void aFailedNodeIsAbandonedToo() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        world.type("fugue/generate-chunks",
                   "'inputs': [{'name': 'prompt', 'type': 'text'}],"
                   "'outputs': [{'name': 'text', 'type': 'text'}, {'name': 'model', 'type': 'text'}]",
                   nullptr);
        const auto kernel = std::make_shared<ChunkedKernel>(3);
        world.kernels.add("fugue/generate-chunks", kernel);
        const QJsonObject nodes{{"g", node("fugue/generate-chunks", {{"prompt", "a cat"}})},
                                {"s", node("fugue/save", {{"value", from("g.text")}})}};
        Run run(world.graph(nodes), world.folder(), world.environment());
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->fail(provider->jobs().at(0), makeError("helper_failed", "no"));
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Failed);
        QCOMPARE(kernel->m_abandoned.load(), 1);
    }

    // --- residency (9.3) ---

    void aNodeThatReusesTheLoadedModelGoesFirst() {
        World world;
        const auto provider = world.provider("This machine", {"flux", "sdxl"});
        const QJsonObject nodes{{"g1", node("fugue/generate", {{"prompt", "one"}, {"model", model("flux")}})},
                                {"g2", node("fugue/generate", {{"prompt", "two"}, {"model", model("sdxl")}})},
                                {"g3", node("fugue/generate", {{"prompt", "three"}, {"model", model("flux")}})},
                                {"s1", node("fugue/save", {{"value", from("g1.text")}})},
                                {"s2", node("fugue/save", {{"value", from("g2.text")}})},
                                {"s3", node("fugue/save", {{"value", from("g3.text")}})}};
        Run run(world.graph(nodes), world.folder(), world.environment());
        run.start();
        QStringList served;
        for (int i = 0; i < 3; ++i) {
            QTRY_COMPARE(provider->jobs().size(), i + 1);
            const QString job = provider->jobs().at(i);
            served.append(provider->submitted(job).model.value("name").toString());
            provider->finish(job, {{"result.txt", QByteArray::number(i)}});
        }
        QTRY_VERIFY(run.isFinished());
        // In path order sdxl would be second; flux is loaded, so g3 goes before g2.
        QCOMPARE(served, (QStringList{"flux", "flux", "sdxl"}));
        QCOMPARE(run.outcome().ran.mid(0, 1), QStringList{"g1"});
        QVERIFY(run.outcome().ran.indexOf("g3") < run.outcome().ran.indexOf("g2"));
    }

    void outOfMemoryDropsTheLeastRecentlyUsedModelAndTriesAgain() {
        World world;
        const QJsonObject lora{{"resource", "fugue/lora"}, {"hash", sha256Of("lora")}, {"name", "detail.safetensors"}};
        const QString own = resourceIdentity(lora);
        std::atomic<int> attempts{0};
        world.kernel("fugue/gpu-model", [&](const NodeContext &) -> Result<Values> {
            if (++attempts == 1)
                return makeError("out_of_memory", "The GPU ran out of memory.");
            return Values{{"text", "loaded"}};
        });
        world.models.put("old");
        world.models.put(own);
        world.models.put("recent");

        const QJsonObject nodes{{"a", node("fugue/gpu-model", {{"model", lora}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        const Ran ran = world.run(world.graph(nodes));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(attempts.load(), 2);
        // Only the least recently used went, and the node's own model was used.
        QCOMPARE(world.models.keys(), (QStringList{"recent", own}));
        QCOMPARE(traceOf(ran.events, {"a"}), (QStringList{"node.running a", "node.running a", "node.done a"}));
    }

    void outOfMemoryWithNothingToDropIsTheNodesError() {
        World world;
        const QJsonObject lora{{"resource", "fugue/lora"}, {"hash", sha256Of("lora")}, {"name", "detail.safetensors"}};
        world.kernel("fugue/gpu-model", [](const NodeContext &) -> Result<Values> {
            return makeError("out_of_memory", "The GPU ran out of memory.");
        });
        world.models.put(resourceIdentity(lora)); // the node's own model is never the one dropped

        const QJsonObject nodes{{"a", node("fugue/gpu-model", {{"model", lora}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        const Ran ran = world.run(world.graph(nodes));
        QCOMPARE(ran.outcome.status, RunStatus::Failed);
        QCOMPARE(ran.outcome.errors.first().code, QStringLiteral("out_of_memory"));
        QCOMPARE(world.calls.count("a"), 1);
        QCOMPARE(world.models.keys().size(), 1);
    }

    void beforeGpuWorkModelsAreDroppedWhileMemoryIsShort() {
        World world;
        world.type("fugue/gpu-big",
                   "'needs': {'gpu': {'api': 'any', 'vram_mb': 4000}},"
                   "'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'text', 'type': 'text'}]",
                   [](const NodeContext &context) -> Result<Values> { return Values{{"text", context.input("text")}}; });
        world.models.put("oldest");
        world.models.put("older");
        world.models.put("newest");
        RunEnvironment environment = world.environment();
        // 8 GB in all, and each loaded model holds 3 GB.
        environment.freeGpuMemoryMb = [&world] { return qint64(8000 - 3000 * world.models.keys().size()); };

        const QJsonObject nodes{{"a", node("fugue/gpu-big", {{"text", "x"}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        const RunOutcome outcome = runGraph(world.graph(nodes), world.folder(), environment);
        QCOMPARE(outcome.status, RunStatus::Ok);
        // 4 GB were needed: two models had to go, least recently used first.
        QCOMPARE(world.models.keys(), QStringList{"newest"});
    }

    // 8.10: a provider Fugue started (the helper) gives its memory back only by stopping, so
    // when the last model it was recorded holding is dropped, it is told to shut down.
    void droppingTheLastModelOfAProviderShutsItDown() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        world.type("fugue/gpu-big",
                   "'needs': {'gpu': {'api': 'any', 'vram_mb': 4000}},"
                   "'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'text', 'type': 'text'}]",
                   [](const NodeContext &context) -> Result<Values> { return Values{{"text", context.input("text")}}; });
        RunEnvironment environment = world.environment();
        environment.freeGpuMemoryMb = [&world] { return qint64(8000 - 6000 * world.models.keys().size()); };

        const QJsonObject nodes{{"g", node("fugue/generate", {{"prompt", "one"}, {"model", model("flux")}})},
                                {"a", node("fugue/gpu-big", {{"text", from("g.text")}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        Run run(world.graph(nodes), world.folder(), environment);
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->finish(provider->jobs().first(), {{"result.txt", "cat"}});
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);
        QVERIFY(world.models.keys().isEmpty());
        QTRY_COMPARE(provider->shutdowns(), 1);
    }

    // A node's own persistent program is among its models: making room for the node does not
    // stop the program it is about to use (7.3).
    void makingRoomKeepsTheNodesOwnProgram() {
        World world;
        QVERIFY(!hasErrors(world.registry.addManifest(json(
            "{'manifest': 1, 'id': 'fugue/gpu-prog', 'version': '1.0.0', 'name': 'Fake',"
            " 'runtime': {'kind': 'command', 'mode': 'persistent', 'entry': './run.sh'},"
            " 'needs': {'gpu': {'api': 'any', 'vram_mb': 4000}},"
            " 'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'text', 'type': 'text'}]}"))));
        world.kernel("fugue/gpu-prog", [](const NodeContext &context) -> Result<Values> {
            return Values{{"text", context.input("text")}};
        });
        world.models.put("program:fugue/gpu-prog"); // least recently used
        world.models.put("other");
        RunEnvironment environment = world.environment();
        environment.freeGpuMemoryMb = [&world] { return qint64(8000 - 3000 * world.models.keys().size()); };
        const QJsonObject nodes{{"a", node("fugue/gpu-prog", {{"text", "x"}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        QCOMPARE(runGraph(world.graph(nodes), world.folder(), environment).status, RunStatus::Ok);
        QCOMPARE(world.models.keys(), QStringList{"program:fugue/gpu-prog"});
    }

    // A model on an instance with a job in flight is in use: it is not dropped.
    void makingRoomKeepsTheModelOfABusyInstance() {
        World world;
        const auto provider = world.provider("Studio", {"flux"}, "https://studio.example.ts.net");
        world.type("fugue/gpu-big",
                   "'needs': {'gpu': {'api': 'any', 'vram_mb': 4000}},"
                   "'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'text', 'type': 'text'}]",
                   [](const NodeContext &context) -> Result<Values> { return Values{{"text", context.input("text")}}; });
        world.models.put("Studio\nflux");
        RunEnvironment environment = world.environment();
        environment.freeGpuMemoryMb = [] { return qint64(1000); };
        const QJsonObject nodes{{"a", node("fugue/generate", {{"prompt", "one"}, {"model", model("flux")}})},
                                {"b", node("fugue/gpu-big", {{"text", "x"}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})},
                                {"t", node("fugue/save", {{"value", from("b.text")}})}};
        Run run(world.graph(nodes), world.folder(), environment);
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        QTRY_COMPARE(world.calls.count("b"), 1); // it ran while Studio was busy
        QVERIFY(world.models.contains("Studio\nflux"));
        QCOMPARE(provider->shutdowns(), 0);
        provider->finish(provider->jobs().first(), {{"result.txt", "cat"}});
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);
    }

    void aFinalOutOfMemoryNamesWhatHoldsGpuMemory() {
        World world;
        world.kernel("fugue/gpu-model", [](const NodeContext &) -> Result<Values> {
            return makeError("out_of_memory", "The GPU ran out of memory.");
        });
        RunEnvironment environment = world.environment();
        environment.gpuProcesses = [] { return QList<GpuProcess>{GpuProcess{4242, "sd-server", 9876}}; };
        const QJsonObject lora{{"resource", "fugue/lora"}, {"hash", sha256Of("lora")}, {"name", "detail.safetensors"}};
        const QJsonObject nodes{{"a", node("fugue/gpu-model", {{"model", lora}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        const RunOutcome outcome = runGraph(world.graph(nodes), world.folder(), environment);
        QCOMPARE(outcome.status, RunStatus::Failed);
        const Error error = outcome.errors.first();
        QCOMPARE(error.code, QStringLiteral("out_of_memory"));
        QVERIFY2(error.message.contains("sd-server (pid 4242, 9.6 GB)"), qPrintable(error.message));
        QVERIFY2(error.hint.contains("omarchy-agent-local stop"), qPrintable(error.hint));
    }

    // --- checkGraph: what validate shows (9.1, 10.3, 8.5) ---

    void checkTellsCachedFromStaleAndRunsNothing() {
        World world;
        const char *graph = "{'a': {'type': 'fugue/text', 'inputs': {'text': 'fox'}},"
                            " 'b': {'type': 'fugue/join', 'inputs': {'a': {'from': 'a.text'}, 'b': '%1'}},"
                            " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}}}";
        const CompiledGraph ranGraph = world.graph(QString::fromLatin1(graph).arg("!").toUtf8());

        GraphCheck check = checkGraph(ranGraph, world.environment());
        QCOMPARE(check.states.value("a"), NodeState::Stale);
        QCOMPARE(check.states.value("s"), NodeState::Stale);
        QCOMPARE(world.calls.total(), 0);
        QVERIFY(!QDir(world.dir.filePath("runs")).exists());

        const Ran ran = world.run(ranGraph);
        const QList<CacheEntry> before = world.cache->entries();
        world.clock.advance(3600);

        check = checkGraph(ranGraph, world.environment());
        QCOMPARE(check.states.value("a"), NodeState::Cached);
        QCOMPARE(check.states.value("b"), NodeState::Cached);
        QCOMPARE(check.states.value("s"), NodeState::Cached);
        QCOMPARE(check.keys, ran.outcome.keys);
        QVERIFY(check.warnings.isEmpty());

        check = checkGraph(world.graph(QString::fromLatin1(graph).arg("?").toUtf8()), world.environment());
        QCOMPARE(check.states.value("a"), NodeState::Cached);
        QCOMPARE(check.states.value("b"), NodeState::Stale);
        QCOMPARE(check.states.value("s"), NodeState::Stale);

        // Looking is not using: nothing ran and no last-used time moved.
        QCOMPARE(world.calls.total(), 3);
        const QList<CacheEntry> after = world.cache->entries();
        QCOMPARE(after.size(), before.size());
        for (int i = 0; i < after.size(); ++i)
            QCOMPARE(after.at(i).lastUsed, before.at(i).lastUsed);
        QCOMPARE(QDir(world.dir.filePath("runs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
    }

    void checkShowsFrozenNodesAndWarnsWhenTheResultIsGone() {
        World world;
        const QJsonObject plain{{"a", node("fugue/text", {{"text", "fox"}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        const Ran ran = world.run(world.graph(plain));
        const QString key = ran.outcome.keys.value("a");
        QVERIFY(world.cache->freeze(key));

        QJsonObject a = node("fugue/text", {{"text", "owl"}});
        a.insert("frozen", key);
        QJsonObject frozen = plain;
        frozen.insert("a", a);
        GraphCheck check = checkGraph(world.graph(frozen), world.environment());
        QCOMPARE(check.states.value("a"), NodeState::Frozen);
        QCOMPARE(check.states.value("s"), NodeState::Cached);
        QVERIFY(check.warnings.isEmpty());

        world.cache->unfreeze(key);
        world.cache->clear();
        check = checkGraph(world.graph(frozen), world.environment());
        QCOMPARE(check.states.value("a"), NodeState::Stale);
        QCOMPARE(check.warnings.size(), 1);
        QCOMPARE(check.warnings.first().code, QStringLiteral("frozen_result_missing"));
        QCOMPARE(check.warnings.first().severity, Error::Severity::Warning);
        QCOMPARE(check.warnings.first().node, QStringLiteral("a"));
    }

    void checkCallsARandomizedSeedStale() {
        World world;
        const QJsonObject nodes{{"a", node("fugue/seeded", {{"seed", QJsonObject{{"auto", "random"}}}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        world.run(world.graph(nodes));
        const GraphCheck check = checkGraph(world.graph(nodes), world.environment());
        QCOMPARE(check.states.value("a"), NodeState::Stale);
        QCOMPARE(check.states.value("s"), NodeState::Stale);
        QCOMPARE(world.nextSeed, qint64(1001)); // checking drew no seed
    }

    void checkReportsAMissingModelAsAWarning() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        const GraphCheck check =
            checkGraph(world.graph(generateGraph("a cat", model("no-such-model"))), world.environment());
        QCOMPARE(check.states.value("g"), NodeState::Stale);
        QCOMPARE(check.warnings.size(), 1);
        QCOMPARE(check.warnings.first().code, QStringLiteral("model_missing"));
        QCOMPARE(check.warnings.first().severity, Error::Severity::Warning);
        QCOMPARE(check.warnings.first().node, QStringLiteral("g"));
        QVERIFY(provider->jobs().isEmpty());

        // A model that is there gives no warning; the node would run.
        const GraphCheck fine = checkGraph(world.graph(generateGraph("a cat", model("flux"))), world.environment());
        QVERIFY(fine.warnings.isEmpty());
        QCOMPARE(fine.states.value("g"), NodeState::Stale);
    }

    // 8.9: a node that loads a model from disk; validate warns while its file is not there.
    void checkReportsAModelThatIsNotOnDiskAsAWarning() {
        World world;
        const QJsonObject lora{{"resource", "fugue/lora"}, {"hash", sha256Of("check-lora")}, {"name", "check.safetensors"}};
        const QJsonObject nodes{{"a", node("fugue/gpu-model", {{"model", lora}})},
                                {"s", node("fugue/save", {{"value", from("a.text")}})}};
        const GraphCheck check = checkGraph(world.graph(nodes), world.environment());
        QCOMPARE(check.warnings.size(), 1);
        QCOMPARE(check.warnings.first().code, QStringLiteral("model_missing"));
        QCOMPARE(check.warnings.first().severity, Error::Severity::Warning);
        QCOMPARE(check.warnings.first().node, QStringLiteral("a"));
        QCOMPARE(check.warnings.first().input, QStringLiteral("model"));
        QCOMPARE(world.calls.count("a"), 0);

        // In Fugue's models folder (the test's own data folder), it is found.
        const QString path = dataDir() + "/models/lora/check.safetensors";
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("check-lora");
        file.close();
        const GraphCheck fine = checkGraph(world.graph(nodes), world.environment());
        QVERIFY(QFile::remove(path));
        QVERIFY2(fine.warnings.isEmpty(), qPrintable(fine.warnings.value(0).message));
    }

    // 8.5: the provider checks do not depend on the rest of the node's inputs being known.
    void checkReportsAMissingModelWhenTheSeedIsRandomized() {
        World world;
        world.provider("This machine", {"flux"});
        world.type("fugue/generate-seeded",
                   "'inputs': [{'name': 'prompt', 'type': 'text'},"
                   " {'name': 'seed', 'type': 'number', 'integer': true, 'widget': 'seed', 'min': 0, 'max': 9999},"
                   " {'name': 'model', 'type': 'fugue/image-model', 'optional': true}],"
                   "'outputs': [{'name': 'text', 'type': 'text'}, {'name': 'model', 'type': 'text'}]",
                   nullptr);
        world.kernels.add("fugue/generate-seeded", std::make_shared<FakeGenerateKernel>());
        const QJsonObject nodes{{"g", node("fugue/generate-seeded", {{"prompt", "a cat"},
                                                                     {"seed", QJsonObject{{"auto", "random"}}},
                                                                     {"model", model("no-such-model")}})},
                                {"s", node("fugue/save", {{"value", from("g.text")}})}};
        const GraphCheck check = checkGraph(world.graph(nodes), world.environment());
        QCOMPARE(check.states.value("g"), NodeState::Stale);
        QCOMPARE(check.warnings.size(), 1);
        QCOMPARE(check.warnings.first().code, QStringLiteral("model_missing"));
        QCOMPARE(check.warnings.first().severity, Error::Severity::Warning);
        QCOMPARE(check.warnings.first().node, QStringLiteral("g"));
        QCOMPARE(world.nextSeed, qint64(1000)); // checking drew no seed
    }

    void checkReportsProviderProblemsWhenAnInputIsLinkedToANodeThatHasNotRun() {
        World world; // no provider at all
        QJsonObject nodes = generateGraph("unused", model("flux"));
        nodes.insert("p", node("fugue/text", {{"text", "a cat"}}));
        nodes.insert("g", node("fugue/generate", {{"prompt", from("p.text")}}));
        GraphCheck check = checkGraph(world.graph(nodes), world.environment());
        QCOMPARE(check.states.value("p"), NodeState::Stale);
        QCOMPARE(check.states.value("g"), NodeState::Stale);
        QCOMPARE(check.warnings.size(), 1);
        QCOMPARE(check.warnings.first().code, QStringLiteral("capability_unavailable"));
        QCOMPARE(check.warnings.first().node, QStringLiteral("g"));

        // With a provider that lacks the model, the linked prompt does not hide model_missing.
        nodes.insert("g", node("fugue/generate", {{"prompt", from("p.text")}, {"model", model("no-such-model")}}));
        world.provider("This machine", {"flux"});
        check = checkGraph(world.graph(nodes), world.environment());
        QCOMPARE(check.warnings.size(), 1);
        QCOMPARE(check.warnings.first().code, QStringLiteral("model_missing"));
    }

    // --- model files that are not on this machine (#395) ---

    void anAppWhoseModelIsOnlyOnThePairedHelperRunsThereAndRunJsonSaysSo() {
        World world;
        addStagedType(world);
        const auto local = world.provider("This machine", {"any"});
        const auto helper = pairedHelper(world, "gx10");
        const RunFolder folder = world.folder();
        Run run(world.graph(stagedGraph(elsewhere())), folder, world.environment());
        run.start();
        QTRY_COMPARE(helper->jobs().size(), 1);
        helper->finish(helper->jobs().first(), picture);
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);
        QCOMPARE(local->jobs().size(), 0);
        const QJsonObject served = folder.state().value("providers").toObject().value("g").toObject();
        QCOMPARE(served.value("instance").toString(), QStringLiteral("gx10"));
        QVERIFY(served.value("remote").toBool());
    }

    void aGoneHelperWithALocalCopyRunsHere() {
        World world;
        addStagedType(world);
        const auto local = world.provider("This machine", {"any"});
        const auto helper = pairedHelper(world, "gx10");
        helper->setDescribeErrors({makeError("provider_unreachable", "cannot reach it: timed out.")});
        const QString path = dataDir() + "/models/lora/staged-here.safetensors";
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("staged-here");
        file.close();
        const QJsonObject here{{"resource", "fugue/lora"}, {"hash", sha256Of("staged-here")}, {"name", "staged-here.safetensors"}};
        Run run(world.graph(stagedGraph(here)), world.folder(), world.environment());
        run.start();
        QTRY_COMPARE(local->jobs().size(), 1);
        local->finish(local->jobs().first(), picture);
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(helper->jobs().size(), 0);
        QVERIFY(QFile::remove(path));
    }

    // An input the provider serves by name (no sha256) is not a model file missing here: the run stays where the order puts it.
    void aNameOnlyInputDoesNotSendTheNodeToThePairedHelper() {
        World world;
        addStagedType(world);
        const auto local = world.provider("This machine", {"any"});
        const auto helper = pairedHelper(world, "gx10");
        const QJsonObject byName{{"resource", "fugue/lora"}, {"name", "served-by-name"}};
        Run run(world.graph(stagedGraph(byName)), world.folder(), world.environment());
        run.start();
        QTRY_COMPARE(local->jobs().size(), 1);
        local->finish(local->jobs().first(), picture);
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(helper->jobs().size(), 0);
    }

    // A cached node did not run anywhere: run.json does not say it ran remotely.
    void aCacheHitIsNotRecordedAsRemote() {
        World world;
        addStagedType(world);
        const auto helper = pairedHelper(world, "gx10");
        const CompiledGraph graph = world.graph(stagedGraph(elsewhere()));
        {
            Run run(graph, world.folder(), world.environment());
            run.start();
            QTRY_COMPARE(helper->jobs().size(), 1);
            helper->finish(helper->jobs().first(), picture);
            QTRY_VERIFY(run.isFinished());
        }
        const RunFolder folder = world.folder();
        Run again(graph, folder, world.environment());
        again.start();
        QTRY_VERIFY(again.isFinished());
        QCOMPARE(helper->jobs().size(), 1);
        QVERIFY(!folder.state().value("providers").toObject().value("g").toObject().value("remote").toBool());
    }

    // #492: a result made on the helper stays in the cache under its engine id; with the host down it is served from there.
    void aResultMadeOnTheHelperIsServedFromTheCacheWhileItIsDown() {
        World world;
        addStagedType(world);
        const auto local = world.provider("This machine", {"any"});
        const auto helper = pairedHelper(world, "gx10");
        const CompiledGraph graph = world.graph(stagedGraph(elsewhere()));
        {
            Run run(graph, world.folder(), world.environment());
            run.start();
            QTRY_COMPARE(helper->jobs().size(), 1);
            helper->finish(helper->jobs().first(), picture);
            QTRY_VERIFY(run.isFinished());
        }
        helper->setDescribeErrors({makeError("provider_unreachable", "cannot reach it: timed out.")});
        Run again(graph, world.folder(), world.environment());
        again.start();
        QTRY_VERIFY(again.isFinished());
        QCOMPARE(again.outcome().status, RunStatus::Ok);
        QCOMPARE(again.outcome().states.value("g"), NodeState::Cached);
        QCOMPARE(helper->jobs().size(), 1);
        QCOMPARE(local->jobs().size(), 0);
    }

    // The same with another input: nothing is cached for it, so the host being gone is the error, not a hang or a local run.
    void aCacheMissWhileTheHelperIsDownStillSaysSo() {
        World world;
        addStagedType(world);
        const auto local = world.provider("This machine", {"any"});
        const auto helper = pairedHelper(world, "gx10");
        {
            Run run(world.graph(stagedGraph(elsewhere())), world.folder(), world.environment());
            run.start();
            QTRY_COMPARE(helper->jobs().size(), 1);
            helper->finish(helper->jobs().first(), picture);
            QTRY_VERIFY(run.isFinished());
        }
        helper->setDescribeErrors({makeError("provider_unreachable", "cannot reach it: timed out.")});
        QJsonObject other = stagedGraph(elsewhere());
        other.insert("g", node("fugue/staged", {{"prompt", "a dog"}, {"model", elsewhere()}}));
        Run run(world.graph(other), world.folder(), world.environment());
        run.start();
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Failed);
        QCOMPARE(run.outcome().errors.first().code, QStringLiteral("provider_unreachable"));
        QCOMPARE(local->jobs().size(), 0);
    }

    // #491: a host that could never serve the node (it did not offer its capability when it last answered) is not named: the node fails
    // at selection with the model that is missing here, and no job is started on this machine to fail late.
    void aGoneHelperThatNeverOfferedTheCapabilityDoesNotHideTheMissingModel() {
        World world;
        addStagedType(world);
        const auto local = world.provider("This machine", {"any"});
        const auto helper = pairedHelper(world, "gx10");
        Description offered = imageDescription({"any"});
        offered.capabilities = {CapabilityOffer{QStringLiteral("text.generate"), {}}};
        offered.engine = QStringLiteral("fake@gx10");
        rememberPairedHelper(world.providers.last().instance, offered);
        helper->setDescribeErrors({makeError("provider_unreachable", "cannot reach it: timed out.")});
        Run run(world.graph(stagedGraph(QJsonObject{{"resource", "fugue/lora"}, {"hash", sha256Of("not-anywhere")}, {"name", "not-anywhere.safetensors"}})),
                world.folder(), world.environment());
        run.start();
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().errors.first().code, QStringLiteral("model_missing"));
        QCOMPARE(local->jobs().size(), 0);
    }

    // Memory stands in only for a choice the live host could not make differently: a host that offered several models would pick one now.
    void aHelperThatOfferedSeveralModelsDoesNotAnswerFromTheCacheWhileDown() {
        World world;
        addStagedType(world);
        world.provider("This machine", {"any"});
        const auto helper = world.provider("gx10", {"a", "b"}, "http://gx10.example.ts.net:8787");
        world.providers.last().instance.type = QStringLiteral("fugue-helper");
        Description description = imageDescription({"a", "b"}, "fake@gx10");
        description.sharesFilesystem = false;
        helper->setDescription(description);
        const CompiledGraph graph = world.graph(stagedGraph(elsewhere()));
        {
            Run run(graph, world.folder(), world.environment());
            run.start();
            QTRY_COMPARE(helper->jobs().size(), 1);
            helper->finish(helper->jobs().first(), picture);
            QTRY_VERIFY(run.isFinished());
        }
        helper->setDescribeErrors({makeError("provider_unreachable", "cannot reach it: timed out.")});
        Run again(graph, world.folder(), world.environment());
        again.start();
        QTRY_VERIFY(again.isFinished());
        QCOMPARE(again.outcome().status, RunStatus::Failed);
        QCOMPARE(again.outcome().errors.first().code, QStringLiteral("provider_unreachable"));
    }

    void aGoneHelperAndNoLocalCopyIsAClearErrorNotAHang() {
        World world;
        addStagedType(world);
        world.provider("This machine", {"any"});
        const auto helper = pairedHelper(world, "gx10");
        helper->setDescribeErrors({makeError("provider_unreachable", "cannot reach it: timed out.")});
        Run run(world.graph(stagedGraph(elsewhere())), world.folder(), world.environment());
        run.start();
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Failed);
        const Error error = run.outcome().errors.first();
        QCOMPARE(error.code, QStringLiteral("provider_unreachable"));
        QVERIFY(error.message.contains("gx10"));
        QCOMPARE(error.node, QStringLiteral("g"));
    }

private:
    static QString sha256Of(const QByteArray &bytes) { return sha256(bytes); }
};

QTEST_GUILESS_MAIN(TestSchedulerProviders)
#include "test_scheduler_providers.moc"
