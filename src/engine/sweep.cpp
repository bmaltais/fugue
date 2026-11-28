#include "engine/sweep.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

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

constexpr int kLabelLength = 40;
constexpr int kSheetGap = 8;       // the Image Grid's default gap
constexpr int kSheetMaxSide = 16384; // and the most it makes on a side

// A value on one line: line breaks and runs of white space become one space.
QString oneLine(const QString &text) { return text.simplified(); }

// The text of a value for a label: a string as it is, anything else as JSON; on one line.
QString textOf(const QJsonValue &value) {
    if (value.isString())
        return oneLine(value.toString());
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

bool looksLikePath(const QString &text) { return text.contains(QLatin1Char('/')) && !text.endsWith(QLatin1Char('/')); }

// The last `depth` components of a path.
QString tail(const QString &path, int depth) {
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.mid(std::max<qsizetype>(0, parts.size() - depth)).join(QLatin1Char('/'));
}

// What a label says of each value of an axis: a path as its file name, with as many parent folders as it takes to tell the paths
// of the axis apart (two `a/x.png` and `b/x.png` are `a/x.png` and `b/x.png`); a long value cut with an ellipsis.
QStringList shown(const QStringList &texts) {
    int depth = 1;
    const auto distinct = [&](int d) {
        QStringList seen;
        for (const QString &text : texts) {
            const QString name = looksLikePath(text) ? tail(text, d) : text;
            if (seen.contains(name) && texts.count(text) == 1)
                return false;
            seen << name;
        }
        return true;
    };
    while (depth < 64 && !distinct(depth))
        ++depth;
    QStringList out;
    for (const QString &text : texts) {
        QString name = looksLikePath(text) ? tail(text, depth) : text;
        if (name.size() > kLabelLength)
            name = name.left(kLabelLength - 1) + QChar(0x2026);
        out << name;
    }
    return out;
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

void collectMediaPaths(const QJsonValue &value, QStringList &paths, const QString &type) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            collectMediaPaths(item, paths);
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (const QString path = object.value("path").toString();
            !path.isEmpty() && !paths.contains(path) && (type.isEmpty() || object.value("type").toString() == type))
            paths << path;
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key() != QLatin1String("path"))
                collectMediaPaths(it.value(), paths, type);
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
        if (part.isEmpty())
            return usage(QStringLiteral("--sweep %1.%2 has an empty value.").arg(axis.node, axis.input), "Give each value between commas.");
        const QJsonValue value = parseSetValue(part);
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
    if (!to.isEmpty()) {
        const Node &target = document.graph.nodes.value(to);
        const NodeType *type = registry.nodeTypeFor(target.type, target.version.major);
        const bool makesPicture =
            type && (type->id == QLatin1String("fugue/preview")
                     || std::any_of(type->outputs.cbegin(), type->outputs.cend(), [](const OutputSpec &o) { return o.type.type.id == QLatin1String("image") && !o.type.list; }));
        if (type && !makesPicture)
            return usage(QStringLiteral("--to names node \"%1\", which makes no picture.").arg(to),
                         "Name a Save Image or Preview node, or any node with an image output.");
    }
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
    // Names for the labels: the input's, or node.input when both axes have inputs of one name (two samplers' steps).
    QStringList names;
    for (const SweepAxis &axis : axes)
        names << (axes.size() == 2 && axes.at(0).input == axes.at(1).input ? axis.node + QLatin1Char('.') + axis.input : axis.input);
    QList<QStringList> words;
    for (const SweepAxis &axis : axes)
        words << shown(axis.texts);
    SweepPlan plan;
    // One axis wraps into a near-square sheet (a long row would pass the sheet's size limit); two axes are the columns and the rows.
    plan.labels.columns = axes.size() == 2 ? int(across.values.size()) : int(std::ceil(std::sqrt(double(across.values.size()))));
    plan.labels.rows = axes.size() == 2 ? int(rows) : int((across.values.size() + plan.labels.columns - 1) / plan.labels.columns);
    if (axes.size() == 2) {
        for (const QString &text : words.at(0))
            plan.labels.columnLabels << names.at(0) + QLatin1Char('=') + text;
        for (const QString &text : words.at(1))
            plan.labels.rowLabels << names.at(1) + QLatin1Char('=') + text;
    }
    for (qsizetype row = 0; row < rows; ++row)
        for (qsizetype column = 0; column < across.values.size(); ++column) {
            SweepCell cell;
            cell.bound = document;
            QStringList cellWords;
            for (qsizetype a = 0; a < axes.size(); ++a) {
                const SweepAxis &axis = axes.at(a);
                const qsizetype at = a == 0 ? column : row;
                cell.bound.graph.nodes[axis.node].inputs.insert(axis.input, InputValue::fromJson(axis.values.at(at)));
                cellWords << names.at(a) + QLatin1Char('=') + words.at(a).at(at);
            }
            cell.label = cellWords.join(QStringLiteral(", "));
            plan.labels.cells << (axes.size() == 1 ? cell.label : QString());
            plan.cells << cell;
        }
    return plan;
}

Errors checkSheetSize(const SweepLabels &labels, const QSize &cell) {
    const long long width = (long long)kSheetGap + (long long)labels.columns * (cell.width() + kSheetGap);
    const long long height = (long long)kSheetGap + (long long)labels.rows * (cell.height() + kSheetGap);
    if (width <= kSheetMaxSide && height <= kSheetMaxSide)
        return {};
    return {makeError(QStringLiteral("value_out_of_range"),
                      QStringLiteral("The sheet would be %1 x %2 pixels, over the limit of %3 on a side.").arg(width).arg(height).arg(kSheetMaxSide),
                      QStringLiteral("Sweep fewer values, or make the pictures smaller."))};
}

Result<Document> composeDocument(const QStringList &cellFiles, const SweepLabels &labels, const Registry &registry, const SheetOptions &options) {
    const NodeType *load = registry.nodeType(QStringLiteral("fugue/load-image"));
    const NodeType *grid = registry.nodeType(QStringLiteral("fugue/image-grid"));
    const NodeType *save = registry.nodeType(QStringLiteral("fugue/save-image"));
    const NodeType *plain = registry.nodeType(QStringLiteral("fugue/solid-mask"));
    const bool needsPlain = cellFiles.contains(QString());
    for (const auto &[id, type] : {std::pair{"fugue/load-image", load}, std::pair{"fugue/image-grid", grid}, std::pair{"fugue/save-image", save},
                                   std::pair{"fugue/solid-mask", needsPlain ? plain : load}})
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
        if (cellFiles.at(i).isEmpty()) {
            // A cell with no picture: a plain gray tile made in the sheet's own graph, so no file is written for it.
            node.type = plain->id;
            node.version = plain->version;
            node.inputs.insert(QStringLiteral("value"), InputValue::literal(0.25));
            node.inputs.insert(QStringLiteral("width"), InputValue::literal(options.blank.width()));
            node.inputs.insert(QStringLiteral("height"), InputValue::literal(options.blank.height()));
            images << InputValue::link({id, QStringLiteral("mask")});
        } else {
            node.type = load->id;
            node.version = load->version;
            node.inputs.insert(QStringLiteral("file"), InputValue::literal(cellFiles.at(i)));
            images << InputValue::link({id, QStringLiteral("image")});
        }
        document.graph.nodes.insert(id, node);
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
    if (!options.folder.isEmpty())
        saveNode.inputs.insert(QStringLiteral("folder"), InputValue::literal(options.folder));
    document.graph.nodes.insert(QStringLiteral("sheet"), saveNode);
    return document;
}

} // namespace fugue
