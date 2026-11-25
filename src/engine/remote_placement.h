#pragma once

// Whether a workflow whose model files are not on this machine can succeed on the paired helper (#395, #491), told before
// anything runs. The scheduler sends a node that takes its model files in the job to the helper when its files are not here
// (8.7); this looks at the document and names the nodes that cannot go there, so a run is refused up front instead of failing
// late: a node pinned to a provider on this machine, or a node whose capability the helper does not offer.

#include <QStringList>

#include <functional>
#include <optional>

#include "engine/document.h"
#include "engine/error.h"
#include "engine/providers.h"
#include "engine/readiness.h"

namespace fugue {

struct RemotePlacement {
    // The configured provider instances (providers.json), to read the pins against.
    QList<ProviderInstance> instances;
    // The capability a node type asks a provider for, or empty for a type that is no provider node. Without it nothing is known
    // about the nodes and nothing is refused.
    std::function<QString(const QString &typeId)> capabilityOf;
    // What the paired helper offers (rememberedPairedHelper), when it is known. Unknown: the host answers for itself at the run.
    std::optional<QStringList> hostCapabilities;
};

// One `model_missing` error per node that takes a missing model (a model of `plan`, or one it cannot get) and cannot run on the
// paired helper, with the node's path id, why, and the hint (`prepare`). Empty when every such node can go there, when no model is
// missing, and when no paired helper is configured.
Errors remoteRunProblems(const Document &document, const ReadinessPlan &plan, const RemotePlacement &placement);

} // namespace fugue
