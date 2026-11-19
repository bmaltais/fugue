#pragma once

// The kernel's one registry of node types, declared port types and providers
// (spec 6.12). Built-in nodes, test fakes and (from M3) packs on disk all come
// in through add(), and the configured provider instances through
// setProviders(); nothing here reads files (loadProviderInstances and
// createProvider in providers.h are the loader for providers.json).
//
// A provider's description is not kept here: describe() is asynchronous and its
// answer changes while the app runs (models start and stop), so each run asks
// the providers again.
//
// Each node type id is registered once, at one version: the installed one.
// Lookups try the exact id first, then the aliases (3.6).

#include "engine/error.h"
#include "engine/node_type.h"
#include "engine/port_type.h"
#include "engine/providers.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <map>
#include <memory>

namespace fugue {

// A port type a pack declares in its pack.json (2.2, 6.4).
struct DeclaredPortType {
    PortType type;
    PortKind kind = PortKind::Data; // Resource or Data
    QString name;                   // display name
    QStringList extensions;         // resources: file extensions in the models folder
};

// A configured provider instance (8.2) and the provider made for it.
struct ProviderEntry {
    ProviderInstance instance;
    std::shared_ptr<Provider> provider; // null for an instance that is disabled or could not be made
};

class Registry {
public:
    // Registers a declared port type. Refused (with errors) for a built-in
    // name, a kind other than Resource or Data, or a type already registered.
    Errors add(const DeclaredPortType &type);

    // Registers a node type after validating it as a manifest
    // (NodeType::fromJson) and against what is registered: every port type
    // must be known, "any" is only for inputs of fugue/ types, and the id
    // must be new. Register port types first. An alias another type already
    // claims stays with that type and gives a warning. The type is
    // registered when the result has no error (warnings may remain).
    Errors add(const NodeType &type);

    // fromJson() then add(): the path a node.json takes.
    Errors addManifest(const QJsonObject &manifest);

    // The type with this id, else the type that has it as an alias, else null.
    // The pointer stays valid as long as the registry.
    const NodeType *nodeType(const QString &id) const;

    // Every node type, sorted by id.
    QList<const NodeType *> nodeTypes() const;

    // Builtin, Any, the declared kind, or Unknown.
    PortKind portKind(const PortType &type) const;
    const DeclaredPortType *portType(const PortType &type) const;

    // The configured provider instances in the user's order (8.3), replacing
    // the ones set before. Every name must be set and unique; when the result
    // has an error nothing changes.
    Errors setProviders(const QList<ProviderEntry> &providers);
    const QList<ProviderEntry> &providers() const { return m_providers; }
    QStringList providerNames() const;

private:
    QList<ProviderEntry> m_providers;
    std::map<QString, NodeType> m_nodeTypes;
    std::map<QString, QString> m_aliases; // alias -> owning type id
    std::map<QString, DeclaredPortType> m_portTypes; // keyed by PortType::toString()
};

} // namespace fugue
