#pragma once

// What a process needs to run graphs, built once from the user's files: the result
// cache, the file-hash memo and the provider instances (spec 8.2). The worker and
// `fugue validate` use the same one.

#include <memory>

#include "cli/commands.h"
#include "engine/file_hashes.h"
#include "engine/result_cache.h"
#include "engine/scheduler.h"

namespace fugue::cli {

struct Process {
    explicit Process(const Context &context);

    ResultCache cache;
    FileHashes hashes;
    RunEnvironment environment;
    Errors errors; // providers.json problems; the environment then has no providers
};

// Garbage collection of 10.5 (the result store, and run folders 7 days after they
// ended), at most once an hour: a stamp file in the state folder remembers when.
void collectGarbageIfDue(const Context &context, Process &process);

} // namespace fugue::cli
