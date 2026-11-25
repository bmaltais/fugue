#pragma once

// Secrets a pack declares and the store that keeps their values (spec 6.4, 6.7, 7.1, #402).
//
// A pack's pack.json may declare `settings`, each one a secret: an API key a command node needs
// (a webhook, a storage or translation service). The shape is the one of a provider module's
// `settings` (8.8): `{"name": "token_1", "type": "text", "secret": true}`, plus an optional
// `label` and `description` the Packs page shows.
//
// The values live in `pack-secrets.json` in the config folder of the root (the ordinary session,
// or a project's own, 1.4.2), like provider keys in providers.json: mode 0600 from the first byte,
// written atomically, never inside a workflow, a pack folder, a share copy or an export:
//
//   {"packs": {"io.github.alice.tools": {"token_1": "..."}}}
//
// A value reaches a node's program in one place only: the `secrets` object of the request on its
// stdin, at run time, for the pack that declared the name (command_node.h). It is never a node
// input, so it is in no workflow, no cache key and no run record.

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "engine/result.h"

namespace fugue {

struct PackSecretSpec {
    QString name;        // lowercase letters, digits and `_`, starting with a letter: what `{secret:NAME}` names
    QString label;       // for people; defaults to the name
    QString description; // one line about what it is for, may be empty
    // `"origin": "required"` in pack.json: the person must say where the value may be sent (an origin, 6.4.1) when
    // setting it, and a value stored without one is not handed to the node. The default is "optional".
    bool originRequired = false;
};

// A value as the store keeps it: the secret, and the origin (scheme://host[:port]) the person said it is for, or empty.
struct StoredSecret {
    QString value;
    QString origin;
};

// The key a pack's values are kept under: its id and where it was installed from (the `.fugue-source.json` location,
// empty for a pack copied in by hand). A pack that claims the id from another source gets none of them (6.4.1).
QString packSecretKey(const QString &packId, const QString &source);
// `https://api.example.com` or `http://127.0.0.1:8080` as the lower-case `scheme://host[:port]` the store keeps (the default
// port left out), or empty when `text` is not exactly an origin: no user name, path, query or fragment, scheme http or https.
QString normalizeOrigin(const QString &text);
// The error for a value that cannot be stored: empty, more than one line, a NUL, or over 4096 bytes. Empty when fine.
Errors checkSecretValue(const QString &value);

// What a pack.json `settings` array declares. Every problem is one error, so a pack that declares
// a secret badly is disabled with a reason (an entry that is not an object, a name that does not
// fit, a name twice, `secret` missing or not true, a type other than "text").
Result<QList<PackSecretSpec>> parsePackSecrets(const QJsonValue &settings);
// The names of `specs`, in order.
QStringList packSecretNames(const QList<PackSecretSpec> &specs);

// The text a person reads in place of a value: shorter values than this are never masked.
constexpr int kMinMaskedSecretLength = 4;
// `text` with every value of `values` of at least kMinMaskedSecretLength characters replaced by "***", also as it
// appears percent-encoded (either case of the hex digits, and `+` for a space) and escaped in JSON.
QString maskSecrets(QString text, const QStringList &values);

class PackSecretStore {
public:
    // <config folder>/pack-secrets.json
    static QString defaultPath();
    explicit PackSecretStore(QString path = defaultPath());

    // The names that have a value under `key` (packSecretKey), sorted, and the origin each was set for (empty when none).
    // Never the values. A file that cannot be read counts as holding none here; values() and set() report it.
    QStringList namesSet(const QString &key) const;
    QString originOf(const QString &key, const QString &name) const;
    // The names set under another source of the same pack id than `key`'s (a pack that claims an id another
    // source has values for, 6.4.1), sorted.
    QStringList namesSetForOtherSources(const QString &packId, const QString &key) const;
    // The stored values under `key` for these names only (a name that has none is left out), so a pack never gets
    // more than it declared. An error when the file exists and cannot be read.
    Result<QMap<QString, StoredSecret>> values(const QString &key, const QStringList &declared) const;

    // Stores `value` (checkSecretValue) under `name` with the origin it is for (normalizeOrigin; empty for none, refused
    // when `originRequired`); the file is made 0600 from the first byte. The whole read-modify-write is under a lock.
    Errors set(const QString &key, const QString &name, const QString &value, const QString &origin = {},
               bool originRequired = false);
    // Drops one value and its origin; no effect for one that is not set.
    Errors clear(const QString &key, const QString &name);
    // Drops every value of a pack, from every source (it was removed).
    Errors forgetPack(const QString &packId);

    const QString &path() const { return m_path; }

private:
    Result<QJsonObject> read() const;
    Errors write(const QJsonObject &packs);
    QString m_path;
};

} // namespace fugue
