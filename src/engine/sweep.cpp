#include "engine/sweep.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue {

namespace {

Error usage(const QString &message, const QString &hint = {}) {
    Error error;
    error.code = QStringLiteral("usage");
    error.message = message;
    error.hint = hint;
    return error;
}

// The text of a value as a label: a string as it is (a path as its file's name), anything else as JSON.
QString textOf(const QJsonValue &value) {
    if (value.isString()) {
        const QString text = value.toString();
        return text.contains(QLatin1Char('/')) && !text.endsWith(QLatin1Char('/')) ? text.mid(text.lastIndexOf(QLatin1Char('/')) + 1) : text;
    }
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

// "a,b" at commas outside quotes, brackets and braces.
QStringList splitValues(const QString &text) {
    QStringList parts;
    QString current;
    int depth = 0;
    bool quoted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            current += c;
            if (c == QLatin1Char('\\') && i + 1 < text.size())
                current += text.at(++i);
            else if (c == QLatin1Char('"'))
                quoted = false;
            continue;
        }
        if (c == QLatin1Char('"'))
            quoted = true;
        else if (c == QLatin1Char('[') || c == QLatin1Char('{'))
            ++depth;
        else if (c == QLatin1Char(']') || c == QLatin1Char('}'))
            --depth;
        if (c == QLatin1Char(',') && depth <= 0 && !quoted) {
            parts << current;
            current.clear();
        } else {
            current += c;
        }
    }
    parts << current;
    return parts;
}

Error withPlace(Error error, const QString &node, const QString &input) {
    error.node = node;
    error.input = input;
    return error;
}

} // namespace

QJsonValue parseSetValue(const QString &raw) {
    const QJsonDocument parsed = QJsonDocument::fromJson("[" + raw.toUtf8() + "]");
    if (parsed.isArray() && parsed.array().size() == 1)
        return parsed.array().at(0);
    return raw;
}

void collectMediaPaths(const QJsonValue &value, QStringList &paths) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            collectMediaPaths(item, paths);
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (const QString path = object.value("path").toString(); !path.isEmpty() && !paths.contains(path))
            paths << path;
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key() != QLatin1String("path"))
                collectMediaPaths(it.value(), paths);
    }
}

Result<SweepAxis> parseSweepAxis(const QString &text) {
    const qsizetype dot = text.indexOf(QLatin1Char('.'));
    const qsizetype equals = text.indexOf(QLatin1Char('='));
    if (dot <= 0 || equals < dot + 2)
        return usage(QStringLiteral("--sweep takes <node>.<input>=<value>,<value>..., not \"%1\".").arg(text));
    SweepAxis axis;
    axis.node = text.left(dot);
    axis.input = text.mid(dot + 1, equals - dot - 1);
    for (const QString &part : splitValues(text.mid(equals + 1))) {
        if (part.trimmed().isEmpty())
            return usage(QStringLiteral("--sweep %1.%2 has an empty value.").arg(axis.node, axis.input), "Give each value between commas.");
        const QJsonValue value = parseSetValue(part.trimmed());
        axis.values << value;
        axis.texts << textOf(value);
    }
    return axis;
}

Result<SweepPlan> planSweep(const Document &document, const QList<SweepAxis> &axes, const QString &to, const Registry &registry) {
    if (axes.isEmpty() || axes.size() > 2)
        return usage(QStringLiteral("A sweep has one or two axes, not %1.").arg(axes.size()), "Give --sweep once or twice.");
    if (!to.isEmpty() && !document.graph.nodes.contains(to))
        return usage(QStringLiteral("--to names node \"%1\", which the workflow does not have.").arg(to));
    Errors errors;
    qsizetype count = 1;
    for (qsizetype a = 0; a < axes.size(); ++a) {
        const SweepAxis &axis = axes.at(a);
        count *= axis.values.size();
        for (qsizetype b = 0; b < a; ++b)
            if (axes.at(b).node == axis.node && axes.at(b).input == axis.input)
                errors << withPlace(usage(QStringLiteral("%1.%2 is swept twice.").arg(axis.node, axis.input)), axis.node, axis.input);
        const auto found = document.graph.nodes.constFind(axis.node);
        if (found == document.graph.nodes.cend()) {
            Error error = usage(QStringLiteral("--sweep names node \"%1\", which the workflow does not have.").arg(axis.node),
                                "Use the node's id as `fugue workflow describe` shows it.");
            for (auto it = document.graph.nodes.cbegin(); it != document.graph.nodes.cend(); ++it)
                error.valid.append(it.key());
            errors << error;
            continue;
        }
        const NodeType *type = registry.nodeTypeFor(found->type, found->version.major);
        if (!type) {
            Error error = withPlace(usage(QStringLiteral("Node %1 is of type %2, which is not installed.").arg(axis.node, found->type)),
                                    axis.node, axis.input);
            error.code = QStringLiteral("unknown_node_type");
            errors << error;
            continue;
        }
        const InputSpec *spec = type->input(axis.input);
        if (!spec) {
            Error error = withPlace(usage(QStringLiteral("Node %1 has no input \"%2\".").arg(axis.node, axis.input)), axis.node, axis.input);
            error.code = QStringLiteral("unknown_input");
            for (const InputSpec &input : type->inputs)
                error.valid.append(input.name);
            errors << error;
            continue;
        }
        if (spec->type.list) {
            Error error = withPlace(usage(QStringLiteral("%1.%2 is a list input, which a sweep cannot vary.").arg(axis.node, axis.input)),
                                    axis.node, axis.input);
            error.code = QStringLiteral("type_mismatch");
            errors << error;
            continue;
        }
        for (const QJsonValue &value : axis.values)
            for (Error error : checkLiteral(*spec, value, registry.portKind(spec->type.type)))
                errors << withPlace(error, axis.node, axis.input);
    }
    if (hasErrors(errors))
        return errors;
    if (count > kMaxSweepCells) {
        Error error = usage(QStringLiteral("This sweep makes %1 pictures; the most is %2.").arg(count).arg(kMaxSweepCells),
                            "Give fewer values.");
        error.code = QStringLiteral("value_out_of_range");
        return error;
    }

    const SweepAxis &across = axes.at(0);
    const qsizetype rows = axes.size() == 2 ? axes.at(1).values.size() : 1;
    SweepPlan plan;
    plan.labels.columns = across.values.size();
    if (axes.size() == 2) {
        for (const QString &text : across.texts)
            plan.labels.columnLabels << across.input + QLatin1Char('=') + text;
        for (const QString &text : axes.at(1).texts)
            plan.labels.rowLabels << axes.at(1).input + QLatin1Char('=') + text;
    }
    for (qsizetype row = 0; row < rows; ++row)
        for (qsizetype column = 0; column < across.values.size(); ++column) {
            SweepCell cell;
            cell.bound = document;
            QStringList words;
            for (qsizetype a = 0; a < axes.size(); ++a) {
                const SweepAxis &axis = axes.at(a);
                const qsizetype at = a == 0 ? column : row;
                cell.bound.graph.nodes[axis.node].inputs.insert(axis.input, InputValue::fromJson(axis.values.at(at)));
                words << axis.input + QLatin1Char('=') + axis.texts.at(at);
            }
            cell.label = words.join(QStringLiteral(", "));
            plan.labels.cells << (axes.size() == 1 ? cell.label : QString());
            plan.cells << cell;
        }
    return plan;
}

Result<Document> composeDocument(const QStringList &cellFiles, const SweepLabels &labels, const Registry &registry, const QString &folder) {
    const NodeType *load = registry.nodeType(QStringLiteral("fugue/load-image"));
    const NodeType *grid = registry.nodeType(QStringLiteral("fugue/image-grid"));
    const NodeType *save = registry.nodeType(QStringLiteral("fugue/save-image"));
    for (const auto &[id, type] : {std::pair{"fugue/load-image", load}, std::pair{"fugue/image-grid", grid}, std::pair{"fugue/save-image", save}})
        if (!type) {
            Error error = usage(QStringLiteral("The node type %1 is not installed.").arg(QString::fromLatin1(id)));
            error.code = QStringLiteral("unknown_node_type");
            return error;
        }
    Document document;
    QList<InputValue> images;
    for (qsizetype i = 0; i < cellFiles.size(); ++i) {
        const QString id = QStringLiteral("cell%1").arg(i + 1);
        Node node;
        node.type = load->id;
        node.version = load->version;
        node.inputs.insert(QStringLiteral("file"), InputValue::literal(cellFiles.at(i)));
        document.graph.nodes.insert(id, node);
        images << InputValue::link({id, QStringLiteral("image")});
    }
    Node gridNode;
    gridNode.type = grid->id;
    gridNode.version = grid->version;
    gridNode.inputs.insert(QStringLiteral("images"), InputValue::list(images));
    gridNode.inputs.insert(QStringLiteral("columns"), InputValue::literal(labels.columns));
    gridNode.inputs.insert(QStringLiteral("labels"), InputValue::literal(labels.cells.join(QLatin1Char('\n'))));
    gridNode.inputs.insert(QStringLiteral("column_labels"), InputValue::literal(labels.columnLabels.join(QLatin1Char('\n'))));
    gridNode.inputs.insert(QStringLiteral("row_labels"), InputValue::literal(labels.rowLabels.join(QLatin1Char('\n'))));
    document.graph.nodes.insert(QStringLiteral("grid"), gridNode);
    Node saveNode;
    saveNode.type = save->id;
    saveNode.version = save->version;
    saveNode.inputs.insert(QStringLiteral("image"), InputValue::link({QStringLiteral("grid"), QStringLiteral("image")}));
    saveNode.inputs.insert(QStringLiteral("name"), InputValue::literal(QStringLiteral("sweep-{date}-{time}")));
    if (!folder.isEmpty())
        saveNode.inputs.insert(QStringLiteral("folder"), InputValue::literal(folder));
    document.graph.nodes.insert(QStringLiteral("sheet"), saveNode);
    return document;
}

} // namespace fugue
