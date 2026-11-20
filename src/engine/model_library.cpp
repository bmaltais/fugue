#include "model_library.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#include <algorithm>

#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/model_shape.h"
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

// An extension matches the whole suffix after a dot: "pt" and ".pt" both match "x.pt", neither
// "x.ckpt".
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

Error notFound(const QString &message, const QString &hint) {
    return makeError(QStringLiteral("model_missing"), message, hint);
}

} // namespace

ModelLibrary::ModelLibrary(QStringList roots, const Registry *registry, QHash<QString, QString> kinds)
    : m_roots(std::move(roots)), m_registry(registry), m_kinds(std::move(kinds)) {}

ModelLibrary ModelLibrary::current(const Registry *registry) {
    const Settings settings = currentSettings();
    return ModelLibrary(roots(settings), registry, rootKinds(settings));
}

QStringList ModelLibrary::roots(const Settings &settings) {
    return searchPaths(dataDir() + QStringLiteral("/models"), settings.modelPaths);
}

QHash<QString, QString> ModelLibrary::rootKinds(const Settings &settings) {
    QHash<QString, QString> kinds;
    for (auto it = settings.modelFolderKinds.begin(); it != settings.modelFolderKinds.end(); ++it)
        kinds.insert(QDir::cleanPath(expandHome(it.key())), it.value());
    return kinds;
}

namespace {

// A resource type's folder names under each root (8.9): its pack's, else its id's last part.
QStringList folderNames(const DeclaredPortType &declared) {
    return declared.folders.isEmpty() ? QStringList{declared.type.id.section(QLatin1Char('/'), -1)} : declared.folders;
}

// How many files of a folder are read to tell what it holds, and how deep they are looked for.
constexpr int kFilesRead = 8;
constexpr int kSampleDepth = 3;

// Up to kFilesRead model files under `folder`, in name order, depth first.
void sample(const QString &folder, int depth, QStringList &out) {
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (out.size() >= kFilesRead)
            return;
        if (entry.isDir()) {
            if (depth < kSampleDepth)
                sample(entry.filePath(), depth + 1, out);
        } else if (hasExtension(entry.fileName(), {QStringLiteral(".safetensors"), QStringLiteral(".gguf")})) {
            out << entry.absoluteFilePath();
        }
    }
}

// What a folder's files say it holds, for the process: a library is made for each use, the
// window makes one each time a picker opens. Again when the folder itself changes.
class FoundKinds {
public:
    std::optional<QString> find(const QString &root, qint64 modified) {
        QMutexLocker lock(&m_mutex);
        const auto it = m_kinds.constFind(root);
        return it != m_kinds.constEnd() && it->first == modified ? std::optional<QString>(it->second) : std::nullopt;
    }
    void keep(const QString &root, qint64 modified, const QString &kind) {
        QMutexLocker lock(&m_mutex);
        m_kinds.insert(root, {modified, kind});
    }

private:
    QMutex m_mutex;
    QHash<QString, std::pair<qint64, QString>> m_kinds;
};

FoundKinds &foundKinds() {
    static FoundKinds kinds;
    return kinds;
}

} // namespace

QString ModelLibrary::kindOf(const QString &root) const {
    if (const QString set = m_kinds.value(root); !set.isEmpty())
        return set;
    // Fugue's own folder (roots() puts it first) keeps the layout: a missing model's hint names
    // the folder to put it in there.
    if (root == m_roots.value(0) || !m_registry)
        return kLayout;
    const qint64 modified = QFileInfo(root).lastModified().toMSecsSinceEpoch();
    if (const std::optional<QString> found = foundKinds().find(root, modified))
        return *found;
    const QList<const DeclaredPortType *> types = m_registry->portTypes();
    QString kind;
    // ComfyUI's layout when any resource type's folder is there.
    for (const DeclaredPortType *declared : types) {
        if (declared->kind != PortKind::Resource)
            continue;
        for (const QString &name : folderNames(*declared))
            if (QFileInfo(root + QLatin1Char('/') + name).isDir())
                kind = kLayout;
        if (!kind.isEmpty())
            break;
    }
    // Else the type whose files most of a few of its files are (pack.json "files").
    if (kind.isEmpty()) {
        QStringList files;
        sample(root, 0, files);
        QHash<QString, int> votes;
        for (const QString &file : std::as_const(files))
            if (const std::optional<ModelShape> shape = ModelShapes::shared().of(file); shape && shape->kind)
                ++votes[modelKindName(*shape->kind)];
        for (auto it = votes.begin(); it != votes.end() && kind.isEmpty(); ++it)
            if (it.value() * 2 > files.size())
                for (const DeclaredPortType *declared : types)
                    if (declared->kind == PortKind::Resource && declared->files == it.key())
                        kind = declared->type.toString();
    }
    foundKinds().keep(root, modified, kind);
    return kind;
}

const DeclaredPortType *ModelLibrary::resourceType(const PortType &type) const {
    const DeclaredPortType *declared = m_registry ? m_registry->portType(type) : nullptr;
    return declared && declared->kind == PortKind::Resource ? declared : nullptr;
}

QStringList ModelLibrary::folders(const PortType &type) const {
    const DeclaredPortType *declared = resourceType(type);
    if (!declared)
        return {};
    const QStringList names = folderNames(*declared);
    QStringList out;
    for (const QString &root : m_roots) {
        const QString kind = kindOf(root);
        if (kind == kLayout)
            for (const QString &name : names)
                out << QDir::cleanPath(root + QLatin1Char('/') + name);
        else if (kind == type.toString())
            out << root;
    }
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
        return notFound(QStringLiteral("%1 is not a resource type of an installed pack.")
                           .arg(resource.isEmpty() ? QStringLiteral("A model with no type") : resource),
                       resource.isEmpty() ? QStringLiteral("Pick the model again.")
                                          : QStringLiteral("Install the pack that declares %1.").arg(resource));
    if (!isSha256(hash))
        return notFound(QStringLiteral("The model %1 is served by a provider; it has no file on disk to give this node.")
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

    return notFound(QStringLiteral("The model %1 (%2) is not in the model folders.").arg(shown, declared->name),
                   QStringLiteral("Put %1 in %2, or add the folder it is in to Extra model folders in Settings.")
                       .arg(fileName.isEmpty() ? QStringLiteral("the file") : fileName, folders(*type).value(0)));
}

QList<MissingModel> ModelLibrary::missing(const Document &document, FileHashes &hashes) const {
    return missing(document.models(), hashes);
}

QList<MissingModel> ModelLibrary::missing(const QJsonArray &models, FileHashes &hashes) const {
    QList<MissingModel> out;
    for (const QJsonValue entry : models) {
        QJsonObject handle = entry.toObject();
        const QString source = handle.take(QStringLiteral("source")).toString();
        if (resolve(handle, hashes, Hashing::RememberedOnly))
            continue;
        const std::optional<PortType> type = PortType::parse(handle.value(QStringLiteral("resource")).toString());
        out.append({handle, source, type ? folders(*type).value(0) : QString()});
    }
    return out;
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
