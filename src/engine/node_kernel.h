#pragma once

// Node kernels: the seam between the scheduler and the code that does a
// node's work. A kernel is registered for a node type id (Kernels); the
// scheduler resolves a node's inputs, looks its result up in the cache, and
// only on a miss asks the kernel to run.
//
// There are two kinds, and a type id has exactly one:
//
//   - NodeKernel: work done in this process (built-in C++ nodes in M1; the
//     command-node runner in M3 is one kernel serving many types). run() is
//     called on a worker thread and blocks until the work is done.
//   - ProviderKernel: work done by a provider (8). The kernel only translates:
//     the node's inputs into a JobRequest, and the JobResult into outputs. The
//     scheduler owns the job: selection (8.3), one job at a time per instance
//     (9.2), progress and preview events, cancel (9.6).
//
// What a kernel can rely on:
//   - `inputs` holds every input of the node type that has a value: literals,
//     defaults filled in, links replaced by the upstream value, `auto`
//     resolved (a randomized seed is a concrete number). An optional input
//     with no value is absent. A list input is always a List value.
//   - `workDir` exists and belongs to this node in this run
//     (runs/<run>/nodes/<path id>/). Write output files there.
//   - Input files are read-only: they are shared with the result cache.
//
// What a kernel returns: a value for every output of the node type that is
// not `optional` (anything else is node_bad_output). An image may be returned
// in memory (MediaHandle::fromImage); the scheduler writes it to
// <workDir>/<output>.png. A media or data file inside workDir is moved into
// the cache's store once the node finishes (9.7); a file anywhere else is
// left where it is. A data handle needs "data" and "path"; the scheduler adds
// its "hash".
//
// Errors: return an Error with a code of 13.3 (node_failed when none fits,
// out_of_memory when memory ran out so the scheduler can free a resident
// model and try again). `node` is filled in by the scheduler. An exception
// thrown out of a kernel is caught and reported as node_failed.
//
// Kernels must be thread-safe: one kernel object may run several nodes at
// once, so per-run state belongs in locals, not members.

#include <QJsonObject>
#include <QString>

#include <functional>
#include <map>
#include <memory>

#include "engine/node_type.h"
#include "engine/provider.h"
#include "engine/providers.h"
#include "engine/result.h"
#include "engine/value.h"

namespace fugue {

class FileHashes;
class Registry;
class ResidentModels;

struct NodeContext {
    QString node;                   // path id
    QString run;                    // run id
    const NodeType *type = nullptr; // the installed node type; outlives the run
    Values inputs;
    QString workDir;

    // Reports progress: value 0 to 1, a stage name, a message (9.4). Safe to
    // call from the kernel's thread, as often as wanted.
    std::function<void(double value, const QString &stage, const QString &message)> progress;
    // True once the run is cancelled. Poll it at every checkpoint and return
    // early (any error will do: a cancelled node is reported as skipped).
    std::function<bool()> cancelled;

    // The process's file-hash memo, for MediaHandle::probe and saved().
    FileHashes *hashes = nullptr;
    // The resident-model cache (9.3), or null. A kernel that loads a model
    // keeps it here under resourceIdentity(handle) so the next node reuses it.
    ResidentModels *models = nullptr;

    // The registry the run was compiled against: node types, for a kernel that reads the
    // document (share_copy.h).
    const Registry *registry = nullptr;
    // The workflow being run, in Document::toJson() form, exactly as the run was
    // started (after `--set` overrides), or empty when the run did not start from a
    // document (a compiled graph). For a node that writes the workflow into its
    // output (Save Image, 11.7); never an input to anything, so it is not in the cache key.
    QJsonObject document;
    // What was used to make this node's inputs, from what the scheduler records in
    // run.json: {"<path id>": {"auto": {<input>: <value>}, "engine": <id>,
    // "model": <handle>}} for this node and every node upstream of it that has any
    // (a randomized input made concrete; a provider node's engine and model).
    // Same purpose as `document`, and likewise not in the cache key.
    QJsonObject used;

    // The input's value, or an Empty value when it has none.
    Value input(const QString &name) const { return inputs.value(name); }
};

class NodeKernel {
public:
    virtual ~NodeKernel();
    virtual Result<Values> run(const NodeContext &context) = 0;
};

class ProviderKernel {
public:
    virtual ~ProviderKernel();

    // What the node asks a provider for: the capability, the properties the
    // model must have, and the model input's handle (empty for "any ready
    // model", 8.4). Pins are the scheduler's; leave them empty. Called on the
    // scheduler's thread before the cache lookup, so it must be quick.
    // Under validate (checkGraph) nothing has run, so `inputs` may lack the inputs
    // that are not known yet: a random seed, or a link to a node that has not run.
    // Do not assume that every required input is present.
    virtual Requirement requirement(const NodeType &type, const Values &inputs) const = 0;

    // The job to submit to the provider chosen. `outputDir` is set by the
    // scheduler (the node's workDir). Called on a worker thread, so it may
    // write files (MediaHandle::plainFile for a video input).
    virtual Result<JobRequest> request(const NodeContext &context, const Selection &selection) = 0;

    // The node's outputs from a finished job: file references become media
    // handles (MediaHandle::probe), and the values used (seed, model, engine)
    // become ordinary outputs (3.3). Called on a worker thread.
    virtual Result<Values> outputs(const NodeContext &context, const Selection &selection,
                                   const JobResult &result) = 0;
};

// The kernels of a process, by node type id.
class Kernels {
public:
    void add(const QString &typeId, std::shared_ptr<NodeKernel> kernel);
    void add(const QString &typeId, std::shared_ptr<ProviderKernel> kernel);

    std::shared_ptr<NodeKernel> node(const QString &typeId) const;
    std::shared_ptr<ProviderKernel> provider(const QString &typeId) const;

private:
    std::map<QString, std::shared_ptr<NodeKernel>> m_nodes;
    std::map<QString, std::shared_ptr<ProviderKernel>> m_providers;
};

} // namespace fugue
