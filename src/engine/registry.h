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
// A node type id is registered once per major version (3.6): older majors stay installed
// beside the newest, so a workflow node runs on the major it was made with. Lookups try the
// exact id first, then the aliases; with no major asked for they give the newest.

#include "engine/error.h"
#include "engine/node_type.h"
#include "engine/port_type.h"
#include "engine/providers.h"

#include <QHash>
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
    QStringList folders;            // resources: folder names under each models root (8.9); empty: the id's name
    QString files;                  // resources: what its files are, by their headers (modelKindName, #86)
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

    // Records the folder a type's node.json was read from (NodeType::dir). No effect for an id
    // that is not a type here. `seal` is NodeType::seal. Applies to the newest major.
    void setNodeDir(const QString &typeId, const QString &dir, const QString &seal = {});

    // The versions of each installed pack, which trust is asked for and given at (6.7). Packs
    // install side by side by major (6.6), so a pack id may have several. The workflow's own
    // `requires.packs` is only what its author had, never what runs. A pack not set here has
    // no installed version: "".
    void setPackVersion(const QString &packId, const QString &version);
    // The newest installed version of the pack.
    QString packVersion(const QString &packId) const;
    // Every installed version of the pack, oldest first.
    QStringList packVersions(const QString &packId) const { return m_packVersions.value(packId); }
    bool hasPackVersion(const QString &packId, const QString &version) const {
        return m_packVersions.value(packId).contains(version);
    }
    // Whether a version of the pack with this major is installed.
    bool hasPackMajor(const QString &packId, int major) const;
    // Where an installed pack can be had, for a workflow's `requires.packs` (4.3, #91): the git
    // URL it was added from (`.fugue-source.json`) with no credentials in it, and the commit (else
    // the ref) it was at. Empty for a pack copied in by hand, from a folder or a file:// URL.
    // Per installed version of the pack (6.6); an empty `version` is the newest.
    void setPackSource(const QString &packId, const QString &version, const QString &source, const QString &ref) {
        m_packSources[packId + QLatin1Char('@') + version] = {source, ref};
    }
    QString packSource(const QString &packId, const QString &version = {}) const {
        return m_packSources.value(sourceKey(packId, version)).first;
    }
    QString packRef(const QString &packId, const QString &version = {}) const {
        return m_packSources.value(sourceKey(packId, version)).second;
    }

    // The newest major of the type with this id, else of the type that has it as an alias,
    // else null. The pointer stays valid until the registry's types are replaced.
    const NodeType *nodeType(const QString &id) const;
    // The type at exactly this major (the newest minor of it, 3.6), or null.
    const NodeType *nodeTypeAt(const QString &id, int major) const;
    // What a workflow node recorded at `major` runs on: that major when installed, else the
    // newest (the node then needs review, 3.6). Null when the type is not installed.
    const NodeType *nodeTypeFor(const QString &id, int major) const;
    // The majors installed of the type, oldest first; empty when it is not installed.
    QList<int> majors(const QString &id) const;

    // Every node type at its newest major, sorted by id: what search and the library list.
    QList<const NodeType *> nodeTypes() const;
    // Every node type at every installed major, sorted by id then major.
    QList<const NodeType *> allNodeTypes() const;

    // Builtin, Any, the declared kind, or Unknown.
    PortKind portKind(const PortType &type) const;
    const DeclaredPortType *portType(const PortType &type) const;
    // Every declared port type, sorted by PortType::toString().
    QList<const DeclaredPortType *> portTypes() const;

    // The configured provider instances in the user's order (8.3), replacing
    // the ones set before. Every name must be set and unique; when the result
    // has an error nothing changes.
    Errors setProviders(const QList<ProviderEntry> &providers);
    const QList<ProviderEntry> &providers() const { return m_providers; }
    QStringList providerNames() const;

    // A reload of the packs (6.5): takes the node types, port types, aliases and pack versions
    // of `from`, a registry filled again from disk, and keeps the providers. Every pointer to a
    // type of this registry is invalid after; the registry itself stays where it is.
    void replaceTypes(Registry &&from);
    // Whether `other` has the same node types (with their folders and seals), port types and
    // pack versions: a reload that would change nothing.
    bool sameTypes(const Registry &other) const;

private:
    QList<ProviderEntry> m_providers;
    QString sourceKey(const QString &packId, const QString &version) const {
        return packId + QLatin1Char('@') + (version.isEmpty() ? packVersion(packId) : version);
    }
    QHash<QString, QStringList> m_packVersions; // oldest first
    QHash<QString, std::pair<QString, QString>> m_packSources; // "<id>@<version>" -> source, ref
    std::map<QString, std::map<int, NodeType>> m_nodeTypes; // id -> major -> type
    std::map<QString, QString> m_aliases; // alias -> owning type id
    std::map<QString, DeclaredPortType> m_portTypes; // keyed by PortType::toString()
};

} // namespace fugue
