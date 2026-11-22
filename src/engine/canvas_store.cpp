#include "canvas_store.h"

#include <algorithm>
#include <optional>
#include <set>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QUuid>

#include "engine/atomic_file.h"
#include "engine/canonical.h"
#include "engine/canvas_layer_ops.h"
#include "engine/hash.h"
#include "engine/paint_file.h"
#include "engine/paths.h"

namespace fugue::canvas {

using mask::LayerKind;
using mask::MaskPaint;
using mask::PaintDocument;

namespace {

Error corrupt(const QString &what) {
    return makeError(QStringLiteral("canvas_corrupt"), QStringLiteral("The canvas store is damaged: %1.").arg(what),
                     QStringLiteral("The files under the store folder are written once and never changed; restore them from a backup, or export the canvas from another copy."));
}

Error notFound(const QString &what) {
    return makeError(QStringLiteral("canvas_not_found"), QStringLiteral("There is no %1.").arg(what),
                     QStringLiteral("A canvas belongs to the project (or the Default) it was made in: open that one, or make one here with `fugue canvas new` or `fugue canvas import` (add `--project <name>` to put it in a project), which prints its id."));
}

QString kindName(LayerKind kind) {
    return kind == LayerKind::Mask ? QStringLiteral("mask") : kind == LayerKind::Picture ? QStringLiteral("picture") : QStringLiteral("paint");
}

bool kindFromName(const QString &name, LayerKind *kind) {
    if (name == QLatin1String("paint"))
        *kind = LayerKind::Paint;
    else if (name == QLatin1String("mask"))
        *kind = LayerKind::Mask;
    else if (name == QLatin1String("picture"))
        *kind = LayerKind::Picture;
    else
        return false;
    return true;
}

QString hexOf(const QString &hash) { return hash.mid(QStringLiteral("sha256:").size()); }

bool readFile(const QString &path, QByteArray *bytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *bytes = file.readAll();
    return true;
}

QByteArray jsonBytes(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Indented); }

// Writes an object once: an existing file of this name is the same content, since the name is its hash.
Errors writeOnce(const QString &path, const QByteArray &bytes, const QString &what) {
    if (QFileInfo::exists(path))
        return {};
    return replaceFile(path, bytes, what);
}

} // namespace

bool isCanvasId(const QString &text) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
    return pattern.match(text).hasMatch();
}

int Tree::tileCount() const {
    int count = 0;
    for (const LayerRecord &layer : layers)
        count += int(layer.tiles.size());
    return count;
}

QJsonObject treeToJson(const Tree &tree) {
    QJsonArray layers;
    for (const LayerRecord &layer : tree.layers) {
        QJsonObject tiles;
        for (const auto &[key, hash] : layer.tiles)
            tiles.insert(key.text(), hash);
        QJsonObject item{{"name", layer.name}, {"kind", kindName(layer.kind)}, {"visible", layer.visible}, {"opacity", layer.opacity}, {"tiles", tiles}};
        // Only what differs from the default, so a tree from before these existed has the same hash.
        if (!layer.compositeOp.isEmpty())
            item.insert("blend", layer.compositeOp);
        if (layer.hasMask) {
            QJsonObject maskTiles;
            for (const auto &[key, hash] : layer.maskTiles)
                maskTiles.insert(key.text(), hash);
            item.insert("mask", QJsonObject{{"enabled", layer.maskEnabled}, {"tiles", maskTiles}});
        }
        if (!layer.producer.isEmpty() || !layer.created.isEmpty())
            item.insert("origin", QJsonObject{{"producer", layer.producer}, {"time", layer.created}});
        layers.append(item);
    }
    return QJsonObject{{"format", storeFormat}, {"width", tree.width}, {"height", tree.height}, {"layers", layers}};
}

Result<Tree> treeFromJson(const QJsonObject &object) {
    if (object.value("format").toInt() != storeFormat)
        return corrupt(QStringLiteral("a tree has an unknown format"));
    Tree tree;
    tree.width = object.value("width").toInt();
    tree.height = object.value("height").toInt();
    if (tree.width < 1 || tree.height < 1 || qint64(tree.width) * tree.height > mask::paintFileMaxPixels)
        return corrupt(QStringLiteral("a tree has a size that cannot be"));
    for (const QJsonValue &value : object.value("layers").toArray()) {
        const QJsonObject item = value.toObject();
        LayerRecord layer;
        if (!kindFromName(item.value("kind").toString(), &layer.kind) || !item.value("name").isString() || !item.value("visible").isBool() ||
            !item.value("opacity").isDouble())
            return corrupt(QStringLiteral("a layer of a tree is not well formed"));
        layer.name = item.value("name").toString();
        layer.visible = item.value("visible").toBool();
        layer.opacity = item.value("opacity").toDouble();
        const QJsonObject tiles = item.value("tiles").toObject();
        if (layer.kind == LayerKind::Picture && !tiles.isEmpty())
            return corrupt(QStringLiteral("a picture layer has tiles"));
        const auto readTiles = [&tree](const QJsonObject &from, TileMap *to) {
            for (auto it = from.begin(); it != from.end(); ++it) {
                TileKey key;
                if (!TileKey::parse(it.key(), &key) || key.x >= tilesAcross(tree.width) || key.y >= tilesDown(tree.height) || !isSha256(it.value().toString()))
                    return false;
                to->emplace(key, it.value().toString());
            }
            return true;
        };
        if (!readTiles(tiles, &layer.tiles))
            return corrupt(QStringLiteral("a tile of a tree is not well formed"));
        if (item.contains("blend")) {
            layer.compositeOp = item.value("blend").toString();
            if (layer.compositeOp.isEmpty() || layer.compositeOp.size() > 64)
                return corrupt(QStringLiteral("a layer's blend is not well formed"));
        }
        if (item.contains("mask")) {
            const QJsonObject mask = item.value("mask").toObject();
            if (layer.kind == LayerKind::Mask || !mask.value("enabled").isBool() || !readTiles(mask.value("tiles").toObject(), &layer.maskTiles))
                return corrupt(QStringLiteral("a layer's mask is not well formed"));
            layer.hasMask = true;
            layer.maskEnabled = mask.value("enabled").toBool();
        }
        if (item.contains("origin")) {
            const QJsonObject origin = item.value("origin").toObject();
            layer.producer = origin.value("producer").toString();
            layer.created = origin.value("time").toString();
        }
        tree.layers.push_back(std::move(layer));
    }
    return tree;
}

Result<Tree> treeOf(const PaintDocument &document, const TileSink &consume) {
    Tree tree;
    tree.width = document.width();
    tree.height = document.height();
    for (int position = 0; position < document.layerCount(); ++position) {
        const mask::LayerInfo info = document.layer(position);
        LayerRecord layer;
        layer.name = info.name;
        layer.kind = info.kind;
        layer.visible = info.visible;
        layer.opacity = info.opacity;
        layer.compositeOp = info.compositeOp;
        layer.producer = info.producer;
        layer.created = info.created;
        if (const MaskPaint *mask = document.maskPixels(info.id)) {
            Result<TileMap> tiles = cutLayer(*mask, consume);
            if (!tiles)
                return Result<Tree>(tiles.errors());
            layer.hasMask = true;
            layer.maskEnabled = info.maskEnabled;
            layer.maskTiles = std::move(tiles.value());
        }
        if (info.kind != LayerKind::Picture) {
            const MaskPaint *pixels = document.pixels(info.id);
            if (!pixels)
                return corrupt(QStringLiteral("a layer has no pixels"));
            Result<TileMap> tiles = cutLayer(*pixels, consume);
            if (!tiles)
                return Result<Tree>(tiles.errors());
            layer.tiles = std::move(tiles.value());
        }
        tree.layers.push_back(std::move(layer));
    }
    return tree;
}

CanvasStore::CanvasStore(QString root, Clock clock) : m_root(QDir::cleanPath(std::move(root))), m_clock(std::move(clock)) {}

CanvasStore CanvasStore::forSession() { return CanvasStore(dataDir() + QStringLiteral("/canvases")); }

QString CanvasStore::canvasDir(const QString &canvasId) const { return m_root + QStringLiteral("/canvases/") + canvasId; }
QString CanvasStore::blobPath(const QString &hash) const {
    const QString hex = hexOf(hash);
    return m_root + QStringLiteral("/blobs/") + hex.left(2) + QLatin1Char('/') + hex + QStringLiteral(".png");
}
QString CanvasStore::treePath(const QString &hash) const { return m_root + QStringLiteral("/trees/") + hexOf(hash) + QStringLiteral(".json"); }

Errors CanvasStore::storeBlob(const QString &hash, const MaskPaint &tile) {
    const QString path = blobPath(hash);
    if (QFileInfo::exists(path))
        return {};
    const Result<QByteArray> png = tileToPng(tile);
    if (!png)
        return png.errors();
    return replaceFile(path, png.value(), QStringLiteral("a canvas tile"));
}

Result<MaskPaint> CanvasStore::readBlob(const QString &hash, int width, int height, int channels) const {
    QByteArray png;
    if (!readFile(blobPath(hash), &png))
        return corrupt(QStringLiteral("the tile %1 is missing").arg(hash));
    Result<MaskPaint> tile = tileFromPng(png, channels);
    if (!tile)
        return corrupt(QStringLiteral("the tile %1 does not decode").arg(hash));
    const QString actual = tileHash(tile->width(), tile->height(), tile->channels(), tileBytes(*tile, mask::IRect{0, 0, tile->width(), tile->height()}));
    if (actual != hash || tile->width() != width || tile->height() != height)
        return corrupt(QStringLiteral("the tile %1 does not hold the pixels it is named for").arg(hash));
    return tile;
}

Result<QString> CanvasStore::storeTree(const PaintDocument &document) {
    const Result<Tree> tree = treeOf(document, [this](const TileKey &, const QString &hash, const MaskPaint &tile) { return storeBlob(hash, tile); });
    if (!tree)
        return Result<QString>(tree.errors());
    return storeTreeRecord(tree.value());
}

Result<QString> CanvasStore::treeHashOf(const PaintDocument &document) {
    const Result<Tree> tree = treeOf(document);
    if (!tree)
        return Result<QString>(tree.errors());
    const QString hash = canonicalHash(treeToJson(tree.value()));
    if (hash.isEmpty())
        return corrupt(QStringLiteral("a tree has no canonical form"));
    return hash;
}

Result<Tree> CanvasStore::tree(const QString &treeHash) const {
    QByteArray bytes;
    if (!isSha256(treeHash) || !readFile(treePath(treeHash), &bytes))
        return notFound(QStringLiteral("stored tree %1").arg(treeHash));
    const QJsonObject json = QJsonDocument::fromJson(bytes).object();
    if (canonicalHash(json) != treeHash)
        return corrupt(QStringLiteral("the tree %1 is not the content it is named for").arg(treeHash));
    return treeFromJson(json);
}

Result<Revision> CanvasStore::addRevision(const QString &canvasId, const PaintDocument &document, const QString &parent, const QString &producer) {
    if (!isCanvasId(canvasId) || !QFileInfo::exists(canvasDir(canvasId) + QStringLiteral("/canvas.json")))
        return notFound(QStringLiteral("canvas %1").arg(canvasId));
    if (!parent.isEmpty() && !revision(canvasId, parent))
        return notFound(QStringLiteral("revision %1 in canvas %2").arg(parent, canvasId));
    const Result<QString> treeHash = storeTree(document);
    if (!treeHash)
        return Result<Revision>(treeHash.errors());
    return addCommit(canvasId, treeHash.value(), parent, producer);
}

Result<Revision> CanvasStore::addCommit(const QString &canvasId, const QString &treeHash, const QString &parent, const QString &producer) {
    Revision made;
    made.tree = treeHash;
    made.parent = parent;
    made.producer = producer;
    made.time = isoTime(utcNow(m_clock));
    const QJsonObject record{{"format", storeFormat},
                             {"tree", made.tree},
                             {"parent", parent.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(parent)},
                             {"producer", producer},
                             {"time", made.time}};
    made.id = canonicalHash(record);
    const Errors written = writeOnce(canvasDir(canvasId) + QStringLiteral("/commits/") + hexOf(made.id) + QStringLiteral(".json"), jsonBytes(record),
                                     QStringLiteral("a canvas revision"));
    if (hasErrors(written))
        return Result<Revision>(written);
    return made;
}

Result<QString> CanvasStore::storeTreeRecord(const Tree &tree) {
    const QJsonObject json = treeToJson(tree);
    const QString hash = canonicalHash(json);
    if (hash.isEmpty())
        return corrupt(QStringLiteral("a tree has no canonical form"));
    const Errors written = writeOnce(treePath(hash), jsonBytes(json), QStringLiteral("a canvas tree"));
    if (hasErrors(written))
        return Result<QString>(written);
    return hash;
}

Result<Revision> CanvasStore::revision(const QString &canvasId, const QString &revisionId) const {
    QByteArray bytes;
    if (!isCanvasId(canvasId) || !isSha256(revisionId) ||
        !readFile(canvasDir(canvasId) + QStringLiteral("/commits/") + hexOf(revisionId) + QStringLiteral(".json"), &bytes))
        return notFound(QStringLiteral("revision %1 in canvas %2").arg(revisionId, canvasId));
    const QJsonObject json = QJsonDocument::fromJson(bytes).object();
    if (canonicalHash(json) != revisionId || json.value("format").toInt() != storeFormat || !isSha256(json.value("tree").toString()) ||
        !json.value("producer").isString() || !json.value("time").isString() || !(json.value("parent").isNull() || isSha256(json.value("parent").toString())))
        return corrupt(QStringLiteral("the revision %1 is not the record it is named for").arg(revisionId));
    Revision found;
    found.id = revisionId;
    found.tree = json.value("tree").toString();
    found.parent = json.value("parent").toString();
    found.producer = json.value("producer").toString();
    found.time = json.value("time").toString();
    return found;
}

Result<CanvasInfo> CanvasStore::info(const QString &canvasId) const {
    QByteArray bytes;
    if (!isCanvasId(canvasId) || !readFile(canvasDir(canvasId) + QStringLiteral("/canvas.json"), &bytes))
        return notFound(QStringLiteral("canvas %1").arg(canvasId));
    const QJsonObject json = QJsonDocument::fromJson(bytes).object();
    if (json.value("id").toString() != canvasId)
        return corrupt(QStringLiteral("canvas.json of %1 names another canvas").arg(canvasId));
    CanvasInfo found;
    found.id = canvasId;
    found.name = json.value("name").toString();
    found.created = json.value("created").toString();
    QByteArray head;
    if (readFile(canvasDir(canvasId) + QStringLiteral("/head"), &head))
        found.head = QString::fromLatin1(head).trimmed();
    return found;
}

std::vector<CanvasInfo> CanvasStore::list() const {
    std::vector<CanvasInfo> found;
    const QDir folder(m_root + QStringLiteral("/canvases"));
    for (const QString &name : folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const Result<CanvasInfo> canvas = info(name);
        if (canvas)
            found.push_back(canvas.value());
    }
    std::sort(found.begin(), found.end(), [](const CanvasInfo &a, const CanvasInfo &b) { return a.created != b.created ? a.created > b.created : a.id < b.id; });
    return found;
}

Result<HeadMove> CanvasStore::moveHead(const QString &canvasId, const QString &expected, const QString &revisionId) {
    const Result<CanvasInfo> canvas = info(canvasId);
    if (!canvas)
        return Result<HeadMove>(canvas.errors());
    if (!revision(canvasId, revisionId))
        return notFound(QStringLiteral("revision %1 in canvas %2").arg(revisionId, canvasId));
    QLockFile lock(canvasDir(canvasId) + QStringLiteral("/head.lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(10000))
        return makeError(QStringLiteral("node_failed"), QStringLiteral("Another process holds the lock of canvas %1.").arg(canvasId));
    QByteArray head; // read under the lock
    readFile(canvasDir(canvasId) + QStringLiteral("/head"), &head);
    const QString current = QString::fromLatin1(head).trimmed();
    if (current != expected)
        return HeadMove{false, current};
    const Errors written = replaceFile(canvasDir(canvasId) + QStringLiteral("/head"), revisionId.toLatin1() + '\n', QStringLiteral("the canvas head"));
    if (hasErrors(written))
        return Result<HeadMove>(written);
    return HeadMove{true, revisionId};
}

Result<Commit> CanvasStore::commit(const QString &canvasId, const PaintDocument &document, const QString &expectedHead, const QString &producer) {
    const Result<Revision> added = addRevision(canvasId, document, expectedHead, producer);
    if (!added)
        return Result<Commit>(added.errors());
    const Result<HeadMove> moved = moveHead(canvasId, expectedHead, added->id);
    if (!moved)
        return Result<Commit>(moved.errors());
    return Commit{added.value(), moved.value()};
}

Result<CanvasInfo> CanvasStore::create(const QString &name, const PaintDocument &document, const QString &producer) {
    const QString id = QStringLiteral("canvas-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
    const QString dir = canvasDir(id);
    const Errors folder = makeFolder(dir + QStringLiteral("/commits"));
    if (hasErrors(folder))
        return Result<CanvasInfo>(folder);
    const auto fail = [&dir](const Errors &errors) {
        QDir(dir).removeRecursively(); // a canvas that was never complete: only this call knew it
        return Result<CanvasInfo>(errors);
    };
    const QJsonObject record{{"format", storeFormat}, {"id", id}, {"name", name}, {"created", isoTime(utcNow(m_clock))}};
    const Errors written = replaceFile(dir + QStringLiteral("/canvas.json"), jsonBytes(record), QStringLiteral("canvas.json"));
    if (hasErrors(written))
        return fail(written);
    const Result<Commit> first = commit(id, document, QString(), producer);
    if (!first || !first->move.moved)
        return fail(first ? Errors{makeError(QStringLiteral("node_failed"), QStringLiteral("The new canvas's head could not be set."))} : first.errors());
    return info(id);
}

Result<QString> CanvasStore::resolve(const QString &canvasId, const QString &ref) const {
    const Result<CanvasInfo> canvas = info(canvasId);
    if (!canvas)
        return Result<QString>(canvas.errors());
    if (ref.isEmpty() || ref == QLatin1String("head")) {
        if (canvas->head.isEmpty())
            return notFound(QStringLiteral("head: canvas %1 has no revision yet").arg(canvasId));
        return canvas->head;
    }
    if (isSha256(ref))
        return revision(canvasId, ref) ? Result<QString>(ref) : Result<QString>(notFound(QStringLiteral("revision %1 in canvas %2").arg(ref, canvasId)));
    const QString prefix = ref.startsWith(QLatin1String("sha256:")) ? hexOf(ref) : ref;
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{8,63}$"));
    if (!hex.match(prefix).hasMatch())
        return notFound(QStringLiteral("revision %1: write \"head\", a full hash, or at least 8 hex digits of one").arg(ref));
    QStringList found;
    QDirIterator it(canvasDir(canvasId) + QStringLiteral("/commits"), {prefix + QLatin1Char('*') + QStringLiteral(".json")}, QDir::Files);
    while (it.hasNext())
        found << QFileInfo(it.next()).completeBaseName();
    if (found.size() == 1)
        return QStringLiteral("sha256:") + found.first();
    return found.isEmpty() ? Result<QString>(notFound(QStringLiteral("revision starting %1 in canvas %2").arg(ref, canvasId)))
                           : Result<QString>(makeError(QStringLiteral("canvas_ambiguous"), QStringLiteral("%1 starts more than one revision of canvas %2.").arg(ref, canvasId),
                                                       QStringLiteral("Write more of the hash.")));
}

Result<std::vector<Revision>> CanvasStore::log(const QString &canvasId, const QString &from, int limit) const {
    const Result<QString> start = resolve(canvasId, from);
    if (!start)
        return Result<std::vector<Revision>>(start.errors());
    std::vector<Revision> entries;
    QString at = start.value();
    while (!at.isEmpty() && int(entries.size()) < limit) {
        const Result<Revision> found = revision(canvasId, at);
        if (!found)
            return Result<std::vector<Revision>>(found.errors());
        entries.push_back(found.value());
        at = found->parent;
    }
    return entries;
}

QString CanvasStore::resolvedPath(const QString &canvasId, const QString &revisionId) const {
    return canvasDir(canvasId) + QStringLiteral("/resolved/") + hexOf(revisionId);
}

QString CanvasStore::resolution(const QString &canvasId, const QString &revisionId) const {
    QByteArray bytes;
    if (!readFile(resolvedPath(canvasId, revisionId), &bytes))
        return {};
    return QString::fromLatin1(bytes).split(QLatin1Char(' ')).value(0).trimmed();
}

// Every revision hash in the head's history (the head itself too), and the head.
Result<QStringList> CanvasStore::historyOfHead(const QString &canvasId, QString *head) const {
    const Result<CanvasInfo> canvas = info(canvasId);
    if (!canvas)
        return Result<QStringList>(canvas.errors());
    *head = canvas->head;
    QStringList seen;
    QString at = canvas->head;
    while (!at.isEmpty()) {
        const Result<Revision> found = revision(canvasId, at);
        if (!found)
            return Result<QStringList>(found.errors());
        seen << at;
        at = found->parent;
    }
    return seen;
}

Result<std::vector<Candidate>> CanvasStore::candidates(const QString &canvasId, bool withDropped) const {
    QString head;
    const Result<QStringList> history = historyOfHead(canvasId, &head);
    if (!history)
        return Result<std::vector<Candidate>>(history.errors());
    std::vector<Candidate> found;
    QDirIterator it(canvasDir(canvasId) + QStringLiteral("/commits"), {QStringLiteral("*.json")}, QDir::Files);
    while (it.hasNext()) {
        const QString id = QStringLiteral("sha256:") + QFileInfo(it.next()).completeBaseName();
        if (history->contains(id))
            continue;
        const QString state = resolution(canvasId, id);
        if (state == QLatin1String("accepted") || state == QLatin1String("undone") || (state == QLatin1String("dropped") && !withDropped))
            continue;
        const Result<Revision> record = revision(canvasId, id);
        if (!record)
            return Result<std::vector<Candidate>>(record.errors());
        found.push_back({record.value(), record->parent == head, state == QLatin1String("dropped")});
    }
    std::sort(found.begin(), found.end(), [](const Candidate &a, const Candidate &b) {
        return a.revision.time != b.revision.time ? a.revision.time > b.revision.time : a.revision.id < b.revision.id;
    });
    return found;
}

namespace {

Error notCandidate(const QString &revisionId, const QString &why) {
    return makeError(QStringLiteral("canvas_not_candidate"), QStringLiteral("Revision %1 is %2, so it is not a candidate.").arg(revisionId.left(19), why),
                     QStringLiteral("`fugue canvas candidates <canvas>` lists the revisions that can be accepted or dropped."));
}

bool sameLayer(const LayerRecord &a, const LayerRecord &b) { return a == b; }

} // namespace

Result<Accepted> CanvasStore::accept(const QString &canvasId, const QString &revisionId) {
    const Result<Revision> candidate = revision(canvasId, revisionId);
    if (!candidate)
        return Result<Accepted>(candidate.errors());
    QString head;
    const Result<QStringList> history = historyOfHead(canvasId, &head);
    if (!history)
        return Result<Accepted>(history.errors());
    if (history->contains(revisionId))
        return Result<Accepted>(notCandidate(revisionId, QStringLiteral("already in the canvas's history")));
    if (resolution(canvasId, revisionId) == QLatin1String("accepted"))
        return Result<Accepted>(notCandidate(revisionId, QStringLiteral("already accepted")));

    Accepted done;
    if (candidate->parent == head) {
        const Result<HeadMove> moved = moveHead(canvasId, head, revisionId);
        if (!moved)
            return Result<Accepted>(moved.errors());
        done.moved = moved->moved;
        done.current = moved->head;
        done.head = candidate.value();
    } else {
        // Made from an older revision: place the layers it added on the head, when that is all it did.
        const Result<Revision> parent = candidate->parent.isEmpty() ? Result<Revision>(notFound(QStringLiteral("parent of revision %1").arg(revisionId.left(19))))
                                                                    : revision(canvasId, candidate->parent);
        const Result<Revision> current = head.isEmpty() ? Result<Revision>(notFound(QStringLiteral("head of canvas %1").arg(canvasId))) : revision(canvasId, head);
        if (!parent || !current)
            return Result<Accepted>(!parent ? parent.errors() : current.errors());
        const Result<Tree> before = tree(parent->tree), after = tree(candidate->tree), now = tree(current->tree);
        for (const Errors &errors : {before.errors(), after.errors(), now.errors()})
            if (hasErrors(errors))
                return Result<Accepted>(errors);
        const auto conflict = [&](const QString &why) {
            return Result<Accepted>(makeError(QStringLiteral("canvas_conflict"),
                                              QStringLiteral("Revision %1 was made from an older revision than the head and %2, so it cannot be placed on the head.").arg(revisionId.left(19), why),
                                              QStringLiteral("Drop it and run the fill again from the canvas as it is now.")));
        };
        if (before->width != now->width || before->height != now->height || after->width != now->width || after->height != now->height)
            return conflict(QStringLiteral("the canvas has another size now"));
        if (after->layers.size() <= before->layers.size())
            return conflict(QStringLiteral("it added no layer"));
        for (size_t i = 0; i < before->layers.size(); ++i)
            if (!sameLayer(before->layers[i], after->layers[i]))
                return conflict(QStringLiteral("it changed the layer %1").arg(before->layers[i].name));
        const Result<PaintDocument> candidateDocument = load(canvasId, revisionId);
        Result<PaintDocument> onHead = load(canvasId, head);
        if (!candidateDocument || !onHead)
            return Result<Accepted>(!candidateDocument ? candidateDocument.errors() : onHead.errors());
        for (int i = int(before->layers.size()); i < candidateDocument->layerCount(); ++i) {
            const mask::LayerInfo layer = candidateDocument->layer(i);
            const MaskPaint *pixels = candidateDocument->pixels(layer.id);
            if (!pixels)
                return Result<Accepted>(corrupt(QStringLiteral("a layer has no pixels")));
            onHead->loadLayer(layer, *pixels, candidateDocument->maskPixels(layer.id));
        }
        const Result<Commit> made = commit(canvasId, onHead.value(), head, QStringLiteral("accept:") + revisionId.left(19));
        if (!made)
            return Result<Accepted>(made.errors());
        done.rebased = true;
        done.moved = made->move.moved;
        done.current = made->move.head;
        done.head = made->revision;
    }
    if (done.moved) {
        const Errors marked = replaceFile(resolvedPath(canvasId, revisionId), QStringLiteral("accepted %1\n").arg(done.head.id).toLatin1(), QStringLiteral("a canvas resolution"));
        if (hasErrors(marked))
            return Result<Accepted>(marked);
    }
    return done;
}

Result<Revision> CanvasStore::drop(const QString &canvasId, const QString &revisionId) {
    const Result<Revision> candidate = revision(canvasId, revisionId);
    if (!candidate)
        return Result<Revision>(candidate.errors());
    QString head;
    const Result<QStringList> history = historyOfHead(canvasId, &head);
    if (!history)
        return Result<Revision>(history.errors());
    if (history->contains(revisionId))
        return Result<Revision>(notCandidate(revisionId, QStringLiteral("in the canvas's history")));
    const QString state = resolution(canvasId, revisionId);
    if (state == QLatin1String("accepted"))
        return Result<Revision>(notCandidate(revisionId, QStringLiteral("already accepted")));
    if (state != QLatin1String("dropped")) {
        const Errors marked = replaceFile(resolvedPath(canvasId, revisionId), QByteArrayLiteral("dropped\n"), QStringLiteral("a canvas resolution"));
        if (hasErrors(marked))
            return Result<Revision>(marked);
    }
    return candidate.value();
}

namespace {

Error noStep(const QString &what) {
    return makeError(QStringLiteral("canvas_no_step"), what,
                     QStringLiteral("`fugue canvas log <canvas>` shows the revisions; `fugue canvas accept` or a revision hash names another."));
}

// Every tile a tree refers to, layer and mask, for checking that an edit added none.
std::set<QString> tilesOf(const Tree &tree) {
    std::set<QString> all;
    for (const LayerRecord &layer : tree.layers) {
        for (const auto &entry : layer.tiles)
            all.insert(entry.second);
        for (const auto &entry : layer.maskTiles)
            all.insert(entry.second);
    }
    return all;
}

} // namespace

Result<Commit> CanvasStore::editLayers(const QString &canvasId, const QString &base, const LayerEdit &edit, const QString &producer) {
    const Result<CanvasInfo> canvas = info(canvasId);
    if (!canvas)
        return Result<Commit>(canvas.errors());
    const Result<Revision> from = revision(canvasId, base);
    if (!from)
        return Result<Commit>(from.errors());
    if (canvas->head != base)
        return Commit{from.value(), HeadMove{false, canvas->head}};
    const Result<Tree> before = tree(from->tree);
    if (!before)
        return Result<Commit>(before.errors());
    const Result<Tree> after = applyLayerEdit(before.value(), edit);
    if (!after)
        return Result<Commit>(after.errors());
    if (treeToJson(after.value()) == treeToJson(before.value()))
        return Commit{from.value(), HeadMove{false, base}};
    const std::set<QString> had = tilesOf(before.value()), has = tilesOf(after.value());
    if (!std::includes(had.begin(), had.end(), has.begin(), has.end()))
        return corrupt(QStringLiteral("a layer edit made tiles of its own")); // cannot be: an edit moves records, never pixels
    const Result<QString> treeHash = storeTreeRecord(after.value());
    if (!treeHash)
        return Result<Commit>(treeHash.errors());
    const Result<Revision> added = addCommit(canvasId, treeHash.value(), base, producer);
    if (!added)
        return Result<Commit>(added.errors());
    const Result<HeadMove> moved = moveHead(canvasId, base, added->id);
    if (!moved)
        return Result<Commit>(moved.errors());
    if (!moved->moved) // lost a race with a run: not a candidate for anyone to review, an edit that did not happen
        (void)replaceFile(resolvedPath(canvasId, added->id), QByteArrayLiteral("undone\n"), QStringLiteral("a canvas resolution"));
    return Commit{added.value(), moved.value()};
}

Result<HeadMove> CanvasStore::undo(const QString &canvasId, const QString &expected) {
    const Result<CanvasInfo> canvas = info(canvasId);
    if (!canvas)
        return Result<HeadMove>(canvas.errors());
    if (canvas->head != expected)
        return HeadMove{false, canvas->head};
    const Result<Revision> at = revision(canvasId, expected);
    if (!at)
        return Result<HeadMove>(at.errors());
    if (at->parent.isEmpty())
        return Result<HeadMove>(noStep(QStringLiteral("Revision %1 is the canvas's first: there is nothing to undo.").arg(expected.left(19))));
    const Result<HeadMove> moved = moveHead(canvasId, expected, at->parent);
    if (!moved || !moved->moved)
        return moved;
    const Errors marked = replaceFile(resolvedPath(canvasId, expected), QByteArrayLiteral("undone\n"), QStringLiteral("a canvas resolution"));
    if (hasErrors(marked))
        return Result<HeadMove>(marked);
    return moved;
}

Result<QString> CanvasStore::redoTarget(const QString &canvasId) const {
    const Result<CanvasInfo> canvas = info(canvasId);
    if (!canvas)
        return Result<QString>(canvas.errors());
    Revision best;
    QDirIterator it(canvasDir(canvasId) + QStringLiteral("/resolved"), QDir::Files);
    while (it.hasNext()) {
        const QString id = QStringLiteral("sha256:") + QFileInfo(it.next()).fileName();
        if (resolution(canvasId, id) != QLatin1String("undone"))
            continue;
        const Result<Revision> found = revision(canvasId, id);
        if (!found || found->parent != canvas->head || found->parent.isEmpty())
            continue;
        if (best.id.isEmpty() || found->time > best.time || (found->time == best.time && found->id > best.id))
            best = found.value();
    }
    return best.id;
}

Result<HeadMove> CanvasStore::redo(const QString &canvasId, const QString &expected) {
    const Result<QString> target = redoTarget(canvasId);
    if (!target)
        return Result<HeadMove>(target.errors());
    const Result<CanvasInfo> canvas = info(canvasId);
    if (!canvas)
        return Result<HeadMove>(canvas.errors());
    if (canvas->head != expected)
        return HeadMove{false, canvas->head};
    if (target->isEmpty())
        return Result<HeadMove>(noStep(QStringLiteral("Nothing was undone from revision %1: there is nothing to redo.").arg(expected.left(19))));
    return moveHead(canvasId, expected, target.value());
}

Result<PaintDocument> CanvasStore::load(const QString &canvasId, const QString &revisionId) const {
    const Result<Revision> found = revision(canvasId, revisionId);
    if (!found)
        return Result<PaintDocument>(found.errors());
    const Result<Tree> stored = tree(found->tree);
    if (!stored)
        return Result<PaintDocument>(stored.errors());
    return documentOf(stored.value(), [this](const QString &hash, int width, int height, int channels) { return readBlob(hash, width, height, channels); });
}

Result<PaintDocument> documentOf(const Tree &stored, const std::function<Result<MaskPaint>(const QString &, int, int, int)> &read) {
    PaintDocument document(stored.width, stored.height);
    const int width = stored.width, height = stored.height;
    const auto assemble = [&](int channels, const TileMap &tiles) {
        return assembleLayer(width, height, channels, tiles, [&](const TileKey &key, const QString &hash) {
            const mask::IRect rect = tileRect(key, width, height);
            return read(hash, rect.width, rect.height, channels);
        });
    };
    for (const LayerRecord &layer : stored.layers) {
        mask::LayerInfo info;
        info.name = layer.name;
        info.kind = layer.kind;
        info.visible = layer.visible;
        info.opacity = layer.opacity;
        info.compositeOp = layer.compositeOp;
        info.maskEnabled = layer.maskEnabled;
        info.producer = layer.producer;
        info.created = layer.created;
        std::optional<MaskPaint> mask;
        if (layer.hasMask) {
            Result<MaskPaint> read = assemble(1, layer.maskTiles);
            if (!read)
                return Result<PaintDocument>(read.errors());
            mask = std::move(read.value());
        }
        if (layer.kind == LayerKind::Picture) {
            document.loadLayer(info, MaskPaint(1, 1, 4), mask ? &*mask : nullptr);
            continue;
        }
        const Result<MaskPaint> pixels = assemble(layer.channels(), layer.tiles);
        if (!pixels)
            return Result<PaintDocument>(pixels.errors());
        document.loadLayer(info, pixels.value(), mask ? &*mask : nullptr);
    }
    document.forgetHistory();
    return document;
}

} // namespace fugue::canvas
