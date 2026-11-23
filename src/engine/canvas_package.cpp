#include "canvas_package.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <archive.h>
#include <archive_entry.h>

#include <memory>

#include "engine/canonical.h"
#include "engine/hash.h"
#include "engine/zip_archive.h"

namespace fugue::canvas {

namespace {

constexpr int packageFormat = 1;
constexpr qint64 kMaxPacked = qint64(3) << 30;
constexpr int kMaxRevisions = 5000;
// What the unpacking accepts: a tile PNG is at most a few hundred KB, a tree some MB; the count is the tiles of a long history.
constexpr UnpackLimits kLimits{qint64(64) << 20, qint64(4) << 30, 200000};

QString hexOf(const QString &hash) { return hash.mid(QStringLiteral("sha256:").size()); }

Error corrupt(const QString &why) { return makeError(QStringLiteral("canvas_corrupt"), why); }

// The folder name in the zip: the canvas's name in lowercase letters, digits and dashes.
QString folderOf(const QString &name) {
    static const QRegularExpression other(QStringLiteral("[^a-z0-9]+"));
    QString id = name.toLower().replace(other, QStringLiteral("-"));
    while (id.startsWith(QLatin1Char('-')))
        id.remove(0, 1);
    while (id.endsWith(QLatin1Char('-')))
        id.chop(1);
    return id.isEmpty() ? QStringLiteral("canvas") : id.left(64);
}

// The tiles of a tree with the size and channels each must have.
struct TileUse {
    QString hash;
    int width, height, channels;
};
std::vector<TileUse> tilesOf(const Tree &tree) {
    std::vector<TileUse> uses;
    const auto add = [&](const TileMap &tiles, int channels) {
        for (const auto &[key, hash] : tiles) {
            const mask::IRect rect = tileRect(key, tree.width, tree.height);
            uses.push_back({hash, rect.width, rect.height, channels});
        }
    };
    for (const LayerRecord &layer : tree.layers) {
        add(layer.tiles, layer.channels());
        add(layer.maskTiles, 1);
    }
    return uses;
}

QByteArray jsonBytes(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Indented); }

bool readAll(const QString &path, qint64 max, QByteArray *bytes) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() > max)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *bytes = file.readAll();
    return true;
}

// The width and height a PNG says in its header (IHDR), read before a decoder is given the bytes.
bool pngSize(const QByteArray &png, int *width, int *height) {
    static const QByteArray signature("\x89PNG\r\n\x1a\n", 8);
    if (png.size() < 33 || !png.startsWith(signature) || png.mid(12, 4) != QByteArray("IHDR"))
        return false;
    const auto be32 = [&png](int at) {
        return (quint32(quint8(png[at])) << 24) | (quint32(quint8(png[at + 1])) << 16) | (quint32(quint8(png[at + 2])) << 8) | quint32(quint8(png[at + 3]));
    };
    *width = int(be32(16));
    *height = int(be32(20));
    return be32(16) < (1u << 20) && be32(20) < (1u << 20);
}

} // namespace

Errors writeCanvasPackage(const CanvasStore &store, const QString &canvasId, SharedPart part, const QString &zipPath) {
    const Result<CanvasInfo> info = store.info(canvasId);
    if (!info)
        return info.errors();
    if (info->head.isEmpty())
        return {makeError(QStringLiteral("canvas_not_found"), QStringLiteral("Canvas %1 has no revision to share.").arg(canvasId))};
    // The head, or the head and what it came from, oldest first.
    std::vector<Revision> chain;
    if (part == SharedPart::History) {
        const Result<std::vector<Revision>> log = store.log(canvasId, info->head, kMaxRevisions + 1);
        if (!log)
            return log.errors();
        if (int(log->size()) > kMaxRevisions)
            return {makeError(QStringLiteral("value_out_of_range"), QStringLiteral("The history has more than %1 revisions.").arg(kMaxRevisions),
                              QStringLiteral("Share the head only."))};
        chain.assign(log->rbegin(), log->rend());
    } else {
        const Result<Revision> head = store.revision(canvasId, info->head);
        if (!head)
            return head.errors();
        // The head with its parent cut: a new first revision, so it is its own record with its own id.
        chain.push_back(CanvasStore::makeRevision(head->tree, QString(), head->producer, head->time));
    }

    const QString folder = folderOf(info->name);
    QList<ZipEntry> entries;
    QJsonArray revisions;
    QSet<QString> trees, tiles;
    qint64 total = 0;
    for (const Revision &revision : chain) {
        revisions.append(QJsonObject{{"id", revision.id}, {"tree", revision.tree}, {"parent", revision.parent}, {"producer", revision.producer}, {"time", revision.time}});
        if (trees.contains(revision.tree))
            continue;
        trees.insert(revision.tree);
        const Result<Tree> tree = store.tree(revision.tree); // checked against its name
        if (!tree)
            return tree.errors();
        const QByteArray bytes = jsonBytes(treeToJson(tree.value()));
        entries << ZipEntry{folder + QStringLiteral("/trees/") + hexOf(revision.tree) + QStringLiteral(".json"), bytes, {}};
        total += bytes.size();
        for (const TileUse &use : tilesOf(tree.value())) {
            if (tiles.contains(use.hash))
                continue;
            tiles.insert(use.hash);
            // Read and checked once (a tile that is not its name makes the whole share fail), then the stored file goes in as it is.
            const Result<mask::MaskPaint> tile = store.tilePixels(use.hash, use.width, use.height, use.channels);
            if (!tile)
                return tile.errors();
            const QString file = store.blobFile(use.hash);
            total += QFileInfo(file).size();
            entries << ZipEntry{folder + QStringLiteral("/tiles/") + hexOf(use.hash) + QStringLiteral(".png"), {}, file};
        }
        if (total > kMaxPacked)
            return {makeError(QStringLiteral("value_out_of_range"), QStringLiteral("The canvas comes to %1 GB; a package holds at most %2 GB.").arg(double(total) / (1 << 30), 0, 'f', 1).arg(kMaxPacked >> 30),
                              QStringLiteral("Share the head only, or resize the canvas."))};
    }
    const QJsonObject manifest{{"format", packageFormat},
                               {"kind", "canvas"},
                               {"id", canvasId},
                               {"name", info->name},
                               {"mode", part == SharedPart::History ? "history" : "head"},
                               {"revisions", revisions}};
    entries.prepend(ZipEntry{folder + QStringLiteral("/canvas.json"), jsonBytes(manifest), {}});
    return writeZipFile(entries, zipPath);
}

bool isCanvasPackage(const QString &path) {
    std::unique_ptr<archive, decltype(&archive_read_free)> in(archive_read_new(), archive_read_free);
    archive_read_support_format_zip(in.get());
    if (archive_read_open_filename(in.get(), QFile::encodeName(path).constData(), 64 * 1024) != ARCHIVE_OK)
        return false;
    archive_entry *entry = nullptr;
    for (int seen = 0; seen < 1000 && archive_read_next_header(in.get(), &entry) == ARCHIVE_OK; ++seen) {
        const char *name = archive_entry_pathname(entry);
        const QStringList parts = QString::fromUtf8(name ? name : "").split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (!parts.isEmpty() && parts.size() <= 2 && parts.last() == QLatin1String("canvas.json"))
            return true;
    }
    return false;
}

Result<CanvasImported> importCanvasPackage(const QString &zipPath, CanvasStore &store, const QString &name) {
    // Unpacked inside the store's root and nowhere else; removed when this returns.
    if (!QDir().mkpath(store.root()))
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Fugue could not make the folder %1.").arg(store.root()));
    QTemporaryDir unpacked(store.root() + QStringLiteral("/import-XXXXXX"));
    if (!unpacked.isValid())
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Fugue could not unpack %1: %2.").arg(QFileInfo(zipPath).fileName(), unpacked.errorString()));
    if (const Errors problems = unpackZip(zipPath, unpacked.path(), kLimits); hasErrors(problems))
        return Result<CanvasImported>(problems);

    // The folder with canvas.json: the top, or the one folder at the top.
    QString folder = unpacked.path();
    if (!QFileInfo::exists(folder + QStringLiteral("/canvas.json"))) {
        const QFileInfoList tops = QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        if (tops.size() != 1 || !QFileInfo::exists(tops.first().absoluteFilePath() + QStringLiteral("/canvas.json")))
            return notAPackage(zipPath, QStringLiteral("it holds no canvas.json in one folder"));
        folder = tops.first().absoluteFilePath();
    }
    QByteArray manifestBytes;
    if (!readAll(folder + QStringLiteral("/canvas.json"), kLimits.maxFile, &manifestBytes))
        return notAPackage(zipPath, QStringLiteral("canvas.json cannot be read"));
    QJsonParseError parse;
    const QJsonObject manifest = QJsonDocument::fromJson(manifestBytes, &parse).object();
    const QString mode = manifest.value("mode").toString();
    const QJsonArray listed = manifest.value("revisions").toArray();
    if (parse.error != QJsonParseError::NoError || manifest.value("kind").toString() != QLatin1String("canvas"))
        return notAPackage(zipPath, QStringLiteral("canvas.json is not a canvas"));
    if (manifest.value("format").toInt() != packageFormat)
        return notAPackage(zipPath, QStringLiteral("it was made by a newer Fugue; update Fugue to open it"));
    if ((mode != QLatin1String("head") && mode != QLatin1String("history")) || listed.isEmpty() || listed.size() > kMaxRevisions ||
        (mode == QLatin1String("head") && listed.size() != 1))
        return notAPackage(zipPath, QStringLiteral("its revisions are not well formed"));
    const QString shownName = name.trimmed().isEmpty() ? manifest.value("name").toString() : name;

    std::vector<ImportedRevision> chain;
    QHash<QString, Tree> trees;
    for (const QJsonValue &value : listed) {
        const QJsonObject item = value.toObject();
        ImportedRevision made;
        made.revision.id = item.value("id").toString();
        made.revision.tree = item.value("tree").toString();
        made.revision.parent = item.value("parent").toString();
        made.revision.producer = item.value("producer").toString();
        made.revision.time = item.value("time").toString();
        // The name of the file read is made here from a checked hash, never taken from the package.
        if (!isSha256(made.revision.tree))
            return notAPackage(zipPath, QStringLiteral("a revision names a tree that is not a hash"));
        if (!trees.contains(made.revision.tree)) {
            QByteArray bytes;
            if (!readAll(folder + QStringLiteral("/trees/") + hexOf(made.revision.tree) + QStringLiteral(".json"), kLimits.maxFile, &bytes))
                return notAPackage(zipPath, QStringLiteral("the tree %1 is missing").arg(made.revision.tree));
            const QJsonObject json = QJsonDocument::fromJson(bytes).object();
            if (canonicalHash(json) != made.revision.tree)
                return corrupt(QStringLiteral("the tree %1 is not the content it is named for").arg(made.revision.tree));
            const Result<Tree> tree = treeFromJson(json);
            if (!tree)
                return Result<CanvasImported>(tree.errors());
            trees.insert(made.revision.tree, tree.value());
        }
        made.tree = trees.value(made.revision.tree);
        chain.push_back(std::move(made));
    }

    const TileFetch fetch = [&folder](const QString &hash, int width, int height, int channels) -> Result<mask::MaskPaint> {
        QByteArray png;
        int wide = 0, high = 0;
        if (!isSha256(hash) || !readAll(folder + QStringLiteral("/tiles/") + hexOf(hash) + QStringLiteral(".png"), kLimits.maxFile, &png))
            return corrupt(QStringLiteral("the tile %1 is missing").arg(hash));
        if (!pngSize(png, &wide, &high) || wide != width || high != height)
            return corrupt(QStringLiteral("the tile %1 is not the size its place gives").arg(hash));
        return tileFromPng(png, channels);
    };
    const Result<CanvasInfo> made = store.importCanvas(manifest.value("id").toString(), shownName, chain, fetch);
    if (!made)
        return Result<CanvasImported>(made.errors());
    CanvasImported done;
    done.info = made.value();
    done.packagedId = manifest.value("id").toString();
    done.renamedId = done.info.id != done.packagedId;
    done.part = mode == QLatin1String("history") ? SharedPart::History : SharedPart::Head;
    done.revisions = int(chain.size());
    return done;
}

} // namespace fugue::canvas
