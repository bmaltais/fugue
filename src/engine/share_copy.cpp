#include "share_copy.h"

#include <QJsonArray>
#include <QRegularExpression>

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

QString bareName(const QString &path) {
    // Both separators: a Windows path in a file made elsewhere.
    const qsizetype slash = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return slash < 0 ? path : path.mid(slash + 1);
}

// One literal (or list of literals) rewritten for its kind; anything that is not a plain
// string (a link, a randomized value, a number) is returned as it was.
QJsonValue rewrite(const QJsonValue &value, Kind kind, const ShareOptions &options) {
    if (value.isArray()) {
        QJsonArray items;
        for (const QJsonValue &item : value.toArray())
            items.append(rewrite(item, kind, options));
        return items;
    }
    if (!value.isString())
        return value;
    const QString text = value.toString();
    switch (kind) {
    case Kind::Path:
    case Kind::UnknownType: // only what is certainly a path
        return isLocalAbsolutePath(text) ? QJsonValue(bareName(text)) : value;
    case Kind::Prompt:
        return options.withoutPrompts ? QJsonValue(QString()) : value;
    case Kind::Plain:
        return value;
    }
    return value;
}

QJsonObject rewriteNodes(const Context &context, const QJsonObject &nodes, const ShareOptions &options) {
    QJsonObject out;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        QJsonObject node = it.value().toObject();
        QJsonObject inputs = node.value("inputs").toObject();
        for (auto input = inputs.begin(); input != inputs.end(); ++input)
            input.value() = rewrite(input.value(), kindOfInput(context, nodes, it.key(), input.key(), 0), options);
        if (node.contains("inputs"))
            node.insert("inputs", inputs);
        out.insert(it.key(), node);
    }
    return out;
}

} // namespace

bool isLocalAbsolutePath(const QString &text) {
    static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    return text.startsWith(QLatin1Char('/')) || text == QLatin1String("~") || text.startsWith(QLatin1String("~/")) ||
           drive.match(text).hasMatch();
}

QJsonObject shareCopy(const QJsonObject &document, const Registry &registry, const ShareOptions &options) {
    const QJsonObject groups = document.value("groups").toObject();
    const Context context{registry, groups};
    QJsonObject out = document;

    if (document.contains("graph")) {
        QJsonObject graph = document.value("graph").toObject();
        graph.insert("nodes", rewriteNodes(context, graph.value("nodes").toObject(), options));
        out.insert("graph", graph);
    }

    if (document.contains("groups")) {
        QJsonObject rewritten;
        for (auto it = groups.begin(); it != groups.end(); ++it) {
            QJsonObject group = it.value().toObject();
            group.insert("nodes", rewriteNodes(context, group.value("nodes").toObject(), options));
            QJsonArray inputs;
            for (const QJsonValue &exposed : group.value("inputs").toArray()) {
                QJsonObject input = exposed.toObject();
                if (input.contains("default"))
                    input.insert("default", rewrite(input.value("default"),
                                                    kindOfGroupInput(context, group, input.value("name").toString(), 0), options));
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
