#pragma once

// The window follows reloads of its node types (#70, spec 6.5). main() and the test windows wire
// it the same way, after the objects are made:
//   - what changes the packs asks for a reload: the Packs page after an install, update, remove
//     or trust (at once), a group saved to the library (at once), the settings (pack_paths: after
//     the reloader's settle time); the reloader's watch covers changes made outside the window;
//   - once it has reloaded, everything that shows node types refreshes: the canvas and node
//     search, the workflows' banner and trust question, the runs (the run thread reloads too) and
//     the Packs page.

#include "engine/registry_reloader.h"
#include "qml_app.h"

namespace fugue {

void followNodeTypeReloads(RegistryReloader &reloader, const AppObjects &objects);

} // namespace fugue
