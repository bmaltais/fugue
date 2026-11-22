#include "engine/canvas_action.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue::canvas {

namespace {

const QString kInvalid = QStringLiteral("action_invalid");

Error invalid(const QString &message, const QString &hint = {}) {
    return makeError(kInvalid, message,
                     hint.isEmpty() ? QStringLiteral("Edit workflow to fix the action's block, or the nodes it names.") : hint);
}

bool isName(const QString &text) {
    static const QRegularExpression name(QStringLiteral("^[a-z][a-z0-9_]*$"));
    return name.match(text).hasMatch();
}

// The type a role's targets must have: a built-in id, or "resource" for any resource type.
QString roleType(const QString &role) {
    if (role == QLatin1String("seed") || role == QLatin1String("strength"))
        return QStringLiteral("number");
    if (role == QLatin1String("model"))
        return QStringLiteral("resource");
    return QStringLiteral("text"); // prompt, negative_prompt, layer
}

std::optional<double> number(const QJsonObject &object, const char *key, Errors &errors, const QString &what) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isUndefined())
        return std::nullopt;
    if (!value.isDouble()) {
        errors << invalid(QStringLiteral("The action's input %1 has a `%2` that is not a number.").arg(what, QLatin1String(key)));
        return std::nullopt;
    }
    return value.toDouble();
}

QString text(const QJsonObject &object, const char *key, Errors &errors, const QString &what) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isUndefined())
        return {};
    if (!value.isString()) {
        errors << invalid(QStringLiteral("%1 has a `%2` that is not text.").arg(what, QLatin1String(key)));
        return {};
    }
    return value.toString();
}

ActionInput parseInput(const QJsonValue &json, int index, Errors &errors) {
    ActionInput input;
    const QString at = QStringLiteral("The action's input %1").arg(index + 1);
    if (!json.isObject()) {
        errors << invalid(at + QStringLiteral(" is not an object."));
        return input;
    }
    const QJsonObject object = json.toObject();
    input.role = text(object, "role", errors, at);
    input.name = text(object, "name", errors, at);
    if (!input.role.isEmpty() && !actionRoles().contains(input.role))
        errors << invalid(QStringLiteral("%1 has the role \"%2\", which the task bar does not know.").arg(at, input.role),
                          QStringLiteral("Use one of %1, or a plain input with a name.").arg(actionRoles().join(QStringLiteral(", "))));
    if (input.role.isEmpty() && !isName(input.name))
        errors << invalid(QStringLiteral("%1 has no role and no name (a-z, 0-9 and _).").arg(at));
    const QString what = input.key().isEmpty() ? at : QStringLiteral("The action's input \"%1\"").arg(input.key());
    const QJsonValue targets = object.value(QLatin1String("targets"));
    if (!targets.isArray() || targets.toArray().isEmpty())
        errors << invalid(what + QStringLiteral(" names no targets (\"<node>.<input>\")."));
    for (const QJsonValue &target : targets.toArray()) {
        const std::optional<PortRef> ref = target.isString() ? PortRef::parse(target.toString()) : std::nullopt;
        if (!ref)
            errors << invalid(QStringLiteral("%1 has a target that is not \"<node>.<input>\".").arg(what));
        else
            input.targets << *ref;
    }
    input.label = text(object, "label", errors, what);
    input.section = text(object, "section", errors, what);
    input.fallback = text(object, "fallback", errors, what);
    input.defaultValue = object.value(QLatin1String("default"));
    if (input.defaultValue.isObject() && input.defaultValue.toObject().contains(QLatin1String("from")))
        errors << invalid(what + QStringLiteral(" has a link as its default."));
    input.min = number(object, "min", errors, what);
    input.max = number(object, "max", errors, what);
    if (const QJsonValue optional = object.value(QLatin1String("optional")); !optional.isUndefined()) {
        if (!optional.isBool())
            errors << invalid(what + QStringLiteral(" has an `optional` that is not true or false."));
        input.optional = optional.toBool();
    }
    if (const QJsonValue choices = object.value(QLatin1String("choices")); !choices.isUndefined()) {
        QStringList list;
        for (const QJsonValue &choice : choices.toArray())
            list << choice.toString();
        if (!choices.isArray() || list.contains(QString()))
            errors << invalid(what + QStringLiteral(" has choices that are not a list of words."));
        input.choices = list;
    }
    return input;
}

// The spec of an input a target names: a top-level node's, or a group instance's exposed input (the type of its first
// inner target). Null, with `why`, when there is none.
const InputSpec *targetSpec(const Document &document, const Registry &registry, const PortRef &target, QString *why) {
    const auto node = document.graph.nodes.constFind(target.node);
    if (node == document.graph.nodes.constEnd()) {
        *why = QStringLiteral("the workflow has no node %1").arg(target.node);
        return nullptr;
    }
    if (const NodeType *type = registry.nodeTypeFor(node->type, node->version.major)) {
        const InputSpec *spec = type->input(target.port);
        if (!spec)
            *why = QStringLiteral("%1 has no input %2").arg(target.node, target.port);
        return spec;
    }
    if (const GroupDefinition *group = document.group(node->type)) {
        for (const GroupInput &exposed : group->inputs) {
            if (exposed.name != target.port || exposed.targets.isEmpty())
                continue;
            const PortRef inner = exposed.targets.first();
            const auto innerNode = group->nodes.constFind(inner.node);
            const NodeType *innerType = innerNode == group->nodes.constEnd() ? nullptr
                                                                             : registry.nodeTypeFor(innerNode->type, innerNode->version.major);
            if (const InputSpec *spec = innerType ? innerType->input(inner.port) : nullptr)
                return spec;
            break;
        }
        *why = QStringLiteral("the group %1 exposes no input %2").arg(target.node, target.port);
        return nullptr;
    }
    *why = QStringLiteral("%1 is a %2 node, which is not installed").arg(target.node, node->type);
    return nullptr;
}

QString describe(const ActionInput &input) {
    return input.role.isEmpty() ? QStringLiteral("input \"%1\"").arg(input.name) : QStringLiteral("%1 role").arg(input.role);
}

void setLiteral(Node &node, const QString &input, const QJsonValue &value) { node.inputs.insert(input, InputValue::fromJson(value)); }

} // namespace

const QStringList &actionRoles() {
    static const QStringList roles{QStringLiteral("prompt"), QStringLiteral("negative_prompt"), QStringLiteral("strength"),
                                   QStringLiteral("seed"),   QStringLiteral("model"),           QStringLiteral("layer")};
    return roles;
}

const ActionInput *Action::input(const QString &key) const {
    for (const ActionInput &in : inputs)
        if (in.key() == key)
            return &in;
    return nullptr;
}

bool Action::needsPrompt() const {
    const ActionInput *prompt = input(QStringLiteral("prompt"));
    return prompt && !prompt->optional && prompt->fallback.trimmed().isEmpty();
}

Result<Action> parseAction(const QJsonValue &block) {
    if (!block.isObject())
        return invalid(QStringLiteral("The workflow's `action` is not an object."));
    const QJsonObject object = block.toObject();
    Action action;
    Errors errors;
    if (const QJsonValue format = object.value(QLatin1String("format")); !format.isUndefined()) {
        if (!format.isDouble() || format.toDouble() != double(format.toInt()) || format.toInt() < 1)
            return invalid(QStringLiteral("The action's `format` is not a whole number from 1."));
        action.format = format.toInt();
    }
    if (action.format > actionFormat)
        return makeError(QStringLiteral("unsupported_format_version"),
                         QStringLiteral("This action is of format %1, and this Fugue reads format %2.").arg(action.format).arg(actionFormat),
                         QStringLiteral("Needs a newer Fugue: update Fugue to use it."));
    action.name = text(object, "name", errors, QStringLiteral("The action")).trimmed();
    if (action.name.isEmpty())
        errors << invalid(QStringLiteral("The action has no name."));
    action.description = text(object, "description", errors, QStringLiteral("The action"));
    action.glyph = text(object, "glyph", errors, QStringLiteral("The action"));
    action.region = text(object, "region", errors, QStringLiteral("The action"));
    if (object.contains(QLatin1String("selection"))) {
        action.selection = text(object, "selection", errors, QStringLiteral("The action"));
        static const QStringList kinds{QStringLiteral("required"), QStringLiteral("optional"), QStringLiteral("bounds")};
        if (!kinds.contains(action.selection))
            errors << invalid(QStringLiteral("The action's `selection` is \"%1\"; it is required, optional or bounds.").arg(action.selection));
    }
    const QJsonValue inputs = object.value(QLatin1String("inputs"));
    if (!inputs.isUndefined() && !inputs.isArray())
        errors << invalid(QStringLiteral("The action's `inputs` is not a list."));
    QSet<QString> keys;
    const QJsonArray list = inputs.toArray();
    for (int i = 0; i < list.size(); ++i) {
        ActionInput input = parseInput(list.at(i), i, errors);
        if (!input.key().isEmpty() && keys.contains(input.key()))
            errors << invalid(QStringLiteral("The action names its input \"%1\" twice.").arg(input.key()));
        keys.insert(input.key());
        action.inputs << input;
    }
    if (hasErrors(errors))
        return errors;
    return action;
}

Result<std::optional<Action>> actionOf(const Document &document) {
    const QJsonValue block = document.extra.value(QLatin1String("action"));
    if (block.isUndefined())
        return std::optional<Action>();
    const Result<Action> action = parseAction(block);
    if (!action)
        return action.errors();
    return std::optional<Action>(action.value());
}

Result<ActionGraph> checkAction(const Action &action, const Document &document, const Registry &registry) {
    ActionGraph graph;
    for (auto it = document.graph.nodes.cbegin(); it != document.graph.nodes.cend(); ++it) {
        if (it->type == QLatin1String(regionType))
            graph.regions << it.key();
        else if (it->type == QLatin1String(writeType))
            graph.writes << it.key();
    }
    Errors errors;
    if (graph.regions.isEmpty())
        return invalid(QStringLiteral("This action has no Canvas Region to read the selection."));
    if (!action.region.isEmpty()) {
        if (!graph.regions.contains(action.region))
            return invalid(QStringLiteral("This action's selection goes to %1, which is not a Canvas Region of the workflow.").arg(action.region));
        graph.region = action.region;
    } else if (graph.regions.size() > 1) {
        return invalid(QStringLiteral("This action has %1 Canvas Regions and does not say which one takes the selection.").arg(graph.regions.size()),
                       QStringLiteral("Name it in the action's `region`."));
    } else {
        graph.region = graph.regions.first();
    }
    const Node &region = document.graph.nodes.value(graph.region);
    for (const char *input : {"canvas", "revision", "x", "y", "width", "height"})
        if (region.inputs.value(QLatin1String(input)).kind() == InputValue::Kind::Link)
            errors << invalid(QStringLiteral("The %1 of %2 is linked to another node, so the selection cannot be put in.").arg(QLatin1String(input), graph.region));
    if (graph.writes.isEmpty())
        errors << invalid(QStringLiteral("This action has no Canvas Write, so nothing would land on the canvas."));
    // Every Write lands on the shown canvas: its canvas comes from the selection's Region, or from a Region that is left
    // blank (it gets the shown canvas too).
    for (const QString &write : graph.writes) {
        const InputValue canvas = document.graph.nodes.value(write).inputs.value(QStringLiteral("canvas"));
        const QString from = canvas.kind() == InputValue::Kind::Link ? canvas.from().node : QString();
        const bool own = graph.regions.contains(from) &&
                         (from == graph.region ||
                          document.graph.nodes.value(from).inputs.value(QStringLiteral("canvas")).literal().toString().trimmed().isEmpty());
        if (!own)
            errors << invalid(QStringLiteral("This action writes to another canvas: %1 does not take its canvas from a Canvas Region of the shown canvas.").arg(write));
    }
    for (const ActionInput &input : action.inputs) {
        std::optional<PortType> seen;
        for (const PortRef &target : input.targets) {
            QString why;
            const InputSpec *spec = targetSpec(document, registry, target, &why);
            if (!spec) {
                errors << invalid(QStringLiteral("The %1 goes to %2, but %3.").arg(describe(input), target.toString(), why));
                continue;
            }
            const auto node = document.graph.nodes.constFind(target.node);
            if (node->inputs.value(target.port).kind() == InputValue::Kind::Link) {
                errors << invalid(QStringLiteral("The %1 goes to %2, which is linked to another node.").arg(describe(input), target.toString()));
                continue;
            }
            if (seen && *seen != spec->type.type) {
                errors << invalid(QStringLiteral("The %1 goes to inputs of different types (%2 and %3).")
                                      .arg(describe(input), seen->toString(), spec->type.type.toString()));
                continue;
            }
            seen = spec->type.type;
            if (!input.role.isEmpty()) {
                const QString want = roleType(input.role);
                const bool fits = want == QLatin1String("resource") ? registry.portKind(spec->type.type) == PortKind::Resource
                                                                    : spec->type.type.id == want && !spec->type.list;
                if (!fits)
                    errors << invalid(QStringLiteral("The %1 goes to %2, which takes %3, not %4.")
                                          .arg(describe(input), target.toString(), spec->type.toString(),
                                               want == QLatin1String("resource") ? QStringLiteral("a model") : want));
            }
        }
    }
    if (hasErrors(errors))
        return errors;
    return graph;
}

Result<Document> bindAction(const Document &document, const Action &action, const ActionGraph &graph, const ActionBinding &binding) {
    if (binding.canvas.trimmed().isEmpty() || binding.revision.trimmed().isEmpty())
        return invalid(QStringLiteral("There is no canvas to run the action on."), QStringLiteral("Open a canvas first."));
    if (!binding.rect && binding.selection.isEmpty() && action.selection == QLatin1String("required"))
        return invalid(QStringLiteral("Select an area first."), QStringLiteral("Draw a rectangle on the picture, or paint a selection with Quick Mask."));
    Document bound = document;
    Node &region = bound.graph.nodes[graph.region];
    setLiteral(region, QStringLiteral("canvas"), binding.canvas);
    setLiteral(region, QStringLiteral("revision"), binding.revision);
    if (binding.rect) {
        setLiteral(region, QStringLiteral("x"), binding.rect->x);
        setLiteral(region, QStringLiteral("y"), binding.rect->y);
        setLiteral(region, QStringLiteral("width"), binding.rect->width);
        setLiteral(region, QStringLiteral("height"), binding.rect->height);
    }
    // A stored selection, or none: a rectangle never reads one the workflow happened to hold.
    if (!binding.selection.isEmpty())
        setLiteral(region, QStringLiteral("selection"), binding.selection);
    else
        region.inputs.remove(QStringLiteral("selection"));
    if (binding.padding)
        setLiteral(region, QStringLiteral("padding"), *binding.padding);
    if (binding.scale)
        setLiteral(region, QStringLiteral("scale"), *binding.scale);
    // Other Regions of a blank canvas read the same picture: the same canvas and revision.
    for (const QString &other : graph.regions) {
        if (other == graph.region)
            continue;
        Node &node = bound.graph.nodes[other];
        const InputValue canvas = node.inputs.value(QStringLiteral("canvas"));
        if (canvas.isEmpty() || (canvas.kind() == InputValue::Kind::Literal && canvas.literal().toString().trimmed().isEmpty())) {
            setLiteral(node, QStringLiteral("canvas"), binding.canvas);
            setLiteral(node, QStringLiteral("revision"), binding.revision);
        }
    }
    for (const ActionInput &input : action.inputs) {
        QJsonValue value = binding.values.value(input.key(), QJsonValue::Undefined);
        if (input.role == QLatin1String("prompt") && (value.isUndefined() || value.toString().trimmed().isEmpty())) {
            if (!input.fallback.trimmed().isEmpty())
                value = input.fallback;
            else if (!input.optional && !value.isUndefined())
                return invalid(QStringLiteral("Type what to put in the selection first."), QStringLiteral("This action needs a prompt."));
        }
        if (value.isUndefined())
            value = input.defaultValue;
        if (value.isUndefined())
            continue; // the workflow's own value
        if (value.isDouble() && (input.min || input.max)) {
            double v = value.toDouble();
            if (input.min)
                v = std::max(v, *input.min);
            if (input.max)
                v = std::min(v, *input.max);
            value = v;
        }
        for (const PortRef &target : input.targets)
            setLiteral(bound.graph.nodes[target.node], target.port, value);
    }
    return bound;
}

} // namespace fugue::canvas
