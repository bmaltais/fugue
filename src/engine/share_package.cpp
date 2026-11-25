#include "share_package.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <functional>
#include <memory>

#include "engine/app_binding.h"
#include "engine/atomic_file.h"
#include "engine/document.h"
#include "engine/file_inputs.h"
#include "engine/file_roots.h"
#include "engine/group_library.h"
#include "engine/image_format.h"
#include "engine/json.h"
#include "engine/pack_loader.h"
#include "engine/pack_manager.h"
#include "engine/registry.h"
#include "engine/trust_store.h"
#include "engine/zip_archive.h"

namespace fugue {

namespace {

constexpr qint64 kMaxPacked = qint64(3) << 30;   // every input of one package, written
constexpr qint64 kMaxUnpacked = qint64(4) << 30; // a whole package, unpacked
constexpr int kMaxEntries = 10000;

// The template's folder name: the title in lowercase letters, digits and dashes.
QString folderId(const QString &title) {
    static const QRegularExpression other(QStringLiteral("[^a-z0-9]+"));
    QString id = title.toLower().replace(other, QStringLiteral("-"));
    while (id.startsWith(QLatin1Char('-')))
        id.remove(0, 1);
    while (id.endsWith(QLatin1Char('-')))
        id.chop(1);
    return id.isEmpty() ? QStringLiteral("workflow") : id.left(64);
}

// A file name every system unpacks and no template check skips: letters, digits, "._-" only,
// not starting with a dot; the image's own extension when the name has none.
QString packedName(const QString &source, const QString &defaultExtension) {
    static const QRegularExpression other(QStringLiteral("[^A-Za-z0-9._-]"));
    QString name = QFileInfo(source).fileName().replace(other, QStringLiteral("_"));
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    if (QFileInfo(name).completeBaseName().isEmpty())
        name = QStringLiteral("image") + name;
    if (QFileInfo(name).suffix().isEmpty())
        name += QLatin1Char('.') + defaultExtension;
    return name.left(120);
}

// The name `source` takes among the package's inputs: its own, else `<stem>-2.<ext>`...
// `claimed` maps the names given so far to their sources, so a file named twice goes in once.
QString uniqueName(const QString &wanted, const QString &source, QHash<QString, QString> &claimed) {
    const QFileInfo info(wanted);
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? wanted : QStringLiteral("%1-%2%3").arg(info.completeBaseName()).arg(n).arg(suffix);
        const auto taken = claimed.constFind(name);
        if (taken == claimed.constEnd()) {
            claimed.insert(name, source);
            return name;
        }
        if (*taken == source)
            return name;
    }
}

// Points every `file` input of `document` at `inputs/<name>` and lists the file to put there.
// Only an image Fugue takes in (image_format.h: by its bytes, at most maxImageBytes) goes in, so
// what is shared is what the person sees on the canvas, never another file a node names;
// another file, or one not there, is a warning, its input keeping the bare file name.
//
// An app bundle (#394) also takes any file, of any type, that a node an app input is bound to names (`anyFile`: the node.port
// texts of those targets), as long as it is a file Fugue can read and at most maxImageBytes.
Errors bundleInputs(Document &document, const Registry &registry, const QString &workflowPath, const QString &id,
                    QList<ZipEntry> &entries, QStringList *included, const QSet<QString> &anyFile = {}) {
    Errors errors;
    QHash<QString, QString> claimed;
    qint64 total = 0;
    const auto warn = [&](const QString &node, const QString &input, const QString &message) {
        Error warning = makeError(QStringLiteral("file_not_found"), message,
                                  QStringLiteral("Pick the image again, then export again."));
        warning.severity = Error::Severity::Warning;
        warning.node = node;
        warning.input = input;
        errors << warning;
    };
    forEachFileLiteral(document, registry, [&](const QString &node, const QString &input, InputValue &value) {
        const QString text = value.literal().toString();
        if (text.trimmed().isEmpty())
            return;
        const QString bare = QFileInfo(text.trimmed()).fileName();
        const Result<QString> located = inputFilePath(text, workflowPath, input);
        const QString path = located ? canonicalPath(located.value()) : QString();
        const QFileInfo info(path);
        QFile file(path);
        if (path.isEmpty() || !info.isFile() || !file.open(QIODevice::ReadOnly)) {
            warn(node, input, QStringLiteral("%1 is not there, so it is not in the package.").arg(bare));
            value = InputValue::literal(bare);
            return;
        }
        const std::optional<ImageFormat> format = sniffInputFile(file.read(128));
        const bool bound = anyFile.contains(node + QLatin1Char('.') + input);
        if ((!format && !bound) || info.size() > maxImageBytes) {
            warn(node, input,
                 info.size() > maxImageBytes
                     ? QStringLiteral("%1 is over %2 MB, so it is not in the package.").arg(bare).arg(maxImageBytes >> 20)
                     : QStringLiteral("%1 is not an image or a paint file, so it is not in the package.").arg(bare));
            value = InputValue::literal(bare);
            return;
        }
        const QString name = uniqueName(packedName(path, format ? format->extension : QStringLiteral("bin")), path, claimed);
        const QString inside = id + QStringLiteral("/inputs/") + name;
        bool listed = false;
        for (const ZipEntry &entry : std::as_const(entries))
            listed = listed || entry.name == inside;
        if (!listed) {
            total += info.size();
            entries << ZipEntry{inside, {}, path};
            if (included)
                *included << info.fileName();
        }
        value = InputValue::literal(QStringLiteral("inputs/") + name);
    });
    if (total > kMaxPacked)
        errors << makeError(QStringLiteral("value_out_of_range"),
                            QStringLiteral("Its images come to %1 GB; a package holds at most %2 GB.")
                                .arg(double(total) / (1 << 30), 0, 'f', 1)
                                .arg(kMaxPacked >> 30),
                            QStringLiteral("Share the largest images another way, and export without them."));
    return errors;
}


// --- app bundles (#394) ---

// The `node.port` of every input an app input is bound to.
QSet<QString> boundTargets(const Document &document) {
    QSet<QString> bound;
    const Result<std::optional<app::App>> block = app::appOf(document);
    if (block && *block)
        for (const InputBinding &input : (*block)->inputs)
            for (const PortRef &target : input.targets)
                bound.insert(target.toString());
    return bound;
}

// One pack to carry: where it is, and the files that go in (name inside the pack folder, file).
struct PackFiles {
    QString folder;
    QList<std::pair<QString, QString>> files;
};

// What an installed pack's folder lends: everything but dot files and folders (git, the .venv a setup makes, the source record),
// the environments and caches that a setup or a run leaves, and links. False when a file is over a file's cap.
bool collectPackFiles(const QString &folder, PackFiles &out, QString *why) {
    static const QStringList skipped{QStringLiteral("venv"), QStringLiteral("__pycache__"), QStringLiteral("node_modules")};
    out.folder = folder;
    const QDir root(folder);
    const std::function<bool(const QString &)> walk = [&](const QString &relative) {
        const QFileInfoList items = QDir(root.filePath(relative)).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                                                QDir::Name);
        for (const QFileInfo &item : items) {
            const QString name = item.fileName();
            if (name.startsWith(QLatin1Char('.')) || item.isSymLink())
                continue;
            const QString inside = relative.isEmpty() ? name : relative + QLatin1Char('/') + name;
            if (item.isDir()) {
                if (!skipped.contains(name) && !walk(inside))
                    return false;
            } else if (item.isFile()) {
                if (item.size() > maxImageBytes) {
                    *why = QStringLiteral("%1 is over %2 MB").arg(inside).arg(maxImageBytes >> 20);
                    return false;
                }
                if (out.files.size() >= kMaxEntries / 2) {
                    *why = QStringLiteral("it holds more than %1 files").arg(kMaxEntries / 2);
                    return false;
                }
                out.files.append({inside, item.absoluteFilePath()});
            }
        }
        return true;
    };
    return walk(QString());
}

// The folder of the pack `id` at `version`: a carried one of the template first, then the search paths.
QString packFolderFor(const QString &id, const QString &version, const ShareRequest &request) {
    for (const CarriedPack &pack : request.carried)
        if (pack.id == id && pack.version == version && QFileInfo(pack.folder).isDir())
            return pack.folder;
    QStringList paths = request.packPaths;
    if (paths.isEmpty())
        paths = packSearchPaths() + inheritedPackPaths();
    for (const QString &path : std::as_const(paths))
        for (const QFileInfo &folder : QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const Result<QJsonObject> json = readJsonObject(folder.absoluteFilePath() + QStringLiteral("/pack.json"));
            if (json && json->value("id").toString() == id && json->value("version").toString() == version)
                return folder.absoluteFilePath();
        }
    return {};
}

struct AppPacks {
    QList<std::pair<QString, PackFiles>> carried; // pack id -> files
    QJsonObject commits;                         // pack id -> {commit} to record in requires.packs
    Errors errors;                               // refusals and warnings
};

Error itemError(const QString &message, const QString &hint, bool refuse) {
    Error error = makeError(QStringLiteral("app_not_shareable"), message, hint);
    if (!refuse)
        error.severity = Error::Severity::Warning;
    return error;
}

// The packs `packed` uses, each carried when its folder is here, else fetched by its recorded source, else a refusal.
AppPacks packsForApp(const Document &packed, const Registry &registry, const ShareRequest &request) {
    AppPacks out;
    QSet<QString> seen;
    for (const PackUse &use : TrustStore::packsUsed(packed, registry)) {
        const QString name = packTitle(use.id) + QLatin1Char(' ') + use.version;
        const QString source = packed.requirements.value("packs").toObject().value(use.id).toObject().value("source").toString();
        QString why;
        const QString folder = use.version.isEmpty() ? QString() : packFolderFor(use.id, use.version, request);
        PackFiles files;
        if (folder.isEmpty())
            why = use.version.isEmpty() ? QStringLiteral("it is not installed here") : QStringLiteral("its folder is not found here");
        else if (seen.contains(use.id))
            why = QStringLiteral("another version of it is already in the bundle");
        else if (!collectPackFiles(folder, files, &why))
            why = QStringLiteral("it cannot be carried: %1").arg(why);
        if (why.isEmpty()) {
            seen.insert(use.id);
            const PackSource recorded = readPackSource(folder);
            if (!recorded.commit.isEmpty())
                out.commits.insert(use.id, recorded.commit);
            out.carried.append({use.id, files});
        } else if (!source.isEmpty()) {
            out.errors << itemError(QStringLiteral("The pack %1 is not in the bundle (%2); the person who opens it gets it from %3.").arg(name, why, source),
                                    QStringLiteral("Install it here so the bundle carries it."), false);
        } else {
            out.errors << itemError(QStringLiteral("The pack %1 cannot be carried (%2) and has no source to get it from, so whoever opens the app is stuck.").arg(name, why),
                                    QStringLiteral("Install the pack from its folder or git address (`fugue pack add`), then export again."), true);
        }
    }
    return out;
}

// A model with no source cannot be downloaded by whoever opens the app.
Errors modelsForApp(const Document &packed) {
    Errors errors;
    for (const QJsonValue &value : packed.models()) {
        const QJsonObject model = value.toObject();
        if (!model.value("source").toString().isEmpty())
            continue;
        errors << itemError(QStringLiteral("The model %1 has no download source, so whoever opens the app cannot get it.").arg(model.value("name").toString()),
                            QStringLiteral("Download it again from its page so its source is recorded, or add a `source` to the model in the workflow's requirements."), true);
    }
    return errors;
}

// The share copy of a document as an app bundle takes it, with its pack check.
Document packedCopy(const Document &document, const Registry &registry) {
    Document packed = document;
    embedGroupTypes(packed, registry);
    return packed;
}

} // namespace

Errors checkAppShare(const Document &document, const Registry &registry, const ShareRequest &request) {
    const Document packed = packedCopy(document, registry);
    Errors errors = packsForApp(packed, registry, request).errors;
    errors << modelsForApp(packed);
    return errors;
}

Errors writeSharePackage(const Document &document, const Registry &registry, const ShareRequest &request,
                         const QString &zipPath, QStringList *included, QStringList *packsCarried) {
    const QString title = request.title.trimmed().isEmpty() ? QStringLiteral("Workflow") : request.title.trimmed();
    const QString id = folderId(title);
    QList<ZipEntry> entries;
    // A library or pack group it names only by its type travels as its definition: a group is
    // a graph of other nodes, no program, so the person needs neither the library nor the pack.
    Document packed = packedCopy(document, registry);
    Errors errors;
    AppPacks packs;
    if (request.app) {
        // Before anything is read or written: what the receiver could not get refuses the export, naming each item.
        packs = packsForApp(packed, registry, request);
        errors = packs.errors;
        errors << modelsForApp(packed);
        if (hasErrors(errors))
            return errors;
    }
    errors << bundleInputs(packed, registry, request.workflowPath, id, entries, included,
                           request.app ? boundTargets(packed) : QSet<QString>());
    if (hasErrors(errors))
        return errors;
    packed.refreshRequirements(request.appVersion, &registry);
    if (request.app) {
        // The commit each carried pack was at, so the receiver's copy can be updated (6.7).
        QJsonObject recordedPacks = packed.requirements.value("packs").toObject();
        for (auto it = packs.commits.begin(); it != packs.commits.end(); ++it) {
            QJsonObject entry = recordedPacks.value(it.key()).toObject();
            entry.insert("commit", it.value());
            recordedPacks.insert(it.key(), entry);
        }
        if (!recordedPacks.isEmpty())
            packed.requirements.insert("packs", recordedPacks);
        for (const auto &[packId, files] : std::as_const(packs.carried)) {
            if (packsCarried)
                *packsCarried << packId;
            for (const auto &[name, file] : files.files)
                entries << ZipEntry{id + QStringLiteral("/packs/") + packId + QLatin1Char('/') + name, {}, file,
                                    QFileInfo(file).permission(QFileDevice::ExeOwner)}; // run.sh stays a program
        }
    }
    entries.prepend(ZipEntry{id + QStringLiteral("/template.json"), writeJson(QJsonObject{{"title", title}}), {}});
    entries.prepend(ZipEntry{id + QStringLiteral("/workflow.fugue"),
                          writeJson(shareCopy(packed.toJson(), registry, request.options)), {}});
    if (!request.preview.isEmpty()) {
        QFile preview(request.preview);
        const std::optional<ImageFormat> format =
            preview.open(QIODevice::ReadOnly) ? sniffImage(preview.read(16)) : std::nullopt;
        if (format && QStringList{"png", "webp", "jpg"}.contains(format->extension) && preview.size() <= maxImageBytes)
            entries << ZipEntry{id + QStringLiteral("/preview.") + format->extension, {}, request.preview};
    }
    if (entries.size() > kMaxEntries)
        return {makeError(QStringLiteral("value_out_of_range"),
                          QStringLiteral("The package would hold %1 files; a package holds at most %2.").arg(entries.size()).arg(kMaxEntries),
                          QStringLiteral("Share it without the largest packs, or fewer files."))};

    if (const Errors written = writeZipFile(entries, zipPath); hasErrors(written))
        return written;
    return errors; // the warnings
}

bool isSharePackage(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(4) == QByteArray("PK\x03\x04", 4);
}

namespace {

Result<QString> unpackInto(const QString &zipPath, const QString &folder) {
    const Errors unpacked = unpackZip(zipPath, folder, UnpackLimits{maxImageBytes, kMaxUnpacked, kMaxEntries});
    if (hasErrors(unpacked))
        return Result<QString>(unpacked);

    // Its files at its top: they go in a new folder named after the zip, as a template's would be.
    if (QFileInfo::exists(folder + QStringLiteral("/workflow.fugue"))) {
        const QString base = folderId(QFileInfo(zipPath).completeBaseName());
        QString inner = folder + QLatin1Char('/') + base;
        for (int n = 2; QFileInfo::exists(inner); ++n)
            inner = folder + QLatin1Char('/') + base + QLatin1Char('-') + QString::number(n);
        const QFileInfoList items = QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        QDir().mkpath(inner);
        for (const QFileInfo &item : items)
            if (!QDir().rename(item.absoluteFilePath(), inner + QLatin1Char('/') + item.fileName()))
                return notAPackage(zipPath, QStringLiteral("%1 cannot be unpacked").arg(item.fileName()));
        return inner;
    }
    const QFileInfoList tops = QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (tops.size() != 1 || !QFileInfo::exists(tops.first().absoluteFilePath() + QStringLiteral("/workflow.fugue")))
        return notAPackage(zipPath, QStringLiteral("it holds no workflow.fugue in one folder"));
    return tops.first().absoluteFilePath();
}

} // namespace

Result<QString> unpackSharePackage(const QString &zipPath, const QString &folder) {
    Result<QString> top = unpackInto(zipPath, folder);
    if (!top)
        return top;
    // Each carried pack remembers the bundle it came in: the folder it is unpacked into is a scratch one, gone when
    // the app closes, so it is no source for `pack update` (#520). The archive cannot say this itself: dot files are skipped.
    const QString bundle = QFileInfo(zipPath).absoluteFilePath();
    const QDir packs(top.value() + QStringLiteral("/packs"));
    for (const QFileInfo &pack : packs.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        writeJsonFile(pack.absoluteFilePath() + QStringLiteral("/.fugue-source.json"),
                      QJsonObject{{"kind", "bundle"}, {"location", bundle}});
    return top;
}

} // namespace fugue
