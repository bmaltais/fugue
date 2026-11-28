#pragma once

// The models a resource input can be given (spec 2.5): those the provider instances offer (8.2,
// 8.4) and the model files on disk (8.9). The canvas's model picker and `fugue model list` (13.1)
// both list them from here.

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace fugue {

class FileHashes;
class ModelLibrary;
struct PortType;

// One model as a picker or the CLI shows it.
struct ListedModel {
    QString name;       // the handle's name
    QJsonObject handle; // what is stored when it is picked (2.5)
    QString status;     // "ready", "loading" or "unavailable" (a file on disk is "ready")
    QString provider;   // the instance's name; empty for a file on disk
    // The paired host that serves it (#164): the instance's name when the instance has an
    // address, else empty (a file on disk, or a server on this machine).
    QString host;
    // The instance's name when it runs off this machine (fugue::placementOf is OffMachine: a cloud
    // service such as fal.ai), else empty. Such a model is never "this machine" (#164).
    QString cloud;
    QString title;      // the name people know it by; empty: the name
    QString note;       // for a model that is not ready: why, and how to make it ready
    // A file on disk: where it is, the type folder it was found in, and its size. Its handle
    // has a `hash` only when the file's hash is remembered (FileHashes): hashing a large model
    // takes from seconds to minutes, so listing never does it.
    QString path;
    QString folder;
    qint64 size = 0;

    // A file on a paired helper's model list (#735, HostModelCache): `host` and `provider` name the instance, `size` is its size, the
    // handle has the hash when the host knows it, `family` is the table's name for it (else the engine's).
    bool remote = false;
    QString family;

    bool onDisk() const { return !path.isEmpty(); }
    // Served by a paired host that does not answer for it (not ready): its models are listed
    // dimmed, "host offline", never hidden (#164).
    bool hostOffline() const { return !host.isEmpty() && status == QLatin1String("unavailable"); }
};

// Asks each enabled provider instance (providers.json, else spec 8.2's default) to describe
// itself and lists every model it offers of the resource type `type` (every type when it is
// empty) for one of `capabilities` (any capability when it is empty), by instance in the user's
// order. An instance whose describe() fails lists nothing. `done` is called once, from the event
// loop, after the last reply: the providers stay alive until then.
void listOfferedModels(const QString &type, const QStringList &capabilities,
                       std::function<void(QList<ListedModel>)> done);

class HostModelCache;
struct ProviderInstance;

// The models the paired helpers' kept lists (HostModelCache, no network) offer for `type`: each file under one of the type's folders
// (`library.folders(type)` names), as a remote row marked `remote`, in the user's order of hosts then by name. A host whose helper has
// no list operation, or never answered, lists nothing.
QList<ListedModel> listHostModels(const QList<ProviderInstance> &instances, const HostModelCache &cache, const ModelLibrary &library,
                                  const PortType &type);

// The model files of `type` (ModelLibrary::list), each with the handle that names it:
// {"resource", "name"}, and "hash" when `hashes` remembers the file's. Reads no model file.
QList<ListedModel> listDiskModels(const ModelLibrary &library, const PortType &type, FileHashes &hashes);

// A file on disk's handle: {"resource", "name", "hash"}. With `hash` empty there is no "hash",
// and the handle does not name the file yet (2.5: a handle without a hash names a provider's
// model); listDiskModels gives that for a file not hashed yet.
QJsonObject diskHandle(const QString &resource, const QString &name, const QString &hash);

} // namespace fugue
