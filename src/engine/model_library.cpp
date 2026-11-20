#include "model_library.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

#include <algorithm>

#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/settings.h"

namespace fugue {

namespace {

// ComfyUI's supported_pt_extensions (folder_paths.py) and .gguf, which its GGUF loaders and
// stable-diffusion.cpp read.
const QStringList defaultExtensions{QStringLiteral(".safetensors"), QStringLiteral(".gguf"), QStringLiteral(".ckpt"),
                                    QStringLiteral(".pt"),          QStringLiteral(".pt2"),  QStringLiteral(".pth"),
                                    QStringLiteral(".bin"),         QStringLiteral(".sft"),  QStringLiteral(".pkl")};

// An extension matches the whole suffix after a dot: "pt" and ".pt" both match "x.pt", neither "x.ckpt".
bool hasExtension(const QString &fileName, const QStringList &extensions) {
    for (const QString &extension : extensions) {
        const QString dotted = extension.startsWith(QLatin1Char('.')) ? extension : QLatin1Char('.') + extension;
        if (fileName.endsWith(dotted, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// The files under `folder` (and its subfolders) with one of `extensions`, named relative to it;
// `top` is the type folder the walk started in.
// Names starting with "." are left out (QDir lists no hidden entries); a folder already walked,
// through a link or another root, is not walked again.
void walk(const QString &top, const QString &folder, const QString &prefix, const QStringList &extensions,
          QSet<QString> &walked, QList<ModelFile> &out) {
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    if (canonical.isEmpty() || walked.contains(canonical))
        return;
    walked.insert(canonical);
    const QFileInfoList entries =
        QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString name = prefix + entry.fileName();
        if (entry.isDir())
            walk(top, entry.filePath(), name + QLatin1Char('/'), extensions, walked, out);
        else if (hasExtension(entry.fileName(), extensions))
            out.append(ModelFile{QDir::cleanPath(entry.absoluteFilePath()), name, top, entry.size()});
    }
}

Error missing(const QString &message, const QString &hint) {
    return makeError(QStringLiteral("model_missing"), message, hint);
}

} // namespace

ModelLibrary::ModelLibrary(QStringList roots, const Registry *registry)
    : m_roots(std::move(roots)), m_registry(registry) {}

ModelLibrary ModelLibrary::current(const Registry *registry) {
    return ModelLibrary(roots(currentSettings()), registry);
}

QStringList ModelLibrary::roots(const Settings &settings) {
    return searchPaths(dataDir() + QStringLiteral("/models"), settings.modelPaths);
}

const DeclaredPortType *ModelLibrary::resourceType(const PortType &type) const {
    const DeclaredPortType *declared = m_registry ? m_registry->portType(type) : nullptr;
    return declared && declared->kind == PortKind::Resource ? declared : nullptr;
}

QStringList ModelLibrary::folders(const PortType &type) const {
    const DeclaredPortType *declared = resourceType(type);
    if (!declared)
        return {};
    const QStringList names = declared->folders.isEmpty() ? QStringList{type.id.section(QLatin1Char('/'), -1)}
                                                          : declared->folders;
    QStringList out;
    for (const QString &root : m_roots)
        for (const QString &name : names)
            out << QDir::cleanPath(root + QLatin1Char('/') + name);
    return out;
}

QList<ModelFile> ModelLibrary::list(const PortType &type) const {
    const QString key = type.toString();
    if (const auto listed = m_listed.constFind(key); listed != m_listed.constEnd())
        return *listed;
    const DeclaredPortType *declared = resourceType(type);
    if (!declared)
        return {};
    const QStringList &extensions = declared->extensions.isEmpty() ? defaultExtensions : declared->extensions;
    QSet<QString> walked;
    QSet<QString> seen; // canonical paths of the files listed so far
    QList<ModelFile> out;
    for (const QString &folder : folders(type)) {
        QList<ModelFile> found;
        walk(folder, folder, {}, extensions, walked, found);
        std::sort(found.begin(), found.end(), [](const ModelFile &a, const ModelFile &b) { return a.name < b.name; });
        for (const ModelFile &file : found) {
            const QString canonical = QFileInfo(file.path).canonicalFilePath();
            if (seen.contains(canonical))
                continue;
            seen.insert(canonical);
            out.append(file);
        }
    }
    m_listed.insert(key, out);
    return out;
}

Result<QString> ModelLibrary::resolve(const QJsonObject &handle, FileHashes &hashes, Hashing hashing) const {
    const QString resource = handle.value("resource").toString();
    const QString hash = handle.value("hash").toString();
    const QString name = handle.value("name").toString();
    const QString shown = name.isEmpty() ? resource : name;
    const std::optional<PortType> type = PortType::parse(resource);
    const DeclaredPortType *declared = type ? resourceType(*type) : nullptr;
    if (!declared)
        return missing(QStringLiteral("%1 is not a resource type of an installed pack.")
                           .arg(resource.isEmpty() ? QStringLiteral("A model with no type") : resource),
                       resource.isEmpty() ? QStringLiteral("Pick the model again.")
                                          : QStringLiteral("Install the pack that declares %1.").arg(resource));
    if (!isSha256(hash))
        return missing(QStringLiteral("The model %1 is served by a provider; it has no file on disk to give this node.")
                           .arg(shown),
                       QStringLiteral("Pick a model file for this input."));

    const QList<ModelFile> files = list(*type);
    for (const ModelFile &file : files)
        if (hashes.known(file.path) == hash)
            return file.path;
    // A file with the handle's name, hashed once (the hash is remembered). Other files are not
    // read: hashing every model of a large folder takes minutes.
    const QString fileName = QFileInfo(name).fileName();
    if (!fileName.isEmpty())
        for (const ModelFile &file : files) {
            if (QFileInfo(file.name).fileName() != fileName)
                continue;
            if (hashing == Hashing::RememberedOnly ? hashes.known(file.path).isEmpty() : hashes.hash(file.path) == hash)
                return file.path;
        }

    return missing(QStringLiteral("The model %1 (%2) is not in the model folders.").arg(shown, declared->name),
                   QStringLiteral("Put %1 in %2, or add the folder it is in to Extra model folders in Settings.")
                       .arg(fileName.isEmpty() ? QStringLiteral("the file") : fileName, folders(*type).value(0)));
}

Result<QJsonValue> ModelLibrary::withPaths(const QJsonValue &value, FileHashes &hashes, Hashing hashing) const {
    if (value.isArray()) {
        QJsonArray items;
        for (const QJsonValue &item : value.toArray()) {
            const Result<QJsonValue> one = withPaths(item, hashes, hashing);
            if (!one)
                return one;
            items.append(one.value());
        }
        return QJsonValue(items);
    }
    const QJsonObject handle = value.toObject();
    if (!handle.contains("derived")) {
        const Result<QString> path = resolve(handle, hashes, hashing);
        if (!path)
            return path.error();
        QJsonObject out = handle;
        out.insert("path", path.value());
        return QJsonValue(out);
    }
    QJsonObject derived = handle.value("derived").toObject();
    const Result<QJsonValue> base = withPaths(derived.value("base"), hashes, hashing);
    if (!base)
        return base;
    derived.insert("base", base.value());
    // The other parts named beside the base (Load Models' text_encoder and vae).
    for (const QString &key : derived.keys()) {
        const QJsonValue part = derived.value(key);
        if (key == QLatin1String("base") || !part.isObject() || !part.toObject().contains("resource"))
            continue;
        const Result<QJsonValue> found = withPaths(part, hashes, hashing);
        if (!found)
            return found;
        derived.insert(key, found.value());
    }
    QJsonArray apply = derived.value("apply").toArray();
    for (qsizetype i = 0; i < apply.size(); ++i) {
        QJsonObject step = apply.at(i).toObject();
        const Result<QJsonValue> lora = withPaths(step.value("lora"), hashes, hashing);
        if (!lora)
            return lora;
        step.insert("lora", lora.value());
        apply[i] = step;
    }
    if (derived.contains("apply"))
        derived.insert("apply", apply);
    QJsonObject out = handle;
    out.insert("derived", derived);
    return QJsonValue(out);
}

} // namespace fugue
