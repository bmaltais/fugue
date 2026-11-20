#include "model_library.h"

#include <QDir>
#include <QJsonArray>
#include <QFileInfo>
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

bool hasExtension(const QString &fileName, const QStringList &extensions) {
    for (const QString &extension : extensions)
        if (fileName.endsWith(extension, Qt::CaseInsensitive))
            return true;
    return false;
}

// The files under `folder` (and its subfolders) with one of `extensions`, named relative to it.
// Names starting with "." are left out (QDir lists no hidden entries); a folder already walked,
// through a link or another root, is not walked again.
void walk(const QString &folder, const QString &prefix, const QStringList &extensions, QSet<QString> &walked,
          QList<ModelFile> &out) {
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    if (canonical.isEmpty() || walked.contains(canonical))
        return;
    walked.insert(canonical);
    const QFileInfoList entries =
        QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString name = prefix + entry.fileName();
        if (entry.isDir())
            walk(entry.filePath(), name + QLatin1Char('/'), extensions, walked, out);
        else if (hasExtension(entry.fileName(), extensions))
            out.append(ModelFile{QDir::cleanPath(entry.absoluteFilePath()), name, entry.size()});
    }
}

} // namespace

ModelLibrary::ModelLibrary(QStringList roots, const Registry *registry)
    : m_roots(std::move(roots)), m_registry(registry) {}

ModelLibrary ModelLibrary::current(const Registry *registry) {
    return ModelLibrary(roots(currentSettings()), registry);
}

QStringList ModelLibrary::roots(const Settings &settings) {
    QStringList out{QDir::cleanPath(dataDir() + QStringLiteral("/models"))};
    for (const QString &path : settings.modelPaths) {
        const QString expanded = expandHome(path);
        if (!QDir::isAbsolutePath(expanded))
            continue;
        const QString clean = QDir::cleanPath(expanded);
        if (!out.contains(clean))
            out << clean;
    }
    return out;
}

QStringList ModelLibrary::folders(const PortType &type) const {
    const DeclaredPortType *declared = m_registry ? m_registry->portType(type) : nullptr;
    if (!declared || declared->kind != PortKind::Resource)
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
    const DeclaredPortType *declared = m_registry ? m_registry->portType(type) : nullptr;
    if (!declared || declared->kind != PortKind::Resource)
        return {};
    const QStringList &extensions = declared->extensions.isEmpty() ? defaultExtensions : declared->extensions;
    QSet<QString> walked;
    QSet<QString> seen; // canonical paths of the files listed so far
    QList<ModelFile> out;
    for (const QString &folder : folders(type)) {
        QList<ModelFile> found;
        walk(folder, {}, extensions, walked, found);
        std::sort(found.begin(), found.end(), [](const ModelFile &a, const ModelFile &b) { return a.name < b.name; });
        for (const ModelFile &file : found) {
            const QString canonical = QFileInfo(file.path).canonicalFilePath();
            if (seen.contains(canonical))
                continue;
            seen.insert(canonical);
            out.append(file);
        }
    }
    return out;
}

Result<QString> ModelLibrary::resolve(const QJsonObject &handle, FileHashes &hashes) const {
    const QString resource = handle.value("resource").toString();
    const QString hash = handle.value("hash").toString();
    const QString name = handle.value("name").toString();
    const QString shown = name.isEmpty() ? resource : name;
    const std::optional<PortType> type = PortType::parse(resource);
    const DeclaredPortType *declared = type && m_registry ? m_registry->portType(*type) : nullptr;
    if (!declared || declared->kind != PortKind::Resource)
        return makeError(QStringLiteral("model_missing"),
                         QStringLiteral("%1 is not a model type of an installed pack.").arg(resource),
                         QStringLiteral("Install the pack that declares %1.").arg(resource));
    if (!isSha256(hash))
        return makeError(QStringLiteral("model_missing"),
                         QStringLiteral("The model %1 has no file hash, so it cannot be found on disk.").arg(shown),
                         QStringLiteral("Pick the model again."));

    const QList<ModelFile> files = list(*type);
    for (const ModelFile &file : files)
        if (hashes.known(file.path) == hash)
            return file.path;
    // A file with the handle's name, hashed once (the hash is remembered). Other files are not
    // read: hashing every model of a large folder takes minutes.
    const QString fileName = QFileInfo(name).fileName();
    if (!fileName.isEmpty())
        for (const ModelFile &file : files)
            if (QFileInfo(file.name).fileName() == fileName && hashes.hash(file.path) == hash)
                return file.path;

    const QStringList searched = folders(*type);
    return makeError(QStringLiteral("model_missing"),
                     QStringLiteral("The model %1 (%2) is not in the models folders.").arg(shown, declared->name),
                     QStringLiteral("Put %1 in %2, or add the folder it is in to Extra model folders in Settings.")
                         .arg(fileName.isEmpty() ? QStringLiteral("the file") : fileName, searched.value(0)));
}

Result<QJsonObject> ModelLibrary::withPaths(const QJsonObject &handle, FileHashes &hashes) const {
    if (!handle.contains("derived")) {
        const Result<QString> path = resolve(handle, hashes);
        if (!path)
            return path.error();
        QJsonObject out = handle;
        out.insert("path", path.value());
        return out;
    }
    const auto isHandle = [](const QJsonValue &value) { return value.isObject() && value.toObject().contains("resource"); };
    QJsonObject derived = handle.value("derived").toObject();
    for (auto part = derived.begin(); part != derived.end(); ++part) {
        if (isHandle(part.value())) {
            const Result<QJsonObject> made = withPaths(part.value().toObject(), hashes);
            if (!made)
                return made.error();
            part.value() = made.value();
        } else if (part.value().isArray()) {
            QJsonArray steps = part.value().toArray();
            for (qsizetype i = 0; i < steps.size(); ++i) {
                QJsonObject step = steps.at(i).toObject();
                for (auto field = step.begin(); field != step.end(); ++field) {
                    if (!isHandle(field.value()))
                        continue;
                    const Result<QJsonObject> made = withPaths(field.value().toObject(), hashes);
                    if (!made)
                        return made.error();
                    field.value() = made.value();
                }
                if (steps.at(i).isObject())
                    steps[i] = step;
            }
            part.value() = steps;
        }
    }
    QJsonObject out = handle;
    out.insert("derived", derived);
    return out;
}

} // namespace fugue
