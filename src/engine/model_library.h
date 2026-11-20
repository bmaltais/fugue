#pragma once

// Models on disk (spec 8.9, 2.5): the model folders and the files of each resource type in them,
// and finding the file a resource handle names.
//
// The roots are Fugue's own models folder (~/.local/share/fugue/models) and then each
// `model_paths` entry of settings.json, in order. Under every root a resource type has its
// folders: the names its pack.json declares in `folders`, else the last part of its id
// (`io.github.alice.tools/upscaler` is `upscaler/`). Declaring ComfyUI's folder names
// (`text_encoders`, `clip`) makes a ComfyUI `models/` folder a root as it is. Each folder is
// searched with its subfolders; a folder or file whose name starts with "." is skipped, and a file
// is a model when its extension is one of the type's `extensions` (else ComfyUI's list plus .gguf).
//
// A handle never holds a path: resolve() finds the file by its content hash. It looks at the
// hashes already remembered (engine/file_hashes) and at the files whose name is the handle's
// `name`, hashing those once. It never hashes every file, so a model renamed since its handle was
// made is found only once its hash is remembered.
//
// A library lists each type's folders once and keeps that list: make one for each use (a run of a
// node, a check), not one for the life of the process. Not thread-safe.

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include "engine/port_type.h"
#include "engine/result.h"

namespace fugue {

class FileHashes;
class Registry;
struct DeclaredPortType;
struct Settings;

// One model file.
struct ModelFile {
    QString path; // absolute
    QString name;   // relative to its type folder ("flux1/model.safetensors"), for display and the handle
    QString folder; // the type folder it was found in (one of folders()), absolute
    qint64 size = 0;
};

class ModelLibrary {
public:
    // How far resolve() may go to tell a handle's file.
    enum class Hashing {
        Read,           // hash a file with the handle's name that has no remembered hash (a run)
        RememberedOnly, // read no file: such a file is taken to be the one (a check, which must be quick)
    };

    // `roots` are absolute folders, searched in order; `registry` gives each resource type's
    // folders and extensions and must outlive the library.
    ModelLibrary(QStringList roots, const Registry *registry);

    // The library of the settings in effect now (currentSettings()).
    static ModelLibrary current(const Registry *registry);

    // dataDir()/models, then each model path of `settings` with "~" expanded; a relative path is
    // left out (as pack paths are), and a folder named twice counts once.
    static QStringList roots(const Settings &settings);

    QStringList roots() const { return m_roots; }

    // The folders the files of `type` are in, under every root in order, whether they exist or not.
    QStringList folders(const PortType &type) const;

    // The model files of `type`, in folder order, each folder's sorted by name. A file reached
    // twice (a link, two roots that overlap) is listed once, where it was first found.
    QList<ModelFile> list(const PortType &type) const;

    // The file `handle` names (2.5): its `resource` type's file whose content hash is the handle's
    // `hash`. Fails with model_missing, saying where to put the file, when no such file is found,
    // the handle has no hash (a model a provider serves), or its type is not a resource type here.
    Result<QString> resolve(const QJsonObject &handle, FileHashes &hashes, Hashing hashing = Hashing::Read) const;

    // A resource input's value as a command node gets it (7.2): each handle with the `path` of its
    // file added, a list item by item. A derived handle (2.5) has no file of its own: its `base`,
    // every other handle beside it (a model's `text_encoder` and `vae`) and the `lora` of each
    // `apply` step get theirs. Fails as resolve() does, for the first
    // handle not found.
    Result<QJsonValue> withPaths(const QJsonValue &value, FileHashes &hashes, Hashing hashing = Hashing::Read) const;

private:
    // The declared type when it is a resource type, else null.
    const DeclaredPortType *resourceType(const PortType &type) const;

    QStringList m_roots;
    const Registry *m_registry = nullptr;
    mutable QHash<QString, QList<ModelFile>> m_listed; // by PortType::toString()
};

} // namespace fugue
