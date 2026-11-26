#pragma once

// Where a workflow whose model files are not on this machine runs: the paired helper's verdict (#395, #491, #581), told before
// anything runs, one answer for the CLI (`app info`, `app run`) and the window (the Runs on line, the Needs panel). The scheduler sends a
// node that takes its model files in the job to a paired helper when its files are not here (8.7); this reads the document the way the
// scheduler chooses, per node, and says for each node that needs a missing model: which host takes it (RunsOn), why it cannot go
// anywhere (Blocked), or that the only host that could does not answer (Silent).
//
// Order: the pins decide first; the hosts are asked (the probe) only when they leave a node that could go somewhere, so an app that
// pins already block never waits for a 12 s probe. What a host last said only refuses a node for a host that does not answer. The probe is the seam: the live one calls
// probePairedHelpers, a test gives a fake.

#include <QByteArray>
#include <QMap>
#include <QStringList>

#include <chrono>
#include <functional>
#include <optional>

#include "engine/document.h"
#include "engine/error.h"
#include "engine/providers.h"
#include "engine/readiness.h"
#include "engine/registry.h"

namespace fugue {

// Only model files are missing: no pack, and the problems are no more than models that have no usable source (`model_missing`,
// `download_failed`), which the host may have all the same (#395). Where this is false there is nothing to place.
bool onlyModelsMissing(const ReadinessPlan &plan);

struct PairedPlacementRequest {
    // The configured provider instances (providers.json): the paired helpers in the user's order, and the pins to read.
    QList<ProviderInstance> instances;
    // The capability a node type asks a provider for, or empty for a type that is no provider node. Without it nothing is known about
    // the nodes and nothing is placed or refused.
    std::function<QString(const QString &typeId)> capabilityOf;
    // To read each node's own inputs: a model reaches a node only through its Resource ports, as the scheduler decides per node (8.7).
    const Registry *registry = nullptr;
    // What a host said the last time it answered (rememberedPairedHelper when empty): it refuses a node only for a host that does not answer now (a live answer overrules it), and
    // is never what runs one.
    std::function<std::optional<Description>(const ProviderInstance &)> remembered;
};

struct PairedVerdict {
    // True when the question was asked: only models are missing and a paired helper is enabled. False: nothing to place, and the
    // other members are empty.
    bool applies = false;
    // RunsOn: for each node that needs a missing model, the first helper in the user's order that answered live, offers the node's
    // capability and respects its pin (the scheduler's own choice).
    QMap<QString, QString> hostOf;
    // Blocked: `model_missing` per node refused by a pin or by what a host offers (live, else what it last said), with the reason.
    Errors blocked;
    // Silent: the hosts that did not answer where one of them was the only way for a node. Never named as a host.
    QStringList silent;

    // The distinct hosts of hostOf, sorted by name.
    QStringList hosts() const;
    // The app runs on `hosts()`: every node that needs a model has a host, and nothing is blocked or silent.
    bool runsOn() const { return !hostOf.isEmpty() && blocked.isEmpty() && silent.isEmpty(); }
    // A run may start: the question applies and no node is blocked. A silent host does not stop it (a cached re-run can still work;
    // else the run fails with `provider_unreachable`).
    bool allowsRun() const { return applies && blocked.isEmpty(); }
};

// Asks every given helper to describe itself and gives the descriptions of those that answered, by instance name (10 s at most each).
// `done` is called once, from the event loop, or at once for a fake.
using PairedProbe = std::function<void(const QList<ProviderInstance> &helpers,
                                       std::function<void(const QMap<QString, Description> &answered)> done)>;

// The probe that asks the hosts (probePairedHelpers; each answer is remembered as it comes).
PairedProbe livePairedProbe();
// `inner` with its answers kept for `ttl`: a question whose helpers all answered within it is answered from the keep, and a question
// asked while the same one is out waits for it. A helper that did not answer is not kept (the next question asks it again).
PairedProbe keptPairedProbe(PairedProbe inner, std::chrono::milliseconds ttl);

// The verdict for `document` as `plan` says it is missing models. `done` is called once, at once when the pins and what the hosts last
// said decide (no probe), else when the probe answers.
void decidePairedPlacement(const Document &document, const ReadinessPlan &plan, const PairedPlacementRequest &request,
                           const PairedProbe &probe, std::function<void(const PairedVerdict &verdict)> done);

// What the verdict depends on, as bytes: the gate's inputs (packs, problem codes), the missing models' hashes, the graph's nodes (type,
// pin, inputs) and the workflow's pin, and the provider instances. Equal keys give the same verdict; the plan's free disk space, the
// layout and the like are not in it, so a download or a run writing files does not ask again.
QByteArray pairedPlacementKey(const Document &document, const ReadinessPlan &plan, const QList<ProviderInstance> &instances);

// What the Needs panel lists for `plan` under `verdict`: every model row marked as the hosts' to have when the app runs on them (#493),
// else the nodes no host can take, each with why, added as problems. A silent host changes nothing here: the rows stay. Only the paired
// verdict counts, never a service off this machine (#404) the view may be naming.
ReadinessPlan planUnder(ReadinessPlan plan, const PairedVerdict &verdict);

} // namespace fugue
