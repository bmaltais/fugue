#pragma once

// The command-node runner (spec 7): the NodeKernel behind every node whose runtime kind is
// `command`. One kernel serves all such types; it reads the node's folder, entry, mode and
// timeouts from the NodeType.
//
//   - One-shot (default): the program is started for each execution, gets one request on
//     stdin (then stdin is closed) and answers with progress lines and one result or error
//     line on stdout. Exit 0 with a result is success.
//   - Persistent: the program is started once per node type and kept, writes `ready`, and then
//     takes one `run` per line (one at a time). It is stopped after `idle_timeout_s` with
//     nothing to do: a `shutdown` message, then SIGTERM, then SIGKILL.
//   - Cancel: one-shot gets SIGTERM, then SIGKILL after the grace period; persistent gets a
//     `cancel` message, and is killed when it does not answer within the grace period.
//   - `runtime.timeout_s` ends a run the same way and reports `node_timeout`.
//
// The program runs in its own process group, so a cancel reaches what a shell script started.
// What a program writes to stderr is its log: the last 20 lines go into a failure's message.
//
// A `preview` line shows its image on the node (NodeContext::preview); a bad one is left out, and
// the first of a run is a warning (node_bad_preview).
//
// A resource input's handle gets the `path` of its model file (7.2), found by hash in the model
// folders (engine/model_library); a model that is not on this machine fails the node with
// model_missing before the program starts.
//
// A persistent program whose type declares `needs.gpu` is a resident model (9.3) while it runs,
// under residentProgramKey(): dropping it when memory is needed (the scheduler, 8.10) stops it
// as the idle stop does (7.3) and returns once it is gone, unless a run holds it.

#include "engine/node_kernel.h"

#include <memory>

namespace fugue {

// A hash of what a command node runs: its node.json, its entry file and the pack.json beside its folder. The loader keeps it in
// NodeType::seal and the runner compares before every run, so a pack that is updated or edited
// on disk after it was loaded (and trusted, 6.7) does not run until it is loaded again. Other
// files of the node are not covered. Empty when a file cannot be read.
QString nodeSeal(const QString &dir, const QString &entry);

// What a node's cache key holds for the secrets its pack reads (#498): a hash of the stored values and the origins
// the runner would hand it now, never a value; empty when the pack declares none or none is set. A changed key or
// origin changes the key, so a deterministic node does not serve a result made with the old one.
QString packSecretsKeyPart(const NodeType &type);

// The resident-model key of a node type's persistent program, "program:<type id>"; empty for a
// type whose program is not one (not persistent, or no `needs.gpu`). A node of the type keeps
// it among its own models, so making room for the node does not stop its program.
QString residentProgramKey(const NodeType &type);
QString residentProgramKey(const QString &typeId);

class CommandKernel : public NodeKernel {
public:
    // `graceMs` is the time a program gets to stop after a cancel, a timeout or a shutdown
    // message before it is killed (7.4: 5 seconds).
    explicit CommandKernel(int graceMs = 5000);
    ~CommandKernel() override;

    Result<Values> run(const NodeContext &context) override;

    // Stops every persistent process now, ending runs in flight (they fail) and refusing new
    // ones. Final: a kernel that was shut down runs nothing.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace fugue
