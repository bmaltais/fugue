#pragma once

// The scheduler (spec 9): runs a compiled graph to the end.
//
// The one entry point for a worker process is runGraph(): given a compiled
// graph, a run folder and a RunEnvironment (registry, kernels, result cache,
// providers), it runs every node, writes run.json, compiled.json and
// events.jsonl into the run folder, and returns the outcome. Run is the same
// thing without blocking, for a caller that has its own event loop (the
// canvas, RunQueue). checkGraph() is the part `fugue validate` needs: the
// state of every node and the warnings a run would give, with nothing run.
//
// What a run does:
//   - Ready queue in dependency order (9.2). A node is looked at once all
//     the nodes it reads from have finished.
//   - Before a node runs, its inputs are resolved (defaults, upstream values,
//     a randomized seed made concrete), a provider node's provider and model
//     are selected (8.3), and its cache key is made (10.1). A hit is reported
//     as node.cached and the kernel is not called.
//   - Lanes: nodes with no `needs.gpu` run in parallel on up to cpuSlots
//     threads. GPU nodes, and jobs on a provider instance on this machine,
//     share one lane and run one at a time; `needs.gpu.parallel` opts a GPU
//     node out of the lane. Each provider instance runs one job at a time; a
//     remote instance does not take the GPU lane.
//   - Residency (9.3): among ready nodes of the GPU lane and of one provider
//     instance, one whose model is already resident goes first. When a node
//     of those lanes fails with out_of_memory, the least recently used
//     resident model it does not need is dropped and the node runs again; it
//     fails only when nothing is left to drop. Before a GPU node starts,
//     free GPU memory (RunEnvironment::freeGpuMemoryMb, 8.10) is compared
//     with its `needs.gpu.vram_mb` and models are dropped the same way.
//   - Errors are per node (9.5): the node gets node.error, every node
//     downstream is skipped with `upstream_failed`, other branches carry on.
//   - A node whose required input is linked to an output that came back
//     empty is skipped with `upstream_empty` (3.3).
//   - Cancel (9.6): nodes not yet started are skipped with `cancelled`;
//     running kernels see NodeContext::cancelled(); provider jobs are
//     cancelled and waited for. Nodes that finished stay cached.
//   - When a node finishes, its outputs go into the result cache: files move
//     into the store, a hard link stays in the node's folder, and the handles
//     passed on carry the store's path (9.7).
//   - A frozen node (10.4) takes the result of its frozen key whatever its
//     inputs are. If that result is gone, the run warns
//     (frozen_result_missing) and the node runs normally.
//
// Events (9.4) are appended to the run folder as they happen and emitted by
// Run::emitted. run.queued belongs to whoever queues the run: RunQueue writes
// it, and a worker waiting in the cross-process queue (9.8) writes its own.
// A run writes run.started, the node events and run.done. Warnings travel in
// run.done's `errors` with severity warning.
//
// run.json while and after a run:
//   {"status": "running" | "ok" | "failed" | "cancelled", "pid", "started",
//    "ended", "workflow": {"path", "hash"},
//    "auto": {<node>: {<input>: <value resolved>}},
//    "providers": {<node>: {"instance", "engine", "model": <handle>}},
//    "nodes": {<node>: {"state", "key", "outputs", "reason"?, "error"?}},
//    "errors": [...]}
//
// Threads: everything here must be called from one thread that runs a Qt
// event loop, because providers answer through it. Kernels run on the run's
// own thread pool.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

#include "engine/compiler.h"
#include "engine/error.h"
#include "engine/node_kernel.h"
#include "engine/providers.h"
#include "engine/registry.h"
#include "engine/resident_models.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"

namespace fugue {

class FileHashes;

// A configured provider instance and the provider made for it
// (loadProviderInstances + createProvider).
struct ProviderEntry {
    ProviderInstance instance;
    std::shared_ptr<Provider> provider; // may be null for a disabled instance
};

// What a process needs to run graphs. Everything pointed at must outlive the
// runs that use it. One environment serves every run of a process.
struct RunEnvironment {
    const Registry *registry = nullptr;
    const Kernels *kernels = nullptr;
    ResultCache *cache = nullptr;
    FileHashes *hashes = nullptr;
    // In the user's order (8.3). May be empty when no node needs a provider.
    QList<ProviderEntry> providers;
    // Optional. Shared by the runs of a process so a model stays loaded
    // between them. Without it there is no residency preference.
    ResidentModels *models = nullptr;

    // Free GPU memory in MB, measured as 8.10 says (by Vulkan device type);
    // unset means it is not measured. Before a GPU node that declares
    // `needs.gpu.vram_mb` starts, resident models are dropped, least recently
    // used first, while this is short of it.
    std::function<qint64()> freeGpuMemoryMb;

    // How many CPU nodes run at once; 0 means the number of cores minus one,
    // at least one (9.2).
    int cpuSlots = 0;
    // Time for `seconds` in node.done and the times in run.json; the system
    // clock when unset. (Event times are RunFolder's.)
    std::function<QDateTime()> clock;
    // A randomized seed (`{"auto": "random"}`) within min..max, both
    // included; a random generator when unset.
    std::function<qint64(qint64 min, qint64 max)> randomSeed;
};

struct RunOptions {
    // The workflow file the graph came from, absolute, or empty. It goes
    // into run.json and into the cache's `used_by`.
    QString workflow;
};

// The state of a node (10.3).
enum class NodeState {
    Cached,  // its result is in the cache (after a run: it ran, or was a hit)
    Stale,   // it would run
    Running, // only while a run is going
    Frozen,  // it takes its frozen result
    Error,
    Skipped,
};
QString nodeStateName(NodeState state); // "cached", "stale", ...

struct RunOutcome {
    QString status; // ok, failed or cancelled
    // Every node error, each naming its node by path id, plus warnings.
    Errors errors;
    QMap<QString, NodeState> states;  // by path id
    QMap<QString, QString> keys;      // the cache key of each node that got one
    QMap<QString, Values> outputs;    // what each finished node produced
    QStringList ran;                  // the nodes whose kernel ran, in finishing order
};

// One run. start() returns at once; finished() comes later from the event
// loop, after run.done has been written. Destroying a Run that has not
// finished cancels it and waits for its running kernels to return.
class Run : public QObject {
    Q_OBJECT

public:
    Run(const CompiledGraph &graph, const RunFolder &folder, const RunEnvironment &environment,
        const RunOptions &options = {}, QObject *parent = nullptr);
    ~Run() override;

    QString id() const;
    bool isFinished() const;
    RunOutcome outcome() const; // complete once finished

public slots:
    void start();
    // Cooperative (9.6). finished() still comes, with status cancelled
    // unless every node had finished already.
    void cancel();

signals:
    // Every event as it is appended to events.jsonl, without `run` and `time`.
    void emitted(const QJsonObject &event);
    void finished(const fugue::RunOutcome &outcome);

private:
    class Private;
    std::unique_ptr<Private> d;
};

// Runs the graph to the end and returns its outcome: the entry point for a
// run worker (`fugue run`). It blocks, running an event loop of its own, so
// the thread needs a QCoreApplication but no running loop. `cancel`, when
// given, is polled about ten times a second: setting it cancels the run, so
// a signal handler can ask for a cancel by storing true.
//
// The caller makes the run folder (RunFolder::create) and compiles the graph;
// runGraph writes compiled.json, run.json and the events. It does not queue:
// one run at a time across processes is the caller's job (9.8).
RunOutcome runGraph(const CompiledGraph &graph, const RunFolder &folder, const RunEnvironment &environment,
                    const RunOptions &options = {}, const std::atomic<bool> *cancel = nullptr);

// What `fugue validate` shows beyond compile errors (9.1, 10.3).
struct GraphCheck {
    // cached, frozen or stale for every node of the graph.
    QMap<QString, NodeState> states;
    QMap<QString, QString> keys; // of the nodes whose key could be made
    // Warnings only: frozen_result_missing, and what would stop a provider
    // node (model_missing and the other 8.5 codes), which a run that reaches
    // the node reports as errors.
    Errors warnings;
};

// Looks at the graph as a run would, running nothing and changing nothing:
// no kernel is called, no event is written and the cache is only read.
// Providers are asked to describe themselves, so this also blocks on an
// event loop of its own. A node is stale when its result is not cached, when
// anything upstream is stale, or when its key cannot be known yet (a
// randomized seed, a node type that is not deterministic).
GraphCheck checkGraph(const CompiledGraph &graph, const RunEnvironment &environment, const RunOptions &options = {});

} // namespace fugue

Q_DECLARE_METATYPE(fugue::RunOutcome)
