#pragma once

// A small world for scheduler tests: fake node types with fake kernels, a
// result cache and run folders in a temporary directory, a fake clock and a
// fake seed source. Nothing here sleeps.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <functional>
#include <memory>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/fake_provider.h"
#include "engine/file_hashes.h"
#include "engine/registry.h"
#include "engine/scheduler.h"

namespace fugue::test {

// JSON written with single quotes (moc skips files with raw string literals).
inline QJsonObject json(const QByteArray &text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (!document.isObject())
        qFatal("bad test JSON: %s in %s", qPrintable(error.errorString()), text.constData());
    return document.object();
}

using KernelFunction = std::function<Result<Values>(const NodeContext &)>;

// Which nodes' kernels were called, in call order. Thread-safe.
class Calls {
public:
    void add(const QString &node) {
        QMutexLocker lock(&m_mutex);
        m_nodes.append(node);
    }
    QStringList all() const {
        QMutexLocker lock(&m_mutex);
        return m_nodes;
    }
    int count(const QString &node) const { return int(all().count(node)); }
    int total() const { return int(all().size()); }

private:
    mutable QMutex m_mutex;
    QStringList m_nodes;
};

class FakeClock {
public:
    QDateTime now() const {
        QMutexLocker lock(&m_mutex);
        return m_now;
    }
    void advance(int seconds) {
        QMutexLocker lock(&m_mutex);
        m_now = m_now.addSecs(seconds);
    }

private:
    mutable QMutex m_mutex;
    QDateTime m_now{QDate(2026, 11, 19), QTime(12, 0), QTimeZone::UTC};
};

// A provider node kernel for the fake provider: the prompt goes out as a
// parameter, and the job's result.txt comes back as the `text` output.
class FakeGenerateKernel : public ProviderKernel {
public:
    Requirement requirement(const NodeType &, const Values &inputs) const override {
        Requirement requirement;
        requirement.capability = QStringLiteral("image.generate");
        requirement.model = inputs.value("model").json().toObject();
        return requirement;
    }
    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        JobRequest request;
        request.capability = QStringLiteral("image.generate");
        request.model = selection.model.handle;
        request.params = QJsonObject{{"prompt", context.input("prompt").text()}};
        return request;
    }
    Result<Values> outputs(const NodeContext &, const Selection &selection, const JobResult &result) override {
        QFile file(result.outputs.value("result").toObject().value("path").toString());
        if (!file.open(QIODevice::ReadOnly))
            return makeError(QStringLiteral("node_bad_output"), QStringLiteral("The job gave no result.txt."));
        return Values{{"text", QString::fromUtf8(file.readAll())},
                      {"model", selection.model.handle.value("name").toString()}};
    }
};

inline OfferedModel offeredModel(const QString &name) {
    OfferedModel model;
    model.handle = QJsonObject{{"resource", "fugue/image-model"}, {"name", name}};
    model.status = ModelStatus::Ready;
    return model;
}

inline Description imageDescription(const QStringList &models, const QString &engine = QStringLiteral("fake")) {
    Description description;
    description.engine = engine;
    CapabilityOffer offer;
    offer.capability = QStringLiteral("image.generate");
    for (const QString &name : models)
        offer.models.append(offeredModel(name));
    description.capabilities.append(offer);
    return description;
}

struct Ran {
    RunOutcome outcome;
    QList<QJsonObject> events;
    QJsonObject state; // run.json
    QString path;      // the run folder
};

// The events as short lines: "node.done a", "node.skipped c upstream_failed",
// "node.error b node_failed", "run.done ok". node.queued and node.progress
// are left out unless asked for.
inline QStringList trace(const QList<QJsonObject> &events, bool everything = false) {
    QStringList lines;
    for (const QJsonObject &event : events) {
        const QString name = event.value("event").toString();
        if (!everything && (name == QLatin1String("node.queued") || name == QLatin1String("node.progress")))
            continue;
        QString line = name;
        if (event.contains("node"))
            line += QLatin1Char(' ') + event.value("node").toString();
        if (event.contains("reason"))
            line += QLatin1Char(' ') + event.value("reason").toString();
        if (event.contains("error"))
            line += QLatin1Char(' ') + event.value("error").toObject().value("code").toString();
        if (event.contains("status"))
            line += QLatin1Char(' ') + event.value("status").toString();
        lines.append(line);
    }
    return lines;
}

// Only the events of these nodes, as trace() writes them.
inline QStringList traceOf(const QList<QJsonObject> &events, const QStringList &nodes) {
    QList<QJsonObject> kept;
    for (const QJsonObject &event : events)
        if (nodes.contains(event.value("node").toString()))
            kept.append(event);
    return trace(kept);
}

class World {
public:
    explicit World(qint64 cacheLimit = ResultCache::defaultLimit) {
        hashes = std::make_unique<FileHashes>(dir.filePath("hashes.json"));
        ResultCache::Options options;
        options.limitBytes = cacheLimit;
        options.clock = [this] { return clock.now(); };
        cache = std::make_unique<ResultCache>(dir.filePath("results"), options);

        must(registry.add(DeclaredPortType{*PortType::parse("fugue/latent"), PortKind::Data, "Latent", {}}));
        must(registry.add(DeclaredPortType{*PortType::parse("fugue/lora"), PortKind::Resource, "LoRA", {}}));
        must(registry.add(DeclaredPortType{*PortType::parse("fugue/image-model"), PortKind::Resource, "Image model", {}}));

        const QByteArray text = "'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'text', 'type': 'text'}]";
        auto echo = [](const NodeContext &context) -> Result<Values> { return Values{{"text", context.input("text")}}; };

        type("fugue/text", text, echo);
        type("fugue/join",
             "'inputs': [{'name': 'a', 'type': 'text'}, {'name': 'b', 'type': 'text', 'optional': true}],"
             "'outputs': [{'name': 'text', 'type': 'text'}]",
             [](const NodeContext &context) -> Result<Values> {
                 return Values{{"text", context.input("a").text() + context.input("b").text()}};
             });
        type("fugue/constant",
             "'inputs': [{'name': 'note', 'type': 'text', 'default': ''}], 'outputs': [{'name': 'text', 'type': 'text'}]",
             [](const NodeContext &) -> Result<Values> { return Values{{"text", "same"}}; });
        type("fugue/render",
             "'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'latent', 'type': 'fugue/latent'}]",
             [](const NodeContext &context) -> Result<Values> {
                 const QString path = context.workDir + "/latent.bin";
                 QFile file(path);
                 if (!file.open(QIODevice::WriteOnly))
                     return makeError("node_failed", "Cannot write " + path);
                 file.write(context.input("text").text().toUtf8());
                 file.close();
                 return Values{{"latent", QJsonObject{{"data", "fugue/latent"}, {"path", path}}}};
             });
        type("fugue/read",
             "'inputs': [{'name': 'latent', 'type': 'fugue/latent'}],"
             "'outputs': [{'name': 'text', 'type': 'text'}, {'name': 'path', 'type': 'text'}]",
             [](const NodeContext &context) -> Result<Values> {
                 const QString path = context.input("latent").json().toObject().value("path").toString();
                 QFile file(path);
                 if (!file.open(QIODevice::ReadOnly))
                     return makeError("node_failed", "Cannot read " + path);
                 return Values{{"text", QString::fromUtf8(file.readAll())}, {"path", path}};
             });
        type("fugue/save", "'sink': true, 'inputs': [{'name': 'value', 'type': 'any'}], 'outputs': []",
             [](const NodeContext &) -> Result<Values> { return Values{}; });
        type("fugue/fail", text, [](const NodeContext &) -> Result<Values> {
            return makeError("node_failed", "The fake node failed.");
        });
        type("fugue/gpu", "'needs': {'gpu': {'api': 'any'}}," + text, echo);
        type("fugue/gpu-parallel", "'needs': {'gpu': {'api': 'any', 'parallel': true}}," + text, echo);
        type("fugue/gpu-model",
             "'needs': {'gpu': {'api': 'any'}},"
             "'inputs': [{'name': 'model', 'type': 'fugue/lora'}], 'outputs': [{'name': 'text', 'type': 'text'}]",
             [](const NodeContext &) -> Result<Values> { return Values{{"text", "loaded"}}; });
        type("fugue/seeded",
             "'inputs': [{'name': 'seed', 'type': 'number', 'integer': true, 'widget': 'seed', 'min': 0, 'max': 9999}],"
             "'outputs': [{'name': 'text', 'type': 'text'}, {'name': 'seed', 'type': 'number'}]",
             [](const NodeContext &context) -> Result<Values> {
                 return Values{{"text", QStringLiteral("seed %1").arg(qint64(context.input("seed").number()))}};
             });
        type("fugue/maybe",
             "'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'text', 'type': 'text', 'optional': true}]",
             [](const NodeContext &context) -> Result<Values> {
                 if (context.input("text").text() == QLatin1String("none"))
                     return Values{};
                 return Values{{"text", context.input("text")}};
             });
        type("fugue/dice", "'deterministic': false," + text, echo);
        type("fugue/load",
             "'inputs': [{'name': 'path', 'type': 'text', 'widget': 'file'}], 'outputs': [{'name': 'text', 'type': 'text'}]",
             [](const NodeContext &context) -> Result<Values> {
                 QFile file(context.input("path").text());
                 if (!file.open(QIODevice::ReadOnly))
                     return makeError("file_not_found", "No such file.");
                 return Values{{"text", QString::fromUtf8(file.readAll())}};
             });
        type("fugue/generate",
             "'inputs': [{'name': 'prompt', 'type': 'text'}, {'name': 'model', 'type': 'fugue/image-model', 'optional': true}],"
             "'outputs': [{'name': 'text', 'type': 'text'}, {'name': 'model', 'type': 'text'}]",
             nullptr);
        kernels.add("fugue/generate", std::make_shared<FakeGenerateKernel>());
    }

    // Registers a fake node type; `fields` are the manifest's fields beyond
    // id, version, name and runtime. Each call of its kernel is logged and
    // moves the fake clock one second on.
    void type(const QString &id, const QByteArray &fields, KernelFunction function) {
        must(registry.addManifest(json("{'manifest': 1, 'id': '" + id.toUtf8() +
                                       "', 'version': '1.0.0', 'name': 'Fake', 'runtime': {'kind': 'builtin'}," +
                                       fields + "}")));
        if (function)
            kernel(id, std::move(function));
    }

    // Replaces the kernel of a node type.
    void kernel(const QString &id, KernelFunction function) {
        class Fake : public NodeKernel {
        public:
            Fake(World *world, KernelFunction function) : m_world(world), m_function(std::move(function)) {}
            Result<Values> run(const NodeContext &context) override {
                m_world->calls.add(context.node);
                m_world->clock.advance(1);
                return m_function(context);
            }

        private:
            World *m_world;
            KernelFunction m_function;
        };
        kernels.add(id, std::make_shared<Fake>(this, std::move(function)));
    }

    // Adds a fake provider instance; an address makes it a remote one.
    std::shared_ptr<FakeProvider> provider(const QString &name, const QStringList &models, const QString &address = {}) {
        auto fake = std::make_shared<FakeProvider>(imageDescription(models));
        ProviderInstance instance;
        instance.name = name;
        instance.type = QStringLiteral("fake");
        instance.address = address;
        providers.append(ProviderEntry{instance, fake});
        return fake;
    }

    RunEnvironment environment() {
        RunEnvironment env;
        env.registry = &registry;
        env.kernels = &kernels;
        env.cache = cache.get();
        env.hashes = hashes.get();
        must(registry.setProviders(providers));
        env.models = &models;
        env.cpuSlots = cpuSlots;
        env.clock = [this] { return clock.now(); };
        env.randomSeed = [this](qint64, qint64) { return nextSeed++; };
        return env;
    }

    // Compiles a graph given as its `nodes` object; a node without a version
    // gets 1.0.0.
    CompiledGraph graph(const QByteArray &nodesText) { return graph(json(nodesText)); }
    CompiledGraph graph(QJsonObject nodes) {
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            QJsonObject node = it.value().toObject();
            if (!node.contains("version"))
                node.insert("version", "1.0.0");
            if (!node.contains("inputs"))
                node.insert("inputs", QJsonObject());
            it.value() = node;
        }
        Errors errors;
        const auto document = Document::fromJson(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}}, errors);
        if (!document)
            qFatal("bad test document: %s", qPrintable(dump(errors)));
        const Compilation compiled = compile(*document, registry);
        if (!compiled.ok())
            qFatal("test graph does not compile: %s", qPrintable(dump(compiled.errors)));
        return *compiled.graph;
    }

    RunFolder folder() {
        const auto made = RunFolder::create(dir.filePath("runs"));
        if (!made)
            qFatal("cannot make a run folder");
        return made.value();
    }

    // Runs the graph to the end through the worker entry point.
    Ran run(const CompiledGraph &graph, const RunOptions &options = {}) {
        const RunFolder runFolder = folder();
        Ran ran;
        ran.outcome = runGraph(graph, runFolder, environment(), options);
        ran.events = runFolder.events();
        ran.state = runFolder.state();
        ran.path = runFolder.path();
        return ran;
    }

    static QString dump(const Errors &errors) {
        return QString::fromUtf8(QJsonDocument(errorEnvelope(errors)).toJson(QJsonDocument::Compact));
    }

    QTemporaryDir dir;
    Registry registry;
    Kernels kernels;
    std::unique_ptr<FileHashes> hashes;
    std::unique_ptr<ResultCache> cache;
    ResidentModels models;
    QList<ProviderEntry> providers; // go into the registry when environment() is called
    FakeClock clock;
    Calls calls;
    int cpuSlots = 4;
    qint64 nextSeed = 1000;

private:
    static void must(const Errors &errors) {
        if (hasErrors(errors))
            qFatal("bad fake type: %s", qPrintable(dump(errors)));
    }
};

} // namespace fugue::test
