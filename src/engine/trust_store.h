#pragma once

// Which packs the user has allowed (spec 6.7), kept in `trust.json` in the state folder, per
// pack id and version: a pack update asks again. The `fugue` and `local` packs are always
// trusted. Opening a workflow asks about the packs of its nodes before anything of theirs can
// run; a group is only a graph, so the packs of the nodes inside it count, not the group's id.
//
//   {"packs": {"io.github.alice.tools": ["1.2.0"]}}
//
// A project that inherits `packs` (spec 1.4.2) also honours the Default project's trust.json, read-only, for
// the packs it inherits: the exact id and version of an inherited pack the Default trusts is trusted here too.
// It never applies to a pack of the project's own, whatever the Default says of that id and version, and
// nothing here ever writes the Default's file. Each change takes `trust.json.lock` beside the file, so two processes
// (Settings, the window, the CLI) never lose each other's change. The layer applies to the session's own trust file
// (defaultPath()); a TrustStore on another path (a test's) has none.

#include "engine/document.h"
#include "engine/registry.h"
#include "engine/result.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace fugue {

// One version's entry in the trust file, as forget() found it: listed, and the secrets digest recorded with the yes.
struct TrustEntry {
    bool listed = false;
    QString digest;
};

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

    // Trusted in this session's own file, or, for an inherited pack, in the Default's.
    bool isTrusted(const QString &packId, const QString &version) const;
    // This session's own file lists the version, whatever its secrets are now: what a person's yes recorded (not the Default's).
    bool isListed(const QString &packId, const QString &version) const;
    // The secrets digest recorded with that yes; empty when none was.
    QString sealedDigest(const QString &packId, const QString &version) const;
    // Trusted only because the Default trusts that inherited pack: the file here does not say so.
    bool isTrustedByDefault(const QString &packId, const QString &version) const;
    // The Default project's trust.json (read-only here).
    static QString defaultProjectPath();
    // `secretsDigest`: packSecretsDigest of the pack being trusted, when the caller has just read its folder; else the one
    // the loader parsed (a pack not loaded has none, and then none is kept).
    Errors trust(const QString &packId, const QString &version, const QString &secretsDigest = {});
    // Puts back a yes that was taken away for a moment (an update that failed): the version is listed again with exactly
    // `digest` (empty: none), never one computed now, so an edit of the secrets made since the yes still asks. Nothing when
    // the version is listed again by then: a yes given meanwhile is newer and stays as it is.
    Errors restore(const QString &packId, const QString &version, const QString &digest);
    // Trust on first sight: a version trusted before digests were kept gets `digest` now (nothing for a version that is not
    // trusted here or already has one), so a later edit of its secrets asks again.
    Errors adoptSecretsDigest(const QString &packId, const QString &version, const QString &digest);
    // Drops every version of a pack (it was removed). No effect for a pack that was never trusted. Only this
    // session's own file is changed.
    Errors forget(const QString &packId);
    // Drops one version of a pack: another major of it stays installed and trusted (6.6). `taken`, when given, says what was
    // dropped, read under the same lock as the change (unlisted when nothing was), for a restore() of exactly that.
    Errors forget(const QString &packId, const QString &version, TrustEntry *taken = nullptr);
    // A person taking trust back: forget(), except that trust an inherited pack has only from the Default's
    // file is refused (usage, naming the Default project as the place to change it). An empty `version` means
    // every version.
    Errors revoke(const QString &packId, const QString &version = {});

    // Every pack the document's nodes (groups' inner nodes included) come from, built-ins left out.
    // The version of a pack is the INSTALLED one (Registry::packVersion), as the compiler asks
    // about it; the workflow's `requires.packs` only gives the source, and the version of a
    // pack that is not installed.
    static QList<PackUse> packsUsed(const Document &document, const Registry &registry);
    // Those of them not trusted.
    QList<PackUse> untrusted(const Document &document, const Registry &registry) const;

private:
    bool layered() const;
    QString m_path;
};

} // namespace fugue
