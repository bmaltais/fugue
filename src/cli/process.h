#pragma once

// What a process needs to run graphs, built once from the user's files: the provider
// instances (spec 8.2), which go into the registry, and the result cache and the
// file-hash memo. The worker and `fugue validate` use the same ones.

#include "cli/commands.h"
#include "engine/file_hashes.h"
#include "engine/gpu_memory.h"
#include "engine/result_cache.h"
#include "engine/scheduler.h"

namespace fugue::cli {

// Puts the configured provider instances (providers.json, or the default "This machine")
// into the registry (6.12), each with the provider made for it. Returns:
//   - errors when providers.json cannot be used; the registry's providers are then left
//     as they were, and the caller must not go on as if there were none;
//   - a warning for each instance whose provider cannot be made (an unknown type). The
//     instance is still registered, without a provider, so a pin to it resolves.
Errors registerProviders(Registry &registry);

// Needs the registry's providers set (registerProviders).
struct Process {
    explicit Process(const Context &context);
    // The same with the result store and the file-hash memo somewhere else (the window's
    // tests keep them in a temporary folder).
    Process(const Context &context, const QString &cacheRoot, const QString &hashesPath);

    ResultCache cache;
    FileHashes hashes;
    GpuMemoryProbe gpu; // free GPU memory (8.10) for environment.freeGpuMemoryMb
    RunEnvironment environment;
};

// Garbage collection of 10.5 (the result store, and run folders 7 days after they
// ended), at most once an hour: a stamp file in the state folder remembers when.
void collectGarbageIfDue(const Context &context, Process &process);

} // namespace fugue::cli
