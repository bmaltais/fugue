#include "share_copy.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace fugue {

namespace {

enum class Kind { Plain, Path, Prompt, UnknownType };

// How the document is looked at: the node maps of the graph and of each group
// definition, and the group definitions themselves, to find what an input is.
struct Context {
    const Registry &registry;
    const QJsonObject &groups;
};

Kind kindOfSpec(const InputSpec &spec) {
    const QString widget = spec.widget();
    if (widget == QLatin1String("file") || widget == QLatin1String("folder"))
        return Kind::Path;
    if (spec.type.type.id == QLatin1String("text") && widget == QLatin1String("multiline"))
        return Kind::Prompt;
    return Kind::Plain;
}

Kind kindOfInput(const Context &context, const QJsonObject &nodes, const QString &nodeId, const QString &input,
                 int depth);

// What a group's exposed input drives: the kind of its first target (all targets share
// a type, 4.4).
Kind kindOfGroupInput(const Context &context, const QJsonObject &group, const QString &input, int depth) {
    if (depth > 8)
        return Kind::Plain;
    for (const QJsonValue &exposed : group.value("inputs").toArray()) {
        const QJsonObject object = exposed.toObject();
        if (object.value("name").toString() != input)
            continue;
        // An exposed input that drives nothing yet is plain (QJsonArray::first() wants one).
        const QJsonArray targets = object.value("targets").toArray();
        if (targets.isEmpty())
            return Kind::Plain;
        const QString target = targets.at(0).toString();
        const qsizetype dot = target.indexOf(QLatin1Char('.'));
        if (dot <= 0)
            return Kind::Plain;
        return kindOfInput(context, group.value("nodes").toObject(), target.left(dot), target.mid(dot + 1), depth + 1);
    }
    return Kind::Plain;
}

Kind kindOfInput(const Context &context, const QJsonObject &nodes, const QString &nodeId, const QString &input,
                 int depth) {
    const QString type = nodes.value(nodeId).toObject().value("type").toString();
    if (const NodeType *nodeType = context.registry.nodeType(type)) {
        const InputSpec *spec = nodeType->input(input);
        return spec ? kindOfSpec(*spec) : Kind::Plain;
    }
    if (context.groups.contains(type))
        return kindOfGroupInput(context, context.groups.value(type).toObject(), input, depth);
    return Kind::UnknownType;
}

QString bareName(QString path) {
    // A folder written with a trailing separator keeps its own name.
    while (path.size() > 1 && (path.endsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('\\'))))
        path.chop(1);
    // Both separators: a Windows path in a file made elsewhere.
    const qsizetype slash = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return slash < 0 ? path : path.mid(slash + 1);
}

// One literal (or list of literals) rewritten for its kind; anything that is not a plain
// string (a link, a randomized value, a number) is returned as it was. `feedsPath`: the
// node's output reaches a file or folder input, so an absolute path typed in it is a path
// whatever input holds it.
QJsonValue rewrite(const QJsonValue &value, Kind kind, const ShareOptions &options, bool feedsPath) {
    if (value.isArray()) {
        QJsonArray items;
        for (const QJsonValue &item : value.toArray())
            items.append(rewrite(item, kind, options, feedsPath));
        return items;
    }
    if (!value.isString())
        return value;
    const QString text = value.toString();
    // Trimmed, as Save Image and the scheduler trim a path before using it.
    const QJsonValue stripped = isLocalAbsolutePath(text) ? QJsonValue(bareName(text.trimmed())) : value;
    switch (kind) {
    case Kind::Path:
    case Kind::UnknownType: // only what is certainly a path
        return stripped;
    case Kind::Prompt:
        return options.withoutPrompts ? QJsonValue(QString()) : feedsPath ? stripped : value;
    case Kind::Plain:
        return feedsPath ? stripped : value;
    }
    return value;
}

// The node ids that the links in `value` (a link, or a list holding links) come from.
void linkSources(const QJsonValue &value, QStringList &into) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            linkSources(item, into);
        return;
    }
    const QString from = value.toObject().value("from").toString();
    const qsizetype dot = from.indexOf(QLatin1Char('.'));
    if (dot > 0)
        into.append(from.left(dot));
}

// The nodes of `nodes` whose output reaches a file or folder input there, directly or
// through other nodes (a Text node typed with a path and linked to Load Image's file).
QSet<QString> feedingPaths(const Context &context, const QJsonObject &nodes, int depth = 0) {
    QStringList pending;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const QJsonObject inputs = it.value().toObject().value("inputs").toObject();
        for (auto input = inputs.begin(); input != inputs.end(); ++input)
            if (kindOfInput(context, nodes, it.key(), input.key(), depth) == Kind::Path)
                linkSources(input.value(), pending);
    }
    QSet<QString> found;
    while (!pending.isEmpty()) {
        const QString id = pending.takeLast();
        if (found.contains(id) || !nodes.contains(id))
            continue;
        found.insert(id);
        const QJsonObject inputs = nodes.value(id).toObject().value("inputs").toObject();
        for (auto input = inputs.begin(); input != inputs.end(); ++input)
            linkSources(input.value(), pending);
    }
    return found;
}

// Whether a group's exposed input drives, inside the group, a node whose output reaches a
// file or folder input (then what is set on it is a path, whatever its own kind).
bool groupInputFeedsPath(const Context &context, const QJsonObject &group, const QString &input, int depth) {
    if (depth > 8)
        return false;
    const QJsonObject nodes = group.value("nodes").toObject();
    for (const QJsonValue &exposed : group.value("inputs").toArray()) {
        const QJsonObject object = exposed.toObject();
        if (object.value("name").toString() != input)
            continue;
        const QSet<QString> feeding = feedingPaths(context, nodes, depth + 1);
        for (const QJsonValue &target : object.value("targets").toArray()) {
            const QString ref = target.toString();
            const qsizetype dot = ref.indexOf(QLatin1Char('.'));
            if (dot <= 0)
                continue;
            const QString node = ref.left(dot);
            if (feeding.contains(node))
                return true;
            const QString type = nodes.value(node).toObject().value("type").toString();
            if (context.groups.contains(type) &&
                groupInputFeedsPath(context, context.groups.value(type).toObject(), ref.mid(dot + 1), depth + 1))
                return true;
        }
    }
    return false;
}

// The groups whose definitions may hold a path that reaches a file or folder input: an
// instance of them feeds one (in the graph or in another group), or they are used inside
// such a group. What is typed anywhere inside may come out of the group's outputs.
QSet<QString> groupsFeedingPaths(const Context &context, const QJsonObject &graphNodes) {
    QSet<QString> marked;
    QStringList pending;
    const auto markInstances = [&](const QJsonObject &nodes, const QSet<QString> &ids) {
        for (const QString &id : ids) {
            const QString type = nodes.value(id).toObject().value("type").toString();
            if (context.groups.contains(type) && !marked.contains(type)) {
                marked.insert(type);
                pending.append(type);
            }
        }
    };
    markInstances(graphNodes, feedingPaths(context, graphNodes));
    for (auto it = context.groups.begin(); it != context.groups.end(); ++it) {
        const QJsonObject nodes = it.value().toObject().value("nodes").toObject();
        markInstances(nodes, feedingPaths(context, nodes));
    }
    while (!pending.isEmpty()) {
        const QJsonObject nodes = context.groups.value(pending.takeLast()).toObject().value("nodes").toObject();
        const QStringList ids = nodes.keys();
        markInstances(nodes, QSet<QString>(ids.begin(), ids.end()));
    }
    return marked;
}

// `allFeedPaths`: the nodes are inside a group that feeds a path (groupsFeedingPaths).
QJsonObject rewriteNodes(const Context &context, const QJsonObject &nodes, const ShareOptions &options,
                         bool allFeedPaths) {
    const QSet<QString> feeding = allFeedPaths ? QSet<QString>() : feedingPaths(context, nodes);
    QJsonObject out;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        QJsonObject node = it.value().toObject();
        QJsonObject inputs = node.value("inputs").toObject();
        const bool feedsPath = allFeedPaths || feeding.contains(it.key());
        const QString type = node.value("type").toString();
        const QJsonObject group = context.groups.value(type).toObject();
        for (auto input = inputs.begin(); input != inputs.end(); ++input) {
            const bool intoPath = feedsPath || (context.groups.contains(type) &&
                                                groupInputFeedsPath(context, group, input.key(), 0));
            input.value() =
                rewrite(input.value(), kindOfInput(context, nodes, it.key(), input.key(), 0), options, intoPath);
        }
        if (node.contains("inputs"))
            node.insert("inputs", inputs);
        out.insert(it.key(), node);
    }
    return out;
}

} // namespace

bool isLocalAbsolutePath(const QString &text) {
    // A root (`/`, `~/`, `C:\`) and then a name: a lone separator, a bare `~` or a drive
    // alone name nothing of anyone's, and a separator is common text (Text Concat).
    static const QRegularExpression path(QStringLiteral("^(?:/+|~/+|[A-Za-z]:[\\\\/]+)[^/\\\\]"));
    return path.match(text.trimmed()).hasMatch();
}

QJsonObject shareCopy(const QJsonObject &document, const Registry &registry, const ShareOptions &options) {
    const QJsonObject groups = document.value("groups").toObject();
    const Context context{registry, groups};
    QJsonObject out = document;

    const QJsonObject graphNodes = document.value("graph").toObject().value("nodes").toObject();
    if (document.contains("graph")) {
        QJsonObject graph = document.value("graph").toObject();
        graph.insert("nodes", rewriteNodes(context, graphNodes, options, false));
        out.insert("graph", graph);
    }

    if (document.contains("groups")) {
        const QSet<QString> feeding = groupsFeedingPaths(context, graphNodes);
        QJsonObject rewritten;
        for (auto it = groups.begin(); it != groups.end(); ++it) {
            QJsonObject group = it.value().toObject();
            const bool feedsPath = feeding.contains(it.key());
            group.insert("nodes", rewriteNodes(context, group.value("nodes").toObject(), options, feedsPath));
            QJsonArray inputs;
            for (const QJsonValue &exposed : group.value("inputs").toArray()) {
                QJsonObject input = exposed.toObject();
                if (input.contains("default"))
                    input.insert("default",
                                 rewrite(input.value("default"),
                                         kindOfGroupInput(context, group, input.value("name").toString(), 0), options,
                                         feedsPath || groupInputFeedsPath(context, group, input.value("name").toString(), 0)));
                inputs.append(input);
            }
            if (group.contains("inputs"))
                group.insert("inputs", inputs);
            rewritten.insert(it.key(), group);
        }
        out.insert("groups", rewritten);
    }

    if (options.withoutLayout)
        out.remove("layout");
    return out;
}

} // namespace fugue
