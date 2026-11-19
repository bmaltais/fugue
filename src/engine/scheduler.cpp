#include "scheduler.h"

#include <algorithm>
#include <exception>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QRandomGenerator>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include "engine/file_hashes.h"
#include "engine/ids.h"

namespace fugue {

QString nodeStateName(NodeState state) {
    switch (state) {
    case NodeState::Cached:
        return QStringLiteral("cached");
    case NodeState::Stale:
        return QStringLiteral("stale");
    case NodeState::Running:
        return QStringLiteral("running");
    case NodeState::Frozen:
        return QStringLiteral("frozen");
    case NodeState::Error:
        return QStringLiteral("error");
    case NodeState::Skipped:
        return QStringLiteral("skipped");
    }
    return {};
}

namespace {

const qint64 maxSeed = (1LL << 53) - 1; // the largest integer a workflow may hold (2.1)

enum class Phase { Waiting, Ready, Running, Finished };

struct NodeRun {
    QString path;
    const CompiledNode *node = nullptr;
    QStringList dependencies; // the path ids it reads from
    int readers = 0;          // nodes that read from it and have not finished
    bool settled = false;     // its finish has been counted for its dependencies
    const NodeType *type = nullptr;
    std::shared_ptr<NodeKernel> kernel;
    std::shared_ptr<ProviderKernel> providerKernel;

    Phase phase = Phase::Waiting;
    NodeState state = NodeState::Stale;
    QString reason; // why it was skipped
    Error error;

    Values inputs;
    Values outputs;
    QJsonObject autoValues; // input -> the value a randomized input got
    QString key;
    QStringList models; // what it would keep resident (9.3)

    // Provider nodes.
    std::optional<Selection> selection;
    int instance = -1;
    QString job;
    bool jobOver = false;

    // What it holds while running.
    bool holdsCpu = false;
    bool holdsGpu = false;
    bool holdsInstance = false;

    QString workDir;
    QDateTime started;
};

bool isLocal(const ProviderInstance &instance) {
    if (instance.address.isEmpty())
        return true;
    const QString host = QUrl(instance.address).host();
    return host == QLatin1String("localhost") || host == QLatin1String("::1") || host.startsWith(QLatin1String("127."));
}

Error nodeError(const QString &code, const QString &message, const QString &hint = {}) {
    return makeError(code, message, hint);
}

// Runs kernel code, turning an exception into node_failed (libvips throws).
template <typename T, typename Call> Result<T> guarded(Call call) {
    try {
        return call();
    } catch (const std::exception &thrown) {
        return nodeError(QStringLiteral("node_failed"), QString::fromUtf8(thrown.what()));
    } catch (...) {
        return nodeError(QStringLiteral("node_failed"), QStringLiteral("The node threw an unknown exception."));
    }
}

// Gives every output a file and a hash, on the worker thread: an image held
// in memory is written to <workDir>/<output>.png, and a data handle gets its
// content hash.
Result<Values> withFiles(const Values &outputs, const QString &workDir, FileHashes &hashes) {
    std::function<Result<Value>(const Value &, const QString &)> one = [&](const Value &value,
                                                                           const QString &name) -> Result<Value> {
        if (value.isMedia()) {
            const Result<MediaHandle> saved = value.media().saved(workDir, name, hashes);
            if (!saved)
                return saved.error();
            return Value(saved.value());
        }
        if (value.isData()) {
            QJsonObject handle = value.json().toObject();
            const QString path = handle.value("path").toString();
            const QString hash = hashes.hash(path);
            if (hash.isEmpty())
                return nodeError(QStringLiteral("node_bad_output"),
                                 QStringLiteral("Output %1 names a file that cannot be read: %2.").arg(name, path));
            handle.insert("hash", hash);
            return Value(handle);
        }
        if (value.isList()) {
            QList<Value> items;
            int index = 0;
            for (const Value &item : value.items()) {
                const Result<Value> made = one(item, QStringLiteral("%1-%2").arg(name).arg(index++));
                if (!made)
                    return made.error();
                items.append(made.value());
            }
            return Value::list(items);
        }
        return value;
    };

    Values out;
    for (auto it = outputs.begin(); it != outputs.end(); ++it) {
        const Result<Value> made = one(it.value(), it.key());
        if (!made) {
            Error error = made.error();
            error.output = it.key();
            return error;
        }
        out.insert(it.key(), made.value());
    }
    return out;
}

// Whether one item fits a port type. Only the built-in types are checked;
// values of declared types are the packs' business.
bool fits(const Value &value, const PortType &type) {
    if (type.id == QLatin1String("text"))
        return value.kind() == Value::Kind::Json && value.json().isString();
    if (type.id == QLatin1String("number"))
        return value.kind() == Value::Kind::Json && value.json().isDouble();
    if (type.id == QLatin1String("boolean"))
        return value.kind() == Value::Kind::Json && value.json().isBool();
    if (type.id == QLatin1String("image"))
        return value.isMedia() && value.media().type() == MediaType::Image;
    if (type.id == QLatin1String("video"))
        return value.isMedia() && value.media().type() == MediaType::Video;
    if (type.id == QLatin1String("audio"))
        return value.isMedia() && value.media().type() == MediaType::Audio;
    return !value.isList();
}

// The same values with no decoded image held: media handles keep their file
// only. A finished node's pixels stay in memory just as long as a node that
// reads them may still run.
Value fileOnly(const Value &value) {
    if (value.isMedia() && value.media().hasImage() && value.media().hasFile()) {
        const Result<MediaHandle> plain = MediaHandle::fromJson(value.media().toJson());
        return plain ? Value(plain.value()) : value;
    }
    if (value.isList()) {
        QList<Value> items;
        for (const Value &item : value.items())
            items.append(fileOnly(item));
        return Value::list(items);
    }
    return value;
}

void dropImages(Values &values) {
    for (auto it = values.begin(); it != values.end(); ++it)
        it.value() = fileOnly(it.value());
}

QJsonObject valuesJson(const Values &values) {
    QJsonObject json;
    for (auto it = values.begin(); it != values.end(); ++it)
        if (!it.value().isEmpty())
            json.insert(it.key(), it.value().toJson());
    return json;
}

void collectModels(const Value &value, QStringList &models) {
    if (value.isList()) {
        for (const Value &item : value.items())
            collectModels(item, models);
    } else if (value.kind() == Value::Kind::Json && value.json().isObject() &&
               value.json().toObject().contains("resource")) {
        models.append(resourceIdentity(value.json().toObject()));
    }
}

class Scheduler : public QObject {
public:
    Scheduler(const CompiledGraph &graph, std::optional<RunFolder> folder, const RunEnvironment &environment,
              const RunOptions &options, bool dry)
        : m_graph(graph), m_folder(std::move(folder)), m_env(environment), m_options(options), m_dry(dry),
          m_cancel(std::make_shared<std::atomic<bool>>(false)) {
        m_cpuSlots = m_env.cpuSlots > 0 ? m_env.cpuSlots : qMax(1, QThread::idealThreadCount() - 1);
        // Every lane can be full at once: the CPU slots, the GPU lane, and
        // one job's translation per provider instance.
        m_pool.setMaxThreadCount(m_cpuSlots + 1 + int(m_env.providers.size()));
        m_busy.fill(false, m_env.providers.size());
    }

    ~Scheduler() override {
        m_cancel->store(true);
        if (!m_finished)
            cancelJobs();
        m_pool.waitForDone();
    }

    std::function<void(const QJsonObject &)> onEvent;
    std::function<void()> onFinished;

    bool finished() const { return m_finished; }
    const RunOutcome &outcome() const { return m_outcome; }

    void start() {
        if (m_startedRun)
            return;
        m_startedRun = true;
        m_startTime = now();
        m_order = m_graph.order();
        bool needsProviders = false;
        for (const QString &path : std::as_const(m_order)) {
            NodeRun n;
            n.path = path;
            n.node = &*m_graph.nodes.constFind(path);
            n.dependencies = m_graph.dependencies(path);
            n.type = m_env.registry ? m_env.registry->nodeType(n.node->type) : nullptr;
            if (n.type && m_env.kernels) {
                n.kernel = m_env.kernels->node(n.type->id);
                n.providerKernel = m_env.kernels->provider(n.type->id);
            }
            needsProviders = needsProviders || n.providerKernel;
            m_nodes.insert(path, n);
        }
        for (const NodeRun &n : std::as_const(m_nodes))
            for (const QString &dep : n.dependencies)
                ++m_nodes[dep].readers;

        if (!m_dry) {
            m_folder->writeCompiled(m_graph.toJson());
            writeState(QStringLiteral("running"));
            emitEvent({{"event", "run.started"}});
            for (const QString &path : std::as_const(m_order))
                emitEvent({{"event", "node.queued"}, {"node", path}});
            // What the compiler left out because of a mute or an empty
            // upstream is told as skipped; nodes that are simply not part of
            // any output are not part of the run.
            for (auto it = m_graph.skipped.begin(); it != m_graph.skipped.end(); ++it)
                if (it.value() != QLatin1String("not_part_of_output"))
                    emitEvent({{"event", "node.skipped"}, {"node", it.key()}, {"reason", it.value()}});
        }

        for (const ProviderEntry &entry : std::as_const(m_env.providers)) {
            Candidate candidate;
            candidate.instance = entry.instance;
            if (!entry.provider)
                candidate.instance.enabled = false;
            m_candidates.append(candidate);
        }
        if (needsProviders)
            describeProviders();
        if (m_describing == 0)
            later([this] { advance(); });
    }

    void cancel() {
        if (m_finished || m_cancel->load())
            return;
        m_cancel->store(true);
        cancelJobs();
        later([this] { advance(); });
    }

private:
    // --- small helpers ---

    QDateTime now() const { return m_env.clock ? m_env.clock().toUTC() : QDateTime::currentDateTimeUtc(); }

    void later(std::function<void()> call) { QMetaObject::invokeMethod(this, std::move(call), Qt::QueuedConnection); }

    void emitEvent(const QJsonObject &event) {
        if (m_dry)
            return;
        m_folder->appendEvent(event);
        if (onEvent)
            onEvent(event);
    }

    UsedBy usedBy(const NodeRun &n) const { return UsedBy{m_options.workflow, n.path}; }

    QString runId() const { return m_folder ? m_folder->id() : QString(); }

    void writeState(const QString &status) {
        if (m_dry)
            return;
        QJsonObject state{{"status", status},
                          {"pid", QCoreApplication::applicationPid()},
                          {"started", m_startTime.toString(Qt::ISODateWithMs)},
                          {"workflow", QJsonObject{{"path", m_options.workflow}, {"hash", m_graph.source}}}};
        if (m_finished || status != QLatin1String("running"))
            state.insert("ended", now().toString(Qt::ISODateWithMs));
        QJsonObject autos;
        QJsonObject providers;
        QJsonObject nodes;
        for (const QString &path : std::as_const(m_order)) {
            const NodeRun &n = m_nodes[path];
            if (!n.autoValues.isEmpty())
                autos.insert(path, n.autoValues);
            if (n.selection)
                providers.insert(path, QJsonObject{{"instance", n.selection->instance},
                                                   {"engine", n.selection->engine},
                                                   {"model", n.selection->model.handle}});
            QJsonObject node;
            const NodeState shown = n.phase == Phase::Running ? NodeState::Running : n.state;
            node.insert("state", nodeStateName(shown));
            if (!n.key.isEmpty())
                node.insert("key", n.key);
            if (!n.outputs.isEmpty())
                node.insert("outputs", valuesJson(n.outputs));
            if (!n.reason.isEmpty())
                node.insert("reason", n.reason);
            if (n.state == NodeState::Error)
                node.insert("error", n.error.toJson());
            nodes.insert(path, node);
        }
        state.insert("auto", autos);
        state.insert("providers", providers);
        state.insert("nodes", nodes);
        QJsonArray errors;
        for (const Error &error : std::as_const(m_outcome.errors))
            errors.append(error.toJson());
        state.insert("errors", errors);
        m_folder->writeState(state);
    }

    // --- providers ---

    void describeProviders() {
        for (int i = 0; i < m_env.providers.size(); ++i) {
            const ProviderEntry &entry = m_env.providers.at(i);
            if (!entry.provider || !entry.instance.enabled)
                continue;
            connect(entry.provider.get(), &Provider::statusChanged, this,
                    [this, i](const QString &job, const JobStatus &status) { jobStatus(i, job, status); });
            ++m_describing;
            const QPointer<Scheduler> self(this);
            entry.provider->describe([self, i](const Result<Description> &description) {
                if (!self)
                    return;
                self->m_candidates[i].description = description;
                if (--self->m_describing == 0)
                    self->advance();
            });
        }
    }

    void cancelJobs() {
        for (NodeRun &n : m_nodes)
            if (n.phase == Phase::Running && !n.job.isEmpty() && !n.jobOver)
                m_env.providers.at(n.instance).provider->cancel(n.job, [](const Result<QString> &) {});
    }

    // --- the loop ---

    void advance() {
        if (m_finished || m_describing > 0)
            return;
        // A node that fails while it is being started asks for another pass
        // from inside this one.
        if (m_advancing) {
            m_again = true;
            return;
        }
        m_advancing = true;
        do {
            m_again = false;
            settle();
            startReady();
        } while (m_again);
        m_advancing = false;
        releaseImages();
        for (const NodeRun &n : std::as_const(m_nodes)) {
            if (n.phase != Phase::Finished) {
                writeState(QStringLiteral("running"));
                return;
            }
        }
        finish();
    }

    // Once every node that reads from a node has finished, its decoded
    // images are let go; the files stay.
    void releaseImages() {
        for (NodeRun &n : m_nodes) {
            if (n.phase != Phase::Finished || n.settled)
                continue;
            n.settled = true;
            if (n.readers == 0)
                dropImages(n.outputs);
            for (const QString &dep : std::as_const(n.dependencies))
                if (--m_nodes[dep].readers == 0 && m_nodes[dep].settled)
                    dropImages(m_nodes[dep].outputs);
        }
    }

    // Looks at every node whose dependencies have finished, in dependency
    // order, so a chain of cache hits settles in one pass.
    void settle() {
        for (const QString &path : std::as_const(m_order)) {
            NodeRun &n = m_nodes[path];
            if (n.phase != Phase::Waiting && n.phase != Phase::Ready)
                continue;
            if (m_cancel->load()) {
                skip(n, QStringLiteral("cancelled"));
                continue;
            }
            if (n.phase == Phase::Ready)
                continue;
            bool depsDone = true;
            for (const QString &dep : std::as_const(n.dependencies))
                depsDone = depsDone && m_nodes[dep].phase == Phase::Finished;
            if (depsDone)
                prepare(n);
        }
    }

    void prepare(NodeRun &n) {
        for (const QString &dep : std::as_const(n.dependencies)) {
            const NodeRun &d = m_nodes[dep];
            if (d.state == NodeState::Error ||
                (d.state == NodeState::Skipped && d.reason == QLatin1String("upstream_failed")))
                return skip(n, QStringLiteral("upstream_failed"));
            if (d.state == NodeState::Skipped && d.reason == QLatin1String("cancelled"))
                return skip(n, QStringLiteral("cancelled"));
        }
        if (!n.type)
            return fail(n, nodeError(QStringLiteral("unknown_node_type"),
                                     QStringLiteral("Node type %1 is not installed.").arg(n.node->type)));

        // Freeze (10.4): the frozen key's result, whatever the inputs are now.
        if (!n.node->frozen.isEmpty()) {
            const std::optional<Values> frozen = m_dry ? m_env.cache->peek(n.node->frozen, *m_env.hashes)
                                                       : m_env.cache->lookup(n.node->frozen, usedBy(n), *m_env.hashes);
            if (frozen) {
                n.key = n.node->frozen;
                return hit(n, *frozen, NodeState::Frozen);
            }
            Error warning = nodeError(QStringLiteral("frozen_result_missing"),
                                      QStringLiteral("The frozen result of node %1 is not in the cache.").arg(n.path),
                                      QStringLiteral("The node runs normally. Freeze it again to keep the new result."));
            warning.severity = Error::Severity::Warning;
            warning.node = n.path;
            m_outcome.errors.append(warning);
        }

        bool unknown = false; // a value that only a run can settle
        QString skipReason;
        KeyParts parts;
        Error problem;
        if (!resolveInputs(n, parts.inputs, unknown, skipReason, problem))
            return fail(n, problem);
        if (!skipReason.isEmpty())
            return skip(n, skipReason);

        // The provider checks come before anything that depends on every input being
        // known, so that validate reports them (8.5) for a node with a randomized seed
        // or an input linked to a node that has not run.
        if (n.providerKernel) {
            Requirement requirement = n.providerKernel->requirement(*n.type, n.inputs);
            requirement.nodePin = n.node->provider;
            requirement.workflowPin.clear();
            const Result<Selection> selection = selectProvider(m_candidates, requirement);
            if (!selection) {
                if (!m_dry)
                    return fail(n, selection.error());
                // Under validate these are warnings (8.5); a run reports them
                // as errors when it reaches the node.
                for (Error warning : selection.errors()) {
                    warning.severity = Error::Severity::Warning;
                    warning.node = n.path;
                    m_outcome.errors.append(warning);
                }
                return stale(n);
            }
            n.selection = selection.value();
            for (int i = 0; i < m_env.providers.size(); ++i)
                if (m_env.providers.at(i).instance.name == n.selection->instance)
                    n.instance = i;
            if (n.instance < 0)
                return fail(n, nodeError(QStringLiteral("internal"),
                                         QStringLiteral("Provider %1 was selected but is not configured.")
                                             .arg(n.selection->instance)));
            parts.engine = n.selection->engine;
            parts.model = resourceIdentity(n.selection->model.handle);
            n.models = {n.selection->instance + QLatin1Char('\n') + parts.model};
        } else if (n.kernel) {
            for (const Value &value : std::as_const(n.inputs))
                collectModels(value, n.models);
        }
        if (unknown)
            return stale(n);
        if (!n.providerKernel && !n.kernel && !m_dry) {
            return fail(n, nodeError(QStringLiteral("internal"),
                                     QStringLiteral("No kernel is registered for node type %1.").arg(n.type->id)));
        }

        parts.type = n.type->id;
        parts.version = n.type->version;
        parts.pack = typeNamespace(n.type->id);
        if (!n.type->deterministic) {
            if (m_dry)
                return stale(n);
            parts.run = runId();
        }
        n.key = cacheKey(parts);

        const std::optional<Values> cached =
            m_dry ? m_env.cache->peek(n.key, *m_env.hashes) : m_env.cache->lookup(n.key, usedBy(n), *m_env.hashes);
        if (cached)
            return hit(n, *cached, NodeState::Cached);
        if (m_dry)
            return stale(n);
        n.phase = Phase::Ready;
    }

    static bool hasLink(const InputValue &written) {
        if (written.kind() == InputValue::Kind::Link)
            return true;
        if (written.kind() == InputValue::Kind::List)
            for (const InputValue &item : written.items())
                if (hasLink(item))
                    return true;
        return false;
    }

    // One input value as written in the graph, as a run-time value.
    Value resolveValue(NodeRun &n, const InputSpec &spec, const InputValue &written, bool &unknown) {
        switch (written.kind()) {
        case InputValue::Kind::Literal:
            return Value(written.literal());
        case InputValue::Kind::Resource:
            return Value(written.handle());
        case InputValue::Kind::Auto: {
            if (m_dry) {
                unknown = true;
                return {};
            }
            const qint64 min = spec.min ? qint64(*spec.min) : 0;
            const qint64 max = spec.max ? qint64(*spec.max) : maxSeed;
            const qint64 seed = m_env.randomSeed ? m_env.randomSeed(min, max)
                                                 : (max > min ? QRandomGenerator::global()->bounded(min, max + 1) : min);
            n.autoValues.insert(spec.name, seed);
            return Value(seed);
        }
        case InputValue::Kind::Link: {
            const PortRef from = written.from();
            const NodeRun &source = m_nodes[from.node];
            if (source.state == NodeState::Stale)
                unknown = true;
            return source.outputs.value(from.port);
        }
        case InputValue::Kind::List: {
            QList<Value> items;
            for (const InputValue &item : written.items()) {
                const Value value = resolveValue(n, spec, item, unknown);
                if (value.isList())
                    items.append(value.items());
                else if (!value.isEmpty())
                    items.append(value);
            }
            return Value::list(items);
        }
        }
        return {};
    }

    // Fills n.inputs and the key's `inputs`. Returns false with `problem`
    // set for an error; sets `skipReason` when the node is to be skipped.
    bool resolveInputs(NodeRun &n, QJsonObject &keyInputs, bool &unknown, QString &skipReason, Error &problem) {
        n.inputs.clear();
        for (const InputSpec &spec : n.type->inputs) {
            const auto written = n.node->inputs.constFind(spec.name);
            Value value;
            bool linked = false;
            bool thisUnknown = false;
            if (written != n.node->inputs.constEnd() && !written->isEmpty()) {
                linked = hasLink(*written);
                value = resolveValue(n, spec, *written, thisUnknown);
                if (spec.type.list && !value.isList() && !value.isEmpty())
                    value = Value::list({value});
            }
            if (thisUnknown) {
                // Left out; the inputs that are known still reach the provider checks.
                unknown = true;
                continue;
            }
            const bool empty = value.isEmpty() || (value.isList() && value.items().isEmpty() && linked);
            if (empty && !spec.defaultValue.isUndefined()) {
                value = Value(spec.defaultValue);
            } else if (empty && spec.required()) {
                if (linked) {
                    // Linked to an output that came back empty (3.3).
                    skipReason = QStringLiteral("upstream_empty");
                    return true;
                }
                problem = nodeError(QStringLiteral("missing_input"),
                                    QStringLiteral("Input %1 has no value.").arg(spec.name));
                problem.input = spec.name;
                return false;
            } else if (empty) {
                continue;
            }
            n.inputs.insert(spec.name, value);
            // A file read by path is keyed by its content, not its path (10.1).
            if (spec.widget() == QLatin1String("file") && value.kind() == Value::Kind::Json && value.json().isString()) {
                const QString hash = m_env.hashes->hash(value.text());
                if (!hash.isEmpty()) {
                    keyInputs.insert(spec.name, QJsonObject{{"file", hash}});
                    continue;
                }
            }
            keyInputs.insert(spec.name, value.identity());
        }
        return true;
    }

    // --- starting ---

    bool prefersResident(const NodeRun &n) const {
        if (!m_env.models)
            return false;
        for (const QString &model : n.models)
            if (m_env.models->contains(model))
                return true;
        return false;
    }

    bool usesGpuLane(const NodeRun &n) const {
        if (n.providerKernel)
            return isLocal(m_env.providers.at(n.instance).instance);
        const QJsonValue gpu = n.type->needs.value("gpu");
        return gpu.isObject() && !gpu.toObject().value("parallel").toBool();
    }

    void startReady() {
        QList<NodeRun *> ready;
        for (const QString &path : std::as_const(m_order))
            if (m_nodes[path].phase == Phase::Ready)
                ready.append(&m_nodes[path]);
        // Residency (9.3): a node that reuses what is loaded goes first.
        std::stable_sort(ready.begin(), ready.end(), [this](const NodeRun *a, const NodeRun *b) {
            return prefersResident(*a) && !prefersResident(*b);
        });
        for (NodeRun *n : std::as_const(ready)) {
            if (n->phase != Phase::Ready)
                continue;
            const bool gpu = usesGpuLane(*n);
            if (gpu && m_gpuBusy)
                continue;
            if (n->providerKernel) {
                if (m_busy.at(n->instance))
                    continue;
                m_busy[n->instance] = true;
                n->holdsInstance = true;
            } else if (!gpu) {
                if (m_cpuRunning >= m_cpuSlots)
                    continue;
                ++m_cpuRunning;
                n->holdsCpu = true;
            }
            if (gpu) {
                m_gpuBusy = true;
                n->holdsGpu = true;
                makeRoom(*n);
            }
            startNode(*n);
        }
    }

    // 8.10: before GPU work starts, free memory is measured against what the
    // node declares. While it is short, the least recently used resident
    // model the node does not need is dropped (9.3). The node starts either
    // way: estimates are declared, not trusted, and a real shortage comes
    // back as out_of_memory.
    void makeRoom(const NodeRun &n) {
        if (!m_env.freeGpuMemoryMb || !m_env.models || n.providerKernel)
            return;
        const qint64 needed = qint64(n.type->needs.value("gpu").toObject().value("vram_mb").toDouble());
        while (needed > 0 && m_env.freeGpuMemoryMb() < needed)
            if (m_env.models->evictLeastRecentlyUsed(n.models).isEmpty())
                break;
    }

    NodeContext contextFor(const NodeRun &n) {
        NodeContext context;
        context.node = n.path;
        context.run = runId();
        context.type = n.type;
        context.inputs = n.inputs;
        context.workDir = n.workDir;
        context.hashes = m_env.hashes;
        context.models = m_env.models;
        const QString path = n.path;
        context.progress = [this, path](double value, const QString &stage, const QString &message) {
            later([this, path, value, stage, message] { progress(path, value, stage, message); });
        };
        context.cancelled = [flag = m_cancel] { return flag->load(); };
        return context;
    }

    void startNode(NodeRun &n) {
        n.phase = Phase::Running;
        n.started = now();
        n.job.clear();
        n.jobOver = false;
        emitEvent({{"event", "node.running"}, {"node", n.path}});
        const Result<QString> dir = m_folder->nodeDir(n.path);
        if (!dir)
            return failed(n, dir.error());
        n.workDir = dir.value();

        const NodeContext context = contextFor(n);
        const QString path = n.path;
        FileHashes *hashes = m_env.hashes;
        if (n.providerKernel) {
            const auto kernel = n.providerKernel;
            const Selection selection = *n.selection;
            m_pool.start([this, kernel, context, selection, path] {
                const Result<JobRequest> request =
                    guarded<JobRequest>([&] { return kernel->request(context, selection); });
                later([this, path, request] { submit(path, request); });
            });
            return;
        }
        const auto kernel = n.kernel;
        m_pool.start([this, kernel, context, path, hashes] {
            Result<Values> result = guarded<Values>([&] { return kernel->run(context); });
            if (result)
                result = guarded<Values>([&] { return withFiles(result.value(), context.workDir, *hashes); });
            later([this, path, result] { returned(path, result); });
        });
    }

    void progress(const QString &path, double value, const QString &stage, const QString &message) {
        if (m_nodes[path].phase != Phase::Running)
            return;
        emitEvent({{"event", "node.progress"},
                   {"node", path},
                   {"value", qBound(0.0, value, 1.0)},
                   {"stage", stage},
                   {"message", message}});
    }

    // --- provider jobs ---

    void submit(const QString &path, const Result<JobRequest> &made) {
        NodeRun &n = m_nodes[path];
        if (!made)
            return failed(n, made.error());
        if (m_cancel->load())
            return failed(n, nodeError(QStringLiteral("cancelled"), QStringLiteral("The run was cancelled.")));
        JobRequest request = made.value();
        request.outputDir = n.workDir;
        const QPointer<Scheduler> self(this);
        m_env.providers.at(n.instance).provider->submit(request, [self, path](const Result<QString> &job) {
            if (self)
                self->submitted(path, job);
        });
    }

    void submitted(const QString &path, const Result<QString> &job) {
        NodeRun &n = m_nodes[path];
        if (!job)
            return failed(n, job.error());
        n.job = job.value();
        const QString id = jobId(n.instance, n.job);
        m_jobs.insert(id, path);
        if (m_cancel->load())
            m_env.providers.at(n.instance).provider->cancel(n.job, [](const Result<QString> &) {});
        // A status that came before the job id did.
        if (m_early.contains(id))
            jobStatus(n.instance, n.job, m_early.take(id));
    }

    static QString jobId(int instance, const QString &job) { return QString::number(instance) + QLatin1Char('/') + job; }

    void jobStatus(int instance, const QString &job, const JobStatus &status) {
        const QString id = jobId(instance, job);
        if (!m_jobs.contains(id)) {
            m_early.insert(id, status);
            return;
        }
        const QString path = m_jobs.value(id);
        NodeRun &n = m_nodes[path];
        if (n.phase != Phase::Running || n.job != job || n.jobOver)
            return;
        if (!status.isTerminal()) {
            if (status.state == QLatin1String("running"))
                progress(path, status.progress, status.stage, QString());
            if (!status.preview.isEmpty()) {
                const QString file = n.workDir + QStringLiteral("/preview.png");
                QFile preview(file);
                if (preview.open(QIODevice::WriteOnly) && preview.write(status.preview) == status.preview.size())
                    emitEvent({{"event", "node.preview"},
                               {"node", path},
                               {"image", QJsonObject{{"path", file}, {"format", "png"}}}});
            }
            return;
        }
        n.jobOver = true;
        m_jobs.remove(id);
        if (status.state == QLatin1String("error")) {
            Error error = status.error;
            if (error.code.isEmpty())
                error = nodeError(QStringLiteral("node_failed"), QStringLiteral("The provider's job failed."));
            return failed(n, error);
        }
        if (status.state == QLatin1String("cancelled"))
            return failed(n, nodeError(QStringLiteral("cancelled"), QStringLiteral("The provider's job was cancelled.")));
        const QPointer<Scheduler> self(this);
        m_env.providers.at(instance).provider->result(job, [self, path](const Result<JobResult> &result) {
            if (self)
                self->jobResult(path, result);
        });
    }

    void jobResult(const QString &path, const Result<JobResult> &result) {
        NodeRun &n = m_nodes[path];
        if (!result)
            return failed(n, result.error());
        const auto kernel = n.providerKernel;
        const NodeContext context = contextFor(n);
        const Selection selection = *n.selection;
        const JobResult job = result.value();
        FileHashes *hashes = m_env.hashes;
        m_pool.start([this, kernel, context, selection, job, path, hashes] {
            Result<Values> outputs = guarded<Values>([&] { return kernel->outputs(context, selection, job); });
            if (outputs)
                outputs = guarded<Values>([&] { return withFiles(outputs.value(), context.workDir, *hashes); });
            later([this, path, outputs] { returned(path, outputs); });
        });
    }

    // --- finishing a node ---

    void release(NodeRun &n) {
        if (n.holdsCpu)
            --m_cpuRunning;
        if (n.holdsGpu)
            m_gpuBusy = false;
        if (n.holdsInstance)
            m_busy[n.instance] = false;
        n.holdsCpu = n.holdsGpu = n.holdsInstance = false;
    }

    // A kernel or a provider phase came back.
    void returned(const QString &path, const Result<Values> &result) {
        NodeRun &n = m_nodes[path];
        if (!result)
            return failed(n, result.error());

        Values outputs = result.value();
        // A randomized input is also a "value used" output (10.1): when the
        // node type has an output of the same name, it carries the value.
        for (auto it = n.autoValues.begin(); it != n.autoValues.end(); ++it)
            if (n.type->output(it.key()) && outputs.value(it.key()).isEmpty())
                outputs.insert(it.key(), Value(it.value()));
        Error bad;
        if (!checkOutputs(n, outputs, bad))
            return failed(n, bad);

        const Result<Values> stored = m_env.cache->store(n.key, n.type->id, outputs, n.workDir, usedBy(n), *m_env.hashes);
        if (!stored)
            return failed(n, stored.error());

        if (m_env.models) {
            // A provider's server now holds this model; a kernel's own
            // models were just used.
            for (const QString &model : std::as_const(n.models)) {
                if (n.providerKernel)
                    m_env.models->put(model);
                else
                    m_env.models->touch(model);
            }
        }
        release(n);
        n.phase = Phase::Finished;
        n.state = NodeState::Cached;
        n.outputs = stored.value();
        m_outcome.ran.append(n.path);
        const double seconds = n.started.msecsTo(now()) / 1000.0;
        emitEvent({{"event", "node.done"}, {"node", n.path}, {"outputs", valuesJson(n.outputs)}, {"seconds", seconds}});
        advance();
    }

    bool checkOutputs(const NodeRun &n, Values &outputs, Error &bad) const {
        auto wrong = [&](const QString &output, const QString &message) {
            bad = nodeError(QStringLiteral("node_bad_output"), message);
            bad.output = output;
            return false;
        };
        for (auto it = outputs.begin(); it != outputs.end(); ++it)
            if (!n.type->output(it.key()))
                return wrong(it.key(), QStringLiteral("The node returned an output it does not declare: %1.").arg(it.key()));
        for (const OutputSpec &spec : n.type->outputs) {
            Value value = outputs.value(spec.name);
            if (value.isEmpty()) {
                if (!spec.optional)
                    return wrong(spec.name, QStringLiteral("The node returned nothing for output %1.").arg(spec.name));
                outputs.remove(spec.name);
                continue;
            }
            if (spec.type.list && !value.isList()) {
                value = Value::list({value});
                outputs.insert(spec.name, value);
            }
            const QList<Value> items = spec.type.list ? value.items() : QList<Value>{value};
            for (const Value &item : items)
                if (!fits(item, spec.type.type))
                    return wrong(spec.name, QStringLiteral("Output %1 is not %2.").arg(spec.name, spec.type.toString()));
        }
        return true;
    }

    // A running node came back with an error.
    void failed(NodeRun &n, const Error &error) {
        const bool lane = n.holdsGpu || n.holdsInstance;
        release(n);
        if (m_cancel->load()) {
            skip(n, QStringLiteral("cancelled"));
        } else if (error.code == QLatin1String("out_of_memory") && lane && m_env.models &&
                   !m_env.models->evictLeastRecentlyUsed(n.models).isEmpty()) {
            // 9.3: drop the least recently used model, not everything, and
            // try again.
            n.phase = Phase::Ready;
        } else {
            fail(n, error);
        }
        advance();
    }

    void fail(NodeRun &n, Error error) {
        if (error.code.isEmpty())
            error.code = QStringLiteral("node_failed");
        error.node = n.path;
        n.phase = Phase::Finished;
        n.state = NodeState::Error;
        n.error = error;
        m_outcome.errors.append(error);
        emitEvent({{"event", "node.error"}, {"node", n.path}, {"error", error.toJson()}});
    }

    void skip(NodeRun &n, const QString &reason) {
        n.phase = Phase::Finished;
        n.state = NodeState::Skipped;
        n.reason = reason;
        emitEvent({{"event", "node.skipped"}, {"node", n.path}, {"reason", reason}});
    }

    void stale(NodeRun &n) {
        n.phase = Phase::Finished;
        n.state = NodeState::Stale;
    }

    void hit(NodeRun &n, const Values &outputs, NodeState state) {
        n.phase = Phase::Finished;
        n.state = state;
        n.outputs = outputs;
        emitEvent({{"event", "node.cached"}, {"node", n.path}, {"outputs", valuesJson(outputs)}});
    }

    void finish() {
        bool anyError = false;
        bool anyCancelled = false;
        for (const QString &path : std::as_const(m_order)) {
            const NodeRun &n = m_nodes[path];
            m_outcome.states.insert(path, n.state);
            if (!n.key.isEmpty())
                m_outcome.keys.insert(path, n.key);
            if (!n.outputs.isEmpty())
                m_outcome.outputs.insert(path, n.outputs);
            anyError = anyError || n.state == NodeState::Error;
            anyCancelled = anyCancelled || (n.state == NodeState::Skipped && n.reason == QLatin1String("cancelled"));
        }
        m_outcome.status = anyCancelled ? QStringLiteral("cancelled")
                           : anyError   ? QStringLiteral("failed")
                                        : QStringLiteral("ok");
        m_finished = true;
        if (!m_dry) {
            writeState(m_outcome.status);
            QJsonArray errors;
            for (const Error &error : std::as_const(m_outcome.errors))
                errors.append(error.toJson());
            emitEvent({{"event", "run.done"}, {"status", m_outcome.status}, {"errors", errors}});
            m_env.hashes->save();
            m_env.cache->evict();
        }
        if (onFinished)
            onFinished();
    }

    const CompiledGraph m_graph;
    std::optional<RunFolder> m_folder;
    const RunEnvironment m_env;
    const RunOptions m_options;
    const bool m_dry;

    std::shared_ptr<std::atomic<bool>> m_cancel;
    QThreadPool m_pool;
    QStringList m_order;
    QMap<QString, NodeRun> m_nodes;
    QList<Candidate> m_candidates;
    int m_describing = 0;
    QHash<QString, QString> m_jobs;    // "<instance>/<job>" -> node
    QHash<QString, JobStatus> m_early; // statuses of jobs not yet known by id

    int m_cpuSlots = 1;
    int m_cpuRunning = 0;
    bool m_gpuBusy = false;
    QList<bool> m_busy; // per provider instance

    bool m_advancing = false;
    bool m_again = false;
    bool m_startedRun = false;
    bool m_finished = false;
    QDateTime m_startTime;
    RunOutcome m_outcome;
};

} // namespace

class Run::Private {
public:
    std::unique_ptr<Scheduler> scheduler;
    QString id;
};

Run::Run(const CompiledGraph &graph, const RunFolder &folder, const RunEnvironment &environment,
         const RunOptions &options, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>()) {
    qRegisterMetaType<fugue::RunOutcome>();
    d->id = folder.id();
    d->scheduler = std::make_unique<Scheduler>(graph, folder, environment, options, false);
    d->scheduler->onEvent = [this](const QJsonObject &event) { emit emitted(event); };
    d->scheduler->onFinished = [this] { emit finished(d->scheduler->outcome()); };
}

Run::~Run() = default;

QString Run::id() const { return d->id; }

bool Run::isFinished() const { return d->scheduler->finished(); }

RunOutcome Run::outcome() const { return d->scheduler->outcome(); }

void Run::start() { d->scheduler->start(); }

void Run::cancel() { d->scheduler->cancel(); }

RunOutcome runGraph(const CompiledGraph &graph, const RunFolder &folder, const RunEnvironment &environment,
                    const RunOptions &options, const std::atomic<bool> *cancel) {
    Run run(graph, folder, environment, options);
    QEventLoop loop;
    QObject::connect(&run, &Run::finished, &loop, &QEventLoop::quit);
    QTimer poll;
    if (cancel) {
        poll.setInterval(100);
        QObject::connect(&poll, &QTimer::timeout, &run, [&run, cancel] {
            if (cancel->load())
                run.cancel();
        });
        poll.start();
    }
    run.start();
    if (cancel && cancel->load())
        run.cancel();
    if (!run.isFinished())
        loop.exec();
    return run.outcome();
}

GraphCheck checkGraph(const CompiledGraph &graph, const RunEnvironment &environment, const RunOptions &options) {
    Scheduler scheduler(graph, std::nullopt, environment, options, true);
    QEventLoop loop;
    scheduler.onFinished = [&loop] { loop.quit(); };
    scheduler.start();
    if (!scheduler.finished())
        loop.exec();
    GraphCheck check;
    check.states = scheduler.outcome().states;
    check.keys = scheduler.outcome().keys;
    check.warnings = scheduler.outcome().errors;
    return check;
}

} // namespace fugue
