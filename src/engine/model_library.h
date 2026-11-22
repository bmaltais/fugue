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
// A root in a model path of the settings may hold one kind of model instead, its files directly
// in it (a LoRA folder, #86): `model_folder_kinds` says so, else it is found by its files. A root
// with any type's folder inside is in ComfyUI's layout; one with none has a few of its files'
// headers read (engine/model_shape.h), and the type whose `files` most of them are is the root's.
//
// A handle never holds a path: resolve() finds the file by its content hash. It looks at the
// hashes already remembered (engine/file_hashes) and at the files whose name is the handle's
// `name`, hashing those once. It never hashes every file, so a model renamed since its handle was
// made is found only once its hash is remembered.
//
// A library lists each type's folders once and keeps that list: make one for each use (a run of a
// node, a check), not one for the life of the process. Not thread-safe.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/paths.h"
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

struct Document;

// A model a workflow names that is not in the model folders (4.3, 11.6).
struct MissingModel {
    QJsonObject handle; // resource, hash, name
    QString source;     // where it can be had, as the workflow's `requires` records it; may be empty
    QString folder;     // the folder it goes in: downloadFolder() of its type
    qint64 size = -1;   // in bytes, as `requires` records it; -1 when it does not
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
    // `kinds` says what a root holds, by its path as roots() gives it: kLayout, or a resource
    // type id; a root not named is found by its files.
    ModelLibrary(QStringList roots, const Registry *registry, QHash<QString, QString> kinds = {});
    static inline const QString kLayout = QStringLiteral("layout");

    // The library of the settings in effect now (currentSettings()).
    static ModelLibrary current(const Registry *registry);

    // dataDir()/models, then each model path of `settings` with "~" expanded; a relative path is
    // left out (as pack paths are), and a folder named twice counts once. A project that inherits
    // `models` (1.4.2) has the Default's models folder and model paths after its own, read-only.
    static QStringList roots(const Settings &settings);
    // The same with where each comes from.
    static QList<SearchPath> taggedRoots(const Settings &settings);
    // The settings' model_folder_kinds, by the paths roots() gives.
    static QHash<QString, QString> rootKinds(const Settings &settings);

    QStringList roots() const { return m_roots; }

    // The roots that belong to the Default project (spec 1.4.2): searched like any other, never written
    // in, never a download target. `current()` sets them from the settings.
    void setInheritedRoots(QStringList roots) { m_inherited = std::move(roots); }
    bool isInherited(const QString &root) const { return m_inherited.contains(root); }
    // The root that holds `path` (the longest of roots() that is a parent of it), empty when none does.
    QString rootOf(const QString &path) const;
    // Why `path` may not be deleted, moved or written: it is under an inherited root. The message names
    // where to change it. Nothing when it can.
    std::optional<Error> writeRefusal(const QString &path) const;
    // Where downloads go (#89): one of roots(), the settings' download_folder when it is one
    // of them, else Fugue's own folder (the first root).
    QString downloadRoot() const;
    void setDownloadRoot(const QString &root) { m_downloadRoot = root; }
    // The folder a download of `type` goes in under downloadRoot(): in ComfyUI's layout, the
    // type's folder that is there (`text_encoders` before `clip` when both are), else its first;
    // a root of that one kind is itself. A root of another kind takes none of `type`: then the
    // folder under Fugue's own root. Empty when `type` is not a resource type here.
    QString downloadFolder(const PortType &type) const;
    // The roots a download of all of `types` may go in, in order: those in the layout, and a
    // root of one kind when every type is that kind.
    QStringList downloadRoots(const QList<PortType> &types) const;
    // Makes Fugue's own folder the download root when the one set cannot take every one of
    // `types`, so a batch of downloads goes in one root, not some here and some there.
    void fitDownloadRoot(const QList<PortType> &types);

    // The folders the files of `type` are in, under every root in order, whether they exist or not.
    QStringList folders(const PortType &type) const;

    // True when `fileName` has one of the extensions of `type`'s files (else ComfyUI's list plus
    // .gguf); false when `type` is not a resource type here.
    bool takesFile(const PortType &type, const QString &fileName) const;

    // The model files of `type`, in folder order, each folder's sorted by name. A file reached
    // twice (a link, two roots that overlap) is listed once, where it was first found.
    QList<ModelFile> list(const PortType &type) const;

    // The file `handle` names (2.5): its `resource` type's file whose content hash is the handle's
    // `hash`. Fails with model_missing, saying where to put the file, when no such file is found,
    // the handle has no hash (a model a provider serves), or its type is not a resource type here.
    Result<QString> resolve(const QJsonObject &handle, FileHashes &hashes, Hashing hashing = Hashing::Read) const;

    // The models `document` names (Document::models()) that are not here, in its order. A check:
    // it reads no model file (Hashing::RememberedOnly).
    QList<MissingModel> missing(const Document &document, FileHashes &hashes) const;
    // The same for a list in Document::models()'s form (a template's).
    QList<MissingModel> missing(const QJsonArray &models, FileHashes &hashes) const;

    // A resource input's value as a command node gets it (7.2): each handle with the `path` of its
    // file added, a list item by item. A derived handle (2.5) has no file of its own: its `base`,
    // every other handle beside it (a model's `text_encoder` and `vae`) and the `lora` of each
    // `apply` step get theirs. Fails as resolve() does, for the first
    // handle not found.
    Result<QJsonValue> withPaths(const QJsonValue &value, FileHashes &hashes, Hashing hashing = Hashing::Read) const;

    // What `root` holds: kLayout, a resource type id, or empty (nothing Fugue can tell): set in
    // the settings, else ComfyUI's layout when a type's folder is in it, else what most of a few
    // of its files are.
    QString kindOf(const QString &root) const;

private:
    // The declared type when it is a resource type, else null.
    const DeclaredPortType *resourceType(const PortType &type) const;

    QStringList m_roots;
    QStringList m_inherited;
    const Registry *m_registry = nullptr;
    QHash<QString, QString> m_kinds; // set in the settings, by root
    QString m_downloadRoot;          // empty: the first root
    mutable QHash<QString, QList<ModelFile>> m_listed; // by PortType::toString()
};

} // namespace fugue
