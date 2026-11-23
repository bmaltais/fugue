#include "engine/canvas_batch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>

#include <algorithm>

#include "engine/atomic_file.h"
#include "engine/canvas_router.h"
#include "engine/paint_file.h"
#include "engine/registry.h"

namespace fugue::canvas {

namespace {

const QStringList &pictureSuffixes() {
    static const QStringList suffixes{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("webp"),
                                      QStringLiteral("bmp"), QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("ora")};
    return suffixes;
}

} // namespace

Result<BatchAction> loadBatchAction(const QString &ref, const QStringList &roots, const Registry &registry) {
    QString file;
    if (QFileInfo(ref).isFile()) {
        file = QFileInfo(ref).absoluteFilePath();
    } else {
        for (const QString &root : roots)
            if (QFileInfo::exists(root + QLatin1Char('/') + ref + QStringLiteral(".fugue"))) {
                file = root + QLatin1Char('/') + ref + QStringLiteral(".fugue");
                break;
            }
    }
    if (file.isEmpty())
        return makeError(QStringLiteral("file_not_found"), QStringLiteral("There is no action %1.").arg(ref),
                         QStringLiteral("Name an action file, or the id of an action (the file name without .fugue)."));
    Result<Document> document = Document::loadFile(file);
    if (!document)
        return document.errors();
    const Result<std::optional<Action>> action = actionOf(document.value());
    if (!action)
        return action.errors();
    if (!action.value())
        return makeError(QStringLiteral("action_invalid"), QStringLiteral("%1 is a workflow with no `action` block, so it is not a canvas action.").arg(QFileInfo(file).fileName()));
    const Result<ActionGraph> graph = checkAction(*action.value(), document.value(), registry);
    if (!graph)
        return graph.errors();
    if (action.value()->selection == QLatin1String("bounds"))
        return makeError(QStringLiteral("action_invalid"), QStringLiteral("%1 works on a crop frame beyond the canvas, so it cannot run over a folder.").arg(action.value()->name));
    return BatchAction{QFileInfo(file).completeBaseName(), file, document.value(), *action.value(), graph.value()};
}

Result<QStringList> batchPictures(const QString &folder) {
    const QDir dir(folder);
    if (!dir.exists())
        return makeError(QStringLiteral("file_not_found"), QStringLiteral("The folder %1 does not exist.").arg(folder));
    QStringList files;
    for (const QFileInfo &info : dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase))
        if (pictureSuffixes().contains(info.suffix().toLower()))
            files << info.absoluteFilePath();
    return files;
}

QString batchOutputPath(const QString &outFolder, const QString &base, const QString &extension, const QSet<QString> &taken) {
    const QDir dir(outFolder);
    for (int n = 1;; ++n) {
        const QString name = (n == 1 ? base : QStringLiteral("%1-%2").arg(base).arg(n)) + QLatin1Char('.') + extension;
        const QString path = dir.absoluteFilePath(name);
        if (!taken.contains(path.toLower()) && !QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
            return path;
    }
}

Errors checkBatch(const BatchSpec &spec) {
    if (spec.action.action.needsPrompt() && spec.prompt.trimmed().isEmpty())
        return {makeError(QStringLiteral("input_invalid"), QStringLiteral("%1 needs a prompt.").arg(spec.action.action.name), QStringLiteral("Give --prompt <text>."))};
    if (spec.action.action.selection == QLatin1String("bounds"))
        return {makeError(QStringLiteral("action_invalid"), QStringLiteral("%1 cannot run over a folder.").arg(spec.action.action.name))};
    return {};
}

Result<BatchPrepared> batchPrepare(const BatchSpec &spec, const QString &file, CanvasStore &store) {
    const Result<mask::PaintDocument> picture = mask::readCanvasFile(file);
    if (!picture)
        return picture.errors();
    const Result<CanvasInfo> made = store.create(QFileInfo(file).completeBaseName(), picture.value(), QStringLiteral("batch"));
    if (!made)
        return made.errors();
    BatchPrepared prepared;
    prepared.canvas = made->id;
    prepared.revision = made->head;
    prepared.width = picture->width();
    prepared.height = picture->height();

    const Action &action = spec.action.action;
    ActionBinding binding;
    binding.canvas = prepared.canvas;
    binding.revision = prepared.revision;
    binding.rect = RouteRect{0, 0, prepared.width, prepared.height};
    RouteInput in;
    in.canvasWidth = prepared.width;
    in.canvasHeight = prepared.height;
    in.selection = binding.rect;
    in.hasPrompt = !spec.prompt.trimmed().isEmpty();
    const Route routed = routeAction(in);
    if (routed.action != RouteAction::None) {
        binding.padding = routed.padding;
        binding.scale = routed.scale;
    }
    if (action.input(QStringLiteral("prompt")))
        binding.values.insert(QStringLiteral("prompt"), spec.prompt.trimmed());
    if (action.input(QStringLiteral("seed")))
        binding.values.insert(QStringLiteral("seed"), spec.seed ? QJsonValue(*spec.seed) : QJsonValue(qint64(QRandomGenerator::global()->generate())));
    Result<Document> bound = bindAction(spec.action.document, action, spec.action.graph, binding);
    if (!bound) {
        batchDiscard(spec, prepared, store);
        return bound.errors();
    }
    prepared.bound = bound.value();
    return prepared;
}

void batchDiscard(const BatchSpec &spec, const BatchPrepared &prepared, CanvasStore &store) {
    if (!spec.keepCanvases && !prepared.canvas.isEmpty())
        store.remove(prepared.canvas);
}

Result<QString> batchFinish(const BatchSpec &spec, const BatchPrepared &prepared, const QString &file, QSet<QString> &taken, CanvasStore &store) {
    const Result<CanvasInfo> info = store.info(prepared.canvas);
    if (!info)
        return info.errors();
    const Result<mask::PaintDocument> document = store.load(prepared.canvas, info->head);
    if (!document)
        return document.errors();
    const Result<ExportedPicture> picture = exportPicture(document.value(), spec.picture);
    if (!picture)
        return picture.errors();
    const QString path = batchOutputPath(spec.outFolder, QFileInfo(file).completeBaseName(), picture->format, taken);
    const Errors written = createFile(path, picture->bytes, QStringLiteral("the exported picture"));
    if (hasErrors(written))
        return written;
    taken.insert(path.toLower());
    batchDiscard(spec, prepared, store);
    return path;
}

QJsonObject BatchItem::toJson() const {
    QJsonObject out{{"file", file}, {"status", status}};
    if (!output.isEmpty())
        out.insert(QStringLiteral("output"), output);
    if (!code.isEmpty())
        out.insert(QStringLiteral("code"), code);
    if (!error.isEmpty())
        out.insert(QStringLiteral("error"), error);
    return out;
}

static int countOf(const QList<BatchItem> &items, const QString &status) {
    return int(std::count_if(items.cbegin(), items.cend(), [&](const BatchItem &item) { return item.status == status; }));
}

int BatchReport::ok() const { return countOf(items, QStringLiteral("ok")); }
int BatchReport::failed() const { return countOf(items, QStringLiteral("failed")); }
int BatchReport::cancelled() const { return countOf(items, QStringLiteral("cancelled")); }

QJsonObject BatchReport::toJson() const {
    QJsonArray list;
    for (const BatchItem &item : items)
        list.append(item.toJson());
    return QJsonObject{{"folder", outFolder}, {"total", int(items.size())}, {"ok", ok()}, {"failed", failed()}, {"cancelled", cancelled()}, {"items", list}};
}

Result<BatchReport> runBatch(const BatchSpec &spec, const BatchRunner &runner, const BatchHooks &hooks) {
    if (const Errors problems = checkBatch(spec); hasErrors(problems))
        return problems;
    const Result<QStringList> files = batchPictures(spec.inFolder);
    if (!files)
        return files.errors();
    if (files->isEmpty())
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("The folder %1 has no pictures (.png, .jpg, .webp, .bmp, .tif or .ora).").arg(spec.inFolder));
    if (const Errors made = makeFolder(spec.outFolder); hasErrors(made))
        return made;

    const auto emitEvent = [&](const QJsonObject &event) {
        if (hooks.event)
            hooks.event(event);
    };
    BatchReport report;
    report.outFolder = QFileInfo(spec.outFolder).absoluteFilePath();
    CanvasStore store = CanvasStore::forSession();
    QSet<QString> taken;
    const int total = int(files->size());
    emitEvent({{"event", "batch.started"}, {"total", total}});
    bool stopped = false;
    for (int index = 0; index < total; ++index) {
        const QString &file = files->at(index);
        BatchItem item;
        item.file = file;
        stopped = stopped || (hooks.cancelled && hooks.cancelled());
        if (stopped) {
            item.status = QStringLiteral("cancelled");
            item.error = QStringLiteral("Not started: the batch was cancelled.");
        } else {
            emitEvent({{"event", "item.started"}, {"index", index}, {"total", total}, {"file", file}});
            const Result<BatchPrepared> prepared = batchPrepare(spec, file, store);
            if (!prepared) {
                item.status = QStringLiteral("failed");
                item.code = prepared.error().code;
                item.error = prepared.error().message;
            } else {
                const BatchRunResult ran = runner(prepared->bound, spec.action.file);
                if (ran.status == QLatin1String("ok")) {
                    const Result<QString> output = batchFinish(spec, prepared.value(), file, taken, store);
                    if (output) {
                        item.status = QStringLiteral("ok");
                        item.output = output.value();
                    } else {
                        item.status = QStringLiteral("failed");
                        item.code = output.error().code;
                        item.error = output.error().message;
                        batchDiscard(spec, prepared.value(), store);
                    }
                } else {
                    item.status = ran.status == QLatin1String("cancelled") ? QStringLiteral("cancelled") : QStringLiteral("failed");
                    item.code = ran.code;
                    item.error = ran.message;
                    stopped = stopped || item.status == QLatin1String("cancelled");
                    batchDiscard(spec, prepared.value(), store);
                }
            }
            QJsonObject done = item.toJson();
            done.insert(QStringLiteral("event"), QStringLiteral("item.done"));
            done.insert(QStringLiteral("index"), index);
            done.insert(QStringLiteral("total"), total);
            emitEvent(done);
        }
        report.items << item;
    }
    emitEvent({{"event", "batch.done"}, {"ok", report.ok()}, {"failed", report.failed()}, {"cancelled", report.cancelled()}});
    return report;
}

} // namespace fugue::canvas
