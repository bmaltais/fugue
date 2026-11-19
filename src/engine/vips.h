#pragma once

// libvips set up the way spike 03 decided (spec 9.2): concurrency 1, because
// the engine's executor already runs CPU nodes in parallel, and an operation
// cache of 0, because the engine has its own result cache.
//
// Every engine function that touches libvips calls initVips() itself; call it
// early anyway to learn whether libvips works. A lone CPU node may raise its
// own concurrency later (the scheduler's choice, not measured yet).

namespace fugue {

// Starts libvips once per process; later calls only return the first result.
bool initVips();

} // namespace fugue
