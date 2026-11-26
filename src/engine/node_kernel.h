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
// Warnings: a kernel may return them beside its outputs (Result(values, warnings)); the
// scheduler gives each the node's path id and puts it among the run's warnings (run.done's
// `errors`, run.json). A cached result does not repeat them.
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
#include <optional>

#include "engine/file_roots.h"
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
    // Shows an intermediate image: `file`, an image the kernel wrote, becomes a node.preview
    // event (9.4) and the node's picture until its result comes. Safe to call from the
    // kernel's thread, as often as wanted: previews that come faster than they are shown are
    // dropped, all but the newest. The file is read after the call returns, so a later preview
    // writes another file, or writes elsewhere and renames over this one.
    std::function<void(const QString &file)> preview;
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
    QJsonObject usedUpstream;
    // The model handle of the nearest node upstream that used a model (a provider's selection),
    // or empty: what a Save node's `{model}` name token is made from.
    QJsonObject upstreamModel;
    // The folders a node that reads or writes files by path may use (13.6), from
    // RunOptions::roots; none means not limited. Load Image and Save Image check them.
    std::optional<FileRoots> roots;

    // The input's value, or an Empty value when it has none.
    Value input(const QString &name) const { return inputs.value(name); }
};

class NodeKernel {
public:
    virtual ~NodeKernel();
    virtual Result<Values> run(const NodeContext &context) = 0;

    // Under validate (the canvas check, `fugue validate`): what a run of these inputs would fail
    // on and can be told cheaply, with no model loaded and no file hashed, as errors the scheduler
    // reports as warnings on the node (8.5). Only `node`, `type`, `inputs`, `hashes` and
    // `registry` are set, and an input not known yet is absent. Called on the scheduler's thread.
    virtual Errors check(const NodeContext &context) const;

    // What else decides the answer that is not an input: files the kernel reads itself (the wildcard
    // files of Dynamic Prompt), as text that changes when they do. It goes into the cache key (10.1);
    // empty (the default) leaves the key as it was. Called on the scheduler's thread.
    virtual QString keyExtra(const Values &inputs) const { Q_UNUSED(inputs); return {}; }

    // The lazy inputs (InputSpec::lazy) the node needs, asked once every input that is not lazy has its value
    // (`ready`: those inputs, by name, as they will reach the kernel; an empty one is absent). What feeds a lazy input
    // that is not returned is not started for this node (9.9); a node that nothing else asks for is skipped as
    // `not_chosen`. The default asks for them all. Not called under validate, where every lazy input is taken as
    // needed. Called on the scheduler's thread, so it must be quick.
    virtual QStringList lazyInputs(const NodeType &type, const Values &ready) const;
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

    // What else decides the job's answer that is not an input or the model: data the kernel reads
    // itself (the prompt template Enhance Prompt asks with, #140), as text that changes when it
    // does. It goes into the cache key (10.1), so a changed template is not served from the old
    // result. Empty (the default) leaves the key as it was. Called on the scheduler's thread.
    virtual QString keyExtra(const Values &inputs) const { Q_UNUSED(inputs); return {}; }

    // The node's outputs from a finished job: file references become media
    // handles (MediaHandle::probe), and the values used (seed, model, engine)
    // become ordinary outputs (3.3). Called on a worker thread.
    virtual Result<Values> outputs(const NodeContext &context, const Selection &selection,
                                   const JobResult &result) = 0;

    // A node that runs several jobs in turn on the same provider, such as a video through an
    // upscaler a chunk of frames at a time (#138) so the whole is never in memory or on disk.
    // After every finished job the scheduler calls next() with its result: a request is the job to
    // run now (its progressFrom and progressSpan place it in the node's bar), nothing means that
    // was the last and outputs() follows with that last result. The kernel keeps no state of its
    // own (one object serves many nodes): what comes between the jobs is in context.workDir. The
    // default is one job. Called on a worker thread.
    virtual Result<std::optional<JobRequest>> next(const NodeContext &context, const Selection &selection,
                                                   const JobResult &result);

    // The node ended with an error or a cancel, after request() was called: remove what the jobs
    // left in context.workDir besides results (the scheduler never removes files). Quick; called on
    // the scheduler's thread.
    virtual void abandon(const NodeContext &context);
};

// The kernels of a process, by node type id (and major, for an older major kept beside the newest).
class Kernels {
public:
    void add(const QString &typeId, std::shared_ptr<NodeKernel> kernel);
    void add(const QString &typeId, std::shared_ptr<ProviderKernel> kernel);

    // The kernel of an older major of a type kept beside the newest (3.6): a node recorded at
    // `major` runs on it, a type at any other major on the one added for the id.
    void addAt(const QString &typeId, int major, std::shared_ptr<NodeKernel> kernel);
    void addAt(const QString &typeId, int major, std::shared_ptr<ProviderKernel> kernel);

    // The kernel that serves every node type whose runtime kind is `command` and that has no
    // kernel of its own (7): one runner for all the packs' programs.
    void setCommandKernel(std::shared_ptr<NodeKernel> kernel) { m_command = std::move(kernel); }

    // The kernel for a node type: its own, else the command runner for a command type.
    std::shared_ptr<NodeKernel> node(const NodeType &type) const;
    // The kernel registered for exactly this id (null for a command type).
    std::shared_ptr<NodeKernel> node(const QString &typeId) const;
    std::shared_ptr<ProviderKernel> provider(const QString &typeId) const;
    // The provider kernel of this major of a type: its own (addAt), else the one of the id.
    std::shared_ptr<ProviderKernel> provider(const NodeType &type) const;

private:
    std::map<QString, std::shared_ptr<NodeKernel>> m_nodes;
    std::map<QString, std::shared_ptr<ProviderKernel>> m_providers;
    std::shared_ptr<NodeKernel> m_command;
};

} // namespace fugue
