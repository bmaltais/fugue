#pragma once

// The models of a paired helper (spec 8.7, #735): what `POST /fugue/v1/models` on `fugue-helper --serve-http` lists under the
// host's --models roots, kept per host in <stateDir>/host_models/ so the model picker and Get ready can say "on gx10" without
// waiting for the network. The list is a cache: it is refreshed when it is older than a few minutes or on demand, never
// by anything that waits (a failure keeps the last list and says when it was fetched), and a host whose helper is too old to
// have the operation is `supported == false`, its models unknown (not "none").

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

#include "engine/providers.h"

namespace fugue {

struct HostModel {
    QString name;   // relative to the host's model root, "diffusion_models/z-image.gguf"
    qint64 size = 0;
    QString sha256; // lower-case hex; empty until the host has hashed the file
    QString folder; // the first part of `name` ("" for a file straight in a root)
    QString family; // the engine's name for it when the host could tell

    // "sha256:<hex>" as a model handle's `hash` has it; empty while the host has not hashed the file.
    QString handleHash() const { return sha256.isEmpty() ? QString() : QStringLiteral("sha256:") + sha256; }
    // The name as a handle has it: relative to the type's folder, which is the first part.
    QString handleName() const { return folder.isEmpty() ? name : name.mid(folder.size() + 1); }
    QJsonObject toJson() const;
    static HostModel fromJson(const QJsonObject &object);
};

struct HostModelList {
    QString host;               // the instance's name
    bool supported = true;      // false: the helper has no `models` operation (an old one): its models are unknown
    QList<HostModel> models;
    QDateTime fetchedAt;        // when `models` was read from the host; invalid when it never was
    QString error;              // why the last refresh failed ("" if it did not): `models` is then the last good list
    bool truncated = false;     // the host listed only its first entries
    int unhashed = 0;           // models the host has not hashed yet
    int hashing = 0;            // files the host is hashing now

    bool known() const { return fetchedAt.isValid(); }
    // The file of this content ("sha256:<hex>" as a handle has it, or bare hex), or null.
    const HostModel *byHash(const QString &hash) const;
    QJsonObject toJson() const;
    static HostModelList fromJson(const QJsonObject &object);
};

// A paired helper's list from the host's reply (what a test's fake host sends too).
HostModelList parseHostModels(const QString &host, const QJsonObject &reply, const QDateTime &now);

class HostModelCache {
public:
    // The folder the lists are kept in (default <stateDir>/host_models).
    explicit HostModelCache(QString folder = {});
    static HostModelCache &shared();

    using Done = std::function<void(const HostModelList &list)>;
    using Make = std::function<Result<std::shared_ptr<Provider>>(const ProviderInstance &)>;

    // The list kept for the instance (its name and address), empty and unknown when none. Reads one small file.
    HostModelList cached(const ProviderInstance &instance) const;
    // Whether the list should be fetched again: never fetched, or older than `ttl`.
    static bool stale(const HostModelList &list, int ttlSeconds, const QDateTime &now = QDateTime::currentDateTimeUtc());
    // Asks the host (never blocks; `done` is called from the event loop with the list now kept, the old one when the host did
    // not answer, `error` saying why). `hash` and `names` ask it to hash those files in the background. One refresh per host runs at
    // a time: a second call joins it. Only for a paired helper.
    void refresh(const ProviderInstance &instance, bool hash, const QStringList &names, Done done, const Make &make = {});
    // The refresh of every paired helper whose list is stale (a few minutes); `changed` is called for each list that was fetched.
    void refreshStale(const QList<ProviderInstance> &instances, int ttlSeconds, const Done &changed, const Make &make = {});

    // The instances (in the user's order) whose kept list has a file for each of `hashes` ("sha256:<hex>"); none for no hash.
    QStringList hostsHavingAll(const QList<ProviderInstance> &instances, const QStringList &hashes) const;

    // The host (the first of `instances` in order) whose kept list has the file of `handle`'s hash; empty when none, or the handle has no hash.
    QString hostHaving(const QList<ProviderInstance> &instances, const QJsonObject &handle) const;
    // The same as a function over the providers.json of now, for ModelLibrary::setHostHaving and ReadinessEnv; asks nothing when no
    // paired helper is configured (the common case costs one read of providers.json).
    std::function<QString(const QJsonObject &handle)> hostHavingFunction() const;

    static constexpr int kTtlSeconds = 300;

private:
    QString pathOf(const ProviderInstance &instance) const;
    void keep(const ProviderInstance &instance, const HostModelList &list) const;

    QString m_folder;
    QSet<QString> m_busy; // instances (by file) being asked
    QHash<QString, QList<Done>> m_waiting;
};

} // namespace fugue
