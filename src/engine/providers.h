#pragma once

// Provider instances (spec 8.2) and choosing one to serve a capability (8.3 to 8.5).
//
// Selection is a pure function over descriptions already fetched: the scheduler calls
// describe() on each instance, then selectProvider() for each provider node. When nothing can
// serve the node the error says what is missing and how to fix it (8.5).

#include <QJsonObject>
#include <QList>
#include <QString>

#include <memory>

#include "engine/provider.h"

namespace fugue {

struct OmarchyPaths; // omarchy_provider.h

// One entry of ~/.config/fugue/providers.json.
struct ProviderInstance {
    QString name;         // unique; what pins name
    QString type;         // the provider module id, e.g. omarchy
    QString address;      // empty means the module's default (for omarchy: this machine)
    QString key;          // lives only in providers.json, never in a workflow or export
    bool enabled = true;  // absent means enabled
    QJsonObject settings; // every other field, as the module declares them
};

// The standard path, $XDG_CONFIG_HOME/fugue/providers.json (~/.config/fugue/providers.json).
QString providersPath();

// The list in the user's order. Errors are invalid_document, one per bad entry.
Result<QList<ProviderInstance>> parseProviderInstances(const QJsonObject &document);

// Reads and parses a providers.json. A missing file gives defaultProviderInstances(), read
// from Omarchy's state in `omarchy` (OmarchyPaths::standard() when not given). A file that
// is there wins entirely: nothing is added to it. Nothing here writes the file.
Result<QList<ProviderInstance>> loadProviderInstances(const QString &path);
Result<QList<ProviderInstance>> loadProviderInstances(const QString &path, const OmarchyPaths &omarchy);

// What a machine with no providers.json gets (spec 8.2): "This machine" (type omarchy, no
// address), then one omarchy instance per host omarchy-local-ai's network.json says this
// machine is paired with, named after the host, at https://<its dns name> (what the omarchy
// provider matches a paired host's models by), in the hosts' name order. A paired host that
// is offline is kept, so its models show as offline and a pin on it stays valid. A
// network.json that is missing or cannot be read adds no host.
QList<ProviderInstance> defaultProviderInstances();
QList<ProviderInstance> defaultProviderInstances(const OmarchyPaths &omarchy);

// Makes the provider an instance is a use of. M0 compiles in only omarchy (8.7); any other type
// is unknown_provider until provider modules arrive (M4).
Result<std::shared_ptr<Provider>> createProvider(const ProviderInstance &instance);

// An instance with the outcome of its describe(). A disabled instance need not be described.
struct Candidate {
    ProviderInstance instance;
    Result<Description> description{Description{}};
};

// What a provider node needs.
struct Requirement {
    QString capability;
    // Each must hold for the model: equal values, or for an array every item present.
    QJsonObject properties;
    // The node's model input; empty means any ready model (8.4).
    QJsonObject model;
    // Pins are names only. The node's own pin wins over the workflow's (graph.provider);
    // `fugue run --provider` replaces the workflow's pin for that run.
    QString nodePin;
    QString workflowPin;
};

struct Selection {
    QString instance;   // the instance name, which is not part of the cache key (8.3)
    QString engine;     // the engine id, which is (10.1)
    OfferedModel model; // the model resolved, with its limits and defaults
};

// Chooses per 8.3: a pin when there is one, else the first enabled ready instance in the
// user's order. Errors: unknown_provider, capability_unavailable, model_not_running,
// model_missing, or a pinned instance's own describe error (provider_unreachable).
Result<Selection> selectProvider(const QList<Candidate> &candidates, const Requirement &requirement);

// Whether an offered handle is the requested one: by hash when the request has one, else by
// name (a model a provider serves by name has no hash, 2.5).
bool sameModel(const QJsonObject &requested, const QJsonObject &offered);

} // namespace fugue
