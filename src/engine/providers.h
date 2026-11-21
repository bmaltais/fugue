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
// network.json that is missing adds no host (none is paired); one that cannot be read adds
// none either, for now: pairedHostsProblem() says so. Last, when a fugue-helper is installed
// (helperProgram), "Fugue helper" (type fugue-helper): after the model servers, so "any ready
// model" stays theirs.
QList<ProviderInstance> defaultProviderInstances();
QList<ProviderInstance> defaultProviderInstances(const OmarchyPaths &omarchy);

// Why the paired hosts of defaultProviderInstances() are unknown right now: network.json is
// there but does not read (omarchy-agent-local may be rewriting it). Empty when it reads or
// is missing. Defaults made meanwhile lack the hosts, so they must not be written to
// providers.json, where a file that is there wins for good.
QString pairedHostsProblem();
QString pairedHostsProblem(const OmarchyPaths &omarchy);

// A `fugue-helper` instance with an address is a helper on a paired host (8.10), reached over
// http: {"name": "gx10", "type": "fugue-helper", "address": "http://host:port", "key": "..."}.
// Without an address it is the helper on this machine.
//
// Makes the provider an instance is a use of: omarchy and fugue-helper are compiled in (8.7);
// any other type is a provider module (8.8) installed in `modulesRoot` (providerModulesDir() when
// not given) whose id it is: a stdio one is a HelperProvider that describes itself, an http one a
// RemoteHelperProvider. No other type is unknown_provider, with every module id as `valid`.
Result<std::shared_ptr<Provider>> createProvider(const ProviderInstance &instance);
Result<std::shared_ptr<Provider>> createProvider(const ProviderInstance &instance, const QString &modulesRoot);

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
    // Whether the provider reads this machine's files (Description::sharesFilesystem): when not,
    // a stage sends model handles, not paths, and leaves the checks of the files to the host.
    bool sharesFilesystem = true;
};

// Chooses per 8.3: a pin when there is one, else the first enabled ready instance in the
// user's order. Errors: unknown_provider, capability_unavailable, model_not_running,
// model_missing, or a pinned instance's own describe error (provider_unreachable).
Result<Selection> selectProvider(const QList<Candidate> &candidates, const Requirement &requirement);

// Whether an offered handle is the requested one: by hash when the request has one, else by
// name (a model a provider serves by name has no hash, 2.5).
bool sameModel(const QJsonObject &requested, const QJsonObject &offered);

} // namespace fugue
