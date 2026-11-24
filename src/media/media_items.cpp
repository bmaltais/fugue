#include "media/media_items.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include "engine/embed.h"
#include "engine/run_folder.h"

namespace fugue {

namespace {

bool isMediaKind(const QString &type) {
    return type == QLatin1String("image") || type == QLatin1String("video") || type == QLatin1String("audio");
}

// Every {type: image|video|audio, path} inside a value, however deep (a batch is a list).
void collectMedia(const QJsonValue &value, QList<QPair<QString, QString>> &out) {
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray())
            collectMedia(entry, out);
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        const QString type = object.value("type").toString();
        const QString path = object.value("path").toString();
        if (isMediaKind(type) && !path.isEmpty())
            out.append({type, path});
        else
            for (const QJsonValue &entry : object)
                collectMedia(entry, out);
    }
}

// The top-level nodes no other node reads: where the results are.
QSet<QString> sinksOf(const QJsonObject &workflow) {
    const QJsonObject nodes = workflow.value("graph").toObject().value("nodes").toObject();
    QSet<QString> read;
    const QRegularExpression from(QStringLiteral("^([a-z][a-z0-9_-]*)\\."));
    for (const QJsonValue &node : nodes) {
        const QJsonObject inputs = node.toObject().value("inputs").toObject();
        for (const QJsonValue &input : inputs) {
            const QJsonValue source = input.isObject() ? input.toObject().value("from") : QJsonValue();
            const QRegularExpressionMatch match = from.match(source.toString());
            if (match.hasMatch())
                read.insert(match.captured(1));
        }
    }
    QSet<QString> sinks;
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
        if (!read.contains(it.key()))
            sinks.insert(it.key());
    return sinks;
}

QString textOf(const QJsonValue &value) { return value.isString() ? value.toString() : QString(); }

// The text of the first Text node of a graph's `nodes` (the prompt), or "".
QString promptOf(const QJsonObject &nodes) {
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        if (node.value("type").toString() == QLatin1String("fugue/text")) {
            const QString text = textOf(node.value("inputs").toObject().value("text"));
            if (!text.isEmpty())
                return text;
        }
    }
    return {};
}

} // namespace

QDateTime runStartOf(const QString &runId) {
    static const QRegularExpression shape(QStringLiteral("^(\\d{8}-\\d{6}-\\d{3})-"));
    const QRegularExpressionMatch match = shape.match(runId);
    if (!match.hasMatch())
        return {};
    QDateTime time = QDateTime::fromString(match.captured(1), QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    time.setTimeZone(QTimeZone::utc());
    return time;
}

QList<MediaItem> mediaOfRun(const QString &runsRoot, const QString &runId) {
    const Result<RunFolder> folder = RunFolder::open(runsRoot, runId);
    if (!folder)
        return {};
    const QJsonObject state = folder->state();
    const QJsonObject workflow = folder->workflow();
    const bool known = !workflow.isEmpty();
    const QSet<QString> sinks = known ? sinksOf(workflow) : QSet<QString>();
    const QString workflowPath = state.value("workflow").toObject().value("path").toString();
    const QJsonObject nodes = state.value("nodes").toObject();

    QList<MediaItem> items;
    QSet<QString> seen;
    // Node order of the file, so the items of a run come in a steady order.
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        const QString nodeState = node.value("state").toString();
        if (nodeState != QLatin1String("done") && nodeState != QLatin1String("cached") && nodeState != QLatin1String("frozen"))
            continue;
        // A group's inner node counts with the group (the part of its path id before "/").
        if (known && !sinks.contains(it.key().section('/', 0, 0)))
            continue;
        QList<QPair<QString, QString>> found;
        collectMedia(node.value("outputs"), found);
        collectMedia(node.value("shown"), found); // what a Preview or a Compare shows (it has no outputs)
        for (const auto &[kind, path] : std::as_const(found)) {
            if (seen.contains(path) || !QFileInfo(path).isFile())
                continue;
            seen.insert(path);
            items.append({path, QFileInfo(path).fileName(), kind, runId, it.key(), workflowPath, runStartOf(runId)});
        }
    }
    return items;
}

QList<MediaItem> mediaOfWorkflow(const QString &runsRoot, const QString &workflow, int maxRuns) {
    if (workflow.isEmpty())
        return {};
    const QStringList runs = QDir(runsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    QList<MediaItem> items;
    QSet<QString> seen;
    int matched = 0;
    for (const QString &run : runs) {
        if (matched >= maxRuns)
            break;
        const Result<RunFolder> folder = RunFolder::open(runsRoot, run);
        if (!folder || folder->state().value("workflow").toObject().value("path").toString() != workflow)
            continue;
        ++matched;
        for (const MediaItem &item : mediaOfRun(runsRoot, run)) {
            if (seen.contains(item.path))
                continue;
            seen.insert(item.path);
            items.append(item);
        }
    }
    return items;
}

RunIdentity runIdentityOf(const QString &runsRoot, const QString &runId) {
    const QList<MediaItem> items = mediaOfRun(runsRoot, runId);
    RunIdentity identity;
    const MediaItem *lead = nullptr;
    for (const MediaItem &item : items)
        if (item.kind == QLatin1String("image")) {
            lead = &item;
            break;
        }
    if (!lead && !items.isEmpty())
        lead = &items.first();
    if (lead) {
        identity.leadPath = lead->path;
        identity.leadKind = lead->kind;
    }
    const Result<RunFolder> folder = RunFolder::open(runsRoot, runId);
    const QString prompt = folder ? promptOf(folder->workflow().value("graph").toObject().value("nodes").toObject()) : QString();
    identity.label = prompt.simplified();
    if (identity.label.isEmpty() && lead)
        identity.label = lead->name;
    return identity;
}

QList<MediaGroup> groupByRun(const QList<MediaItem> &items) {
    QList<MediaGroup> groups;
    for (const MediaItem &item : items) {
        if (groups.isEmpty() || groups.last().run != item.run)
            groups.append({item.run, item.time, {}});
        groups.last().items.append(item);
    }
    return groups;
}

QJsonObject embeddedSummary(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QLatin1String("png") && suffix != QLatin1String("webp"))
        return {{"embedded", false}};
    const Result<QJsonObject> read = readEmbeddedWorkflow(path);
    if (!read)
        return {{"embedded", false}};
    const QJsonObject document = read.value();
    const QJsonObject nodes = document.value("graph").toObject().value("nodes").toObject();
    QJsonObject out{{"embedded", true}, {"nodes", nodes.size()}};
    const QString prompt = promptOf(nodes);
    if (!prompt.isEmpty())
        out.insert("prompt", prompt);
    const QJsonObject used = document.value("run").toObject().value("nodes").toObject();
    for (auto it = used.begin(); it != used.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (!out.contains("seed") && entry.value("auto").toObject().contains("seed"))
            out.insert("seed", entry.value("auto").toObject().value("seed"));
        // The run section records the model as its handle (11.7); its name is what people read.
        const QJsonValue model = entry.value("model");
        const QString modelName = model.isObject() ? textOf(model.toObject().value("name")) : textOf(model);
        if (!out.contains("model") && !modelName.isEmpty())
            out.insert("model", modelName);
    }
    return out;
}

} // namespace fugue
