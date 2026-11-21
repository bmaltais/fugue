#pragma once

// Which packs the user has allowed (spec 6.7), kept in `trust.json` in the state folder, per
// pack id and version: a pack update asks again. The `fugue` and `local` packs are always
// trusted. Opening a workflow asks about the packs of its nodes before anything of theirs can
// run; a group is only a graph, so the packs of the nodes inside it count, not the group's id.
//
//   {"packs": {"io.github.alice.tools": ["1.2.0"]}}

#include "engine/document.h"
#include "engine/registry.h"
#include "engine/result.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace fugue {

// A pack a workflow uses.
struct PackUse {
    QString id;
    QString version; // the installed version; for a pack not installed, the workflow's own, maybe empty
    QString source;  // where `requires.packs` says to get it, may be empty
    QStringList types; // the node types of this pack the workflow uses, sorted
};

class TrustStore {
public:
    // ~/.local/state/fugue/trust.json
    static QString defaultPath();
    // The pack of a node type id: what precedes the last "/" ("io.github.alice.tools/sharpen").
    static QString packOf(const QString &typeId);
    static bool isBuiltIn(const QString &packId);

    explicit TrustStore(QString path = defaultPath());

    bool isTrusted(const QString &packId, const QString &version) const;
    Errors trust(const QString &packId, const QString &version);
    // Drops every version of a pack (it was removed). No effect for a pack that was never trusted.
    Errors forget(const QString &packId);
    // Drops one version of a pack: another major of it stays installed and trusted (6.6).
    Errors forget(const QString &packId, const QString &version);

    // Every pack the document's nodes (groups' inner nodes included) come from, built-ins left out.
    // The version of a pack is the INSTALLED one (Registry::packVersion), as the compiler asks
    // about it; the workflow's `requires.packs` only gives the source, and the version of a
    // pack that is not installed.
    static QList<PackUse> packsUsed(const Document &document, const Registry &registry);
    // Those of them not trusted.
    QList<PackUse> untrusted(const Document &document, const Registry &registry) const;

private:
    QString m_path;
};

} // namespace fugue
