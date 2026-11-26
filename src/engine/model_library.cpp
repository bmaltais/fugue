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
#include "engine/model_catalog.h"
#include "engine/model_shape.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/settings.h"

namespace fugue {

namespace {

// ComfyUI's supported_pt_extensions (folder_paths.py) and .gguf, which its GGUF loaders and
// stable-diffusion.cpp read.
// Never destroyed: a thread still running at exit (the finder's) reads it.
const QStringList &defaultExtensions = *new QStringList{QStringLiteral(".safetensors"), QStringLiteral(".gguf"), QStringLiteral(".ckpt"),
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
    ModelLibrary library(roots(settings), registry, rootKinds(settings));
    QStringList inherited;
    for (const SearchPath &root : taggedRoots(settings))
        if (root.inherited)
            inherited << root.path;
    library.setInheritedRoots(inherited);
    if (!settings.downloadFolder.isEmpty())
        library.setDownloadRoot(QDir::cleanPath(expandHome(settings.downloadFolder)));
    return library;
}

QString ModelLibrary::downloadRoot() const {
    // Downloads always go to an own root: the first one when the one set is not here or is inherited.
    return m_roots.contains(m_downloadRoot) && !isInherited(m_downloadRoot) ? m_downloadRoot : m_roots.value(0);
}

QList<SearchPath> ModelLibrary::taggedRoots(const Settings &settings) {
    QStringList own;
    for (const QString &path : settings.modelPaths)
        if (!settings.inheritedModelPaths.contains(path))
            own << path;
    QStringList inherited;
    if (settings.inheritsModels)
        inherited << baseDataDir() + QStringLiteral("/models");
    inherited << settings.inheritedModelPaths;
    return taggedSearchPaths(dataDir() + QStringLiteral("/models"), own, inherited);
}

QStringList ModelLibrary::roots(const Settings &settings) {
    QStringList paths;
    for (const SearchPath &root : taggedRoots(settings))
        paths << root.path;
    return paths;
}

QString ModelLibrary::rootOf(const QString &path) const {
    const QString clean = QDir::cleanPath(path);
    QString best;
    for (const QString &root : m_roots)
        if ((clean == root || clean.startsWith(root + QLatin1Char('/'))) && root.size() > best.size())
            best = root;
    return best;
}

std::optional<Error> ModelLibrary::writeRefusal(const QString &path) const {
    const QString root = rootOf(path);
    if (root.isEmpty() || !isInherited(root))
        return std::nullopt;
    return makeError(QStringLiteral("usage"),
                     QStringLiteral("%1 is in a model folder this project inherits from the Default project (%2), and is read-only here.")
                         .arg(path, root),
                     QStringLiteral("Change it in the Default project (start fugue without --project), or copy the file into a folder of this project."));
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

// Up to kFilesRead model files under `folder`, in name order, depth first, listed by `lister` (the finder's guarded one, or QDir's).
void sample(const ModelLibrary::DirLister &lister, const std::function<bool()> &stop, const QString &folder, int depth, QStringList &out) {
    QList<ModelLibrary::DirEntry> entries;
    if ((stop && stop()) || !lister(folder, entries))
        return;
    for (const ModelLibrary::DirEntry &entry : std::as_const(entries)) {
        if (out.size() >= kFilesRead || (stop && stop()))
            return;
        if (entry.isDir) {
            if (depth < kSampleDepth)
                sample(lister, stop, entry.path, depth + 1, out);
        } else if (hasExtension(entry.name, {QStringLiteral(".safetensors"), QStringLiteral(".gguf")})) {
            out << entry.path;
        }
    }
}

// QDir's listing, the one a library of the settings uses (a folder the person set; the finder gives its own).
bool listWithQDir(const QString &dir, QList<ModelLibrary::DirEntry> &out) {
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries)
        out.append({entry.fileName(), entry.isDir(), entry.absoluteFilePath()});
    return true;
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

const QStringList &ModelLibrary::defaultModelExtensions() { return defaultExtensions; }

namespace {
// The folder names other tools use for a kind (A1111, Forge, SwarmUI, Stability Matrix: pack.json lists them after ComfyUI's, #619).
// A never-destroyed set, so a thread still running at exit never reads a freed one.
const QSet<QString> &otherToolsNames() {
    static const QSet<QString> &names = *new QSet<QString>{
        QStringLiteral("Stable-diffusion"), QStringLiteral("Stable-Diffusion"), QStringLiteral("StableDiffusion"),
        QStringLiteral("Lora"),             QStringLiteral("LyCORIS"),          QStringLiteral("VAE"),
        QStringLiteral("TextEncoders"),     QStringLiteral("DiffusionModels"),  QStringLiteral("RealESRGAN")};
    return names;
}
} // namespace

// ComfyUI's names (the ones before #619) make a layout with one folder, as they always did. The other tools' names make one only
// when two different kinds have a folder (`Lora/` with `LyCORIS/` is one kind, LoRAs): a folder of LoRAs with a `LyCORIS/` and an
// `SDXL/` inside, or of checkpoints with a `VAE/` and an `SD15/`, is still the folder of one kind it was, found by its files.
bool ModelLibrary::layoutFromFolders(const QHash<QString, QString> &typeOfFolder, const QStringList &present, bool topLevelHasModels) {
    QSet<QString> otherKinds;
    for (const QString &name : present) {
        if (!typeOfFolder.contains(name))
            continue;
        if (!otherToolsNames().contains(name))
            return true;
        otherKinds.insert(typeOfFolder.value(name));
    }
    // And a folder with model files of its own at the top (a folder of one kind with a `VAE/` and a `Lora/` inside) keeps the kind
    // it was: only ComfyUI's names make it a layout then.
    return otherKinds.size() >= 2 && !topLevelHasModels;
}

QString ModelLibrary::detectKind(const QString &root, const QHash<QString, QString> &typeOfFolder,
                                 const QHash<QString, QString> &typeOfFiles, const DirLister &listing, const std::function<bool()> &stop) {
    const DirLister &lister = listing ? listing : DirLister(listWithQDir);
    QList<DirEntry> top;
    if (!lister(root, top))
        return {};
    QStringList present;
    bool topModels = false;
    for (const DirEntry &entry : std::as_const(top)) {
        if (entry.isDir && typeOfFolder.contains(entry.name))
            present << entry.name;
        else if (!entry.isDir && hasExtension(entry.name, defaultExtensions))
            topModels = true;
    }
    if (layoutFromFolders(typeOfFolder, present, topModels))
        return kLayout;
    // Else the type whose files most of a few of its files are (pack.json "files").
    QStringList files;
    sample(lister, stop, root, 0, files);
    QHash<QString, int> votes;
    for (const QString &file : std::as_const(files)) {
        if (stop && stop())
            return {};
        if (const std::optional<ModelShape> shape = ModelShapes::shared().of(file); shape && shape->kind)
            ++votes[modelKindName(*shape->kind)];
    }
    for (auto it = votes.begin(); it != votes.end(); ++it)
        if (it.value() * 2 > files.size())
            return typeOfFiles.value(it.key());
    return {};
}

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
    QHash<QString, QString> typeOfFolder, typeOfFiles;
    for (const DeclaredPortType *declared : m_registry->portTypes()) {
        if (declared->kind != PortKind::Resource)
            continue;
        for (const QString &name : folderNames(*declared))
            typeOfFolder.insert(name, declared->type.toString());
        if (!declared->files.isEmpty())
            typeOfFiles.insert(declared->files, declared->type.toString());
    }
    const QString kind = detectKind(root, typeOfFolder, typeOfFiles);
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

bool ModelLibrary::takesFile(const PortType &type, const QString &fileName) const {
    const DeclaredPortType *declared = resourceType(type);
    return declared && hasExtension(fileName, declared->extensions.isEmpty() ? defaultExtensions : declared->extensions);
}

QString ModelLibrary::downloadFolder(const PortType &type) const {
    const DeclaredPortType *declared = resourceType(type);
    if (!declared)
        return {};
    const QStringList names = folderNames(*declared);
    const auto inLayout = [&](const QString &root) {
        for (const QString &name : names)
            if (QFileInfo(root + QLatin1Char('/') + name).isDir())
                return QDir::cleanPath(root + QLatin1Char('/') + name);
        return QDir::cleanPath(root + QLatin1Char('/') + names.first());
    };
    const QString root = downloadRoot();
    const QString kind = kindOf(root);
    if (kind == kLayout)
        return inLayout(root);
    if (kind == type.toString())
        return root;
    return inLayout(m_roots.value(0));
}

QStringList ModelLibrary::downloadRoots(const QList<PortType> &types) const {
    QStringList out;
    for (const QString &root : m_roots) {
        if (isInherited(root))
            continue;
        const QString kind = kindOf(root);
        bool takes = kind == kLayout;
        if (!takes && !kind.isEmpty() && !types.isEmpty()) {
            takes = true;
            for (const PortType &type : types)
                takes = takes && type.toString() == kind;
        }
        if (takes)
            out << root;
    }
    return out;
}

void ModelLibrary::fitDownloadRoot(const QList<PortType> &types) {
    if (!downloadRoots(types).contains(downloadRoot()))
        m_downloadRoot = m_roots.value(0);
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

QJsonObject companionTokenizer(const QString &diffusionFile) {
    const std::optional<ModelShape> shape = diffusionFile.isEmpty() ? std::nullopt : ModelShapes::shared().of(diffusionFile);
    if (!shape || !shape->needsQwen35Tokenizer)
        return {};
    const ModelCatalog catalog = ModelCatalog::load();
    const CatalogModel *entry = catalog.find(QStringLiteral("qwen3.5-tokenizer"));
    return entry ? entry->handle() : QJsonObject();
}

namespace {

// `models` and the companion files of the diffusion models among them that are on this machine: the tokenizer of a MOSS Image model,
// with its source and size from the catalog, so Get ready offers it like a model file of the workflow.
QJsonArray withCompanions(const QJsonArray &models, const ModelLibrary &library, FileHashes &hashes) {
    QJsonArray out = models;
    QSet<QString> listed;
    for (const QJsonValue entry : models)
        listed.insert(entry.toObject().value(QStringLiteral("hash")).toString());
    for (const QJsonValue entry : models) {
        const QJsonObject handle = entry.toObject();
        const QString resource = handle.value(QStringLiteral("resource")).toString();
        if (resource != QLatin1String("fugue/diffusion-model") && resource != QLatin1String("fugue/model"))
            continue;
        const Result<QString> found = library.resolve(handle, hashes, ModelLibrary::Hashing::RememberedOnly);
        if (!found)
            continue; // not here: its own row is the first thing to get, and its shape cannot be read
        QJsonObject tokenizer = companionTokenizer(found.value());
        if (tokenizer.isEmpty() || listed.contains(tokenizer.value(QStringLiteral("hash")).toString()))
            continue;
        listed.insert(tokenizer.value(QStringLiteral("hash")).toString());
        if (const CatalogModel *known = ModelCatalog::load().find(QStringLiteral("qwen3.5-tokenizer"))) {
            tokenizer.insert(QStringLiteral("source"), known->url);
            tokenizer.insert(QStringLiteral("size"), double(known->size));
        }
        out.append(tokenizer);
    }
    return out;
}

} // namespace

QList<MissingModel> ModelLibrary::missing(const QJsonArray &named, FileHashes &hashes) const {
    const QJsonArray models = withCompanions(named, *this, hashes);
    QList<MissingModel> out;
    for (const QJsonValue entry : models) {
        QJsonObject handle = entry.toObject();
        const QString source = handle.take(QStringLiteral("source")).toString();
        const QJsonValue size = handle.take(QStringLiteral("size"));
        if (resolve(handle, hashes, Hashing::RememberedOnly))
            continue;
        const std::optional<PortType> type = PortType::parse(handle.value(QStringLiteral("resource")).toString());
        out.append({handle, source, type ? downloadFolder(*type) : QString(),
                    size.isDouble() && size.toDouble() >= 0 ? qint64(size.toDouble()) : -1});
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
