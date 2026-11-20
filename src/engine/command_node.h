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
// Not yet (7.2, 7.3): `preview` lines are ignored, resource handles are passed on without a
// resolved `path`, and a persistent process is not stopped when memory is needed.

#include "engine/node_kernel.h"

#include <memory>

namespace fugue {

class CommandKernel : public NodeKernel {
public:
    // `graceMs` is the time a program gets to stop after a cancel, a timeout or a shutdown
    // message before it is killed (7.4: 5 seconds).
    explicit CommandKernel(int graceMs = 5000);
    ~CommandKernel() override;

    Result<Values> run(const NodeContext &context) override;

    // Stops every persistent process now.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace fugue
