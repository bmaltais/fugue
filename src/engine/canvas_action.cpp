#include "engine/canvas_action.h"

#include <QJsonArray>
#include <QSet>

#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue::canvas {

namespace {

const BindingReader &reader() {
    static const BindingReader read{QStringLiteral("action"), QStringLiteral("action_invalid"), actionRoles(),
                                    QStringLiteral("Edit workflow to fix the action's block, or the nodes it names.")};
    return read;
}

Error invalid(const QString &message, const QString &hint = {}) { return reader().invalid(message, hint); }

void setLiteral(Node &node, const QString &input, const QJsonValue &value) { node.inputs.insert(input, InputValue::fromJson(value)); }

} // namespace

const QStringList &actionRoles() { return sharedRoles(); }

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
    action.name = reader().text(object, "name", errors, QStringLiteral("The action")).trimmed();
    if (action.name.isEmpty())
        errors << invalid(QStringLiteral("The action has no name."));
    action.description = reader().text(object, "description", errors, QStringLiteral("The action"));
    action.glyph = reader().text(object, "glyph", errors, QStringLiteral("The action"));
    action.region = reader().text(object, "region", errors, QStringLiteral("The action"));
    if (object.contains(QLatin1String("selection"))) {
        action.selection = reader().text(object, "selection", errors, QStringLiteral("The action"));
        static const QStringList kinds{QStringLiteral("required"), QStringLiteral("optional"), QStringLiteral("bounds"), QStringLiteral("none")};
        if (!kinds.contains(action.selection))
            errors << invalid(QStringLiteral("The action's `selection` is \"%1\"; it is required, optional, bounds or none.").arg(action.selection));
    }
    action.inputs = reader().inputs(object.value(QLatin1String("inputs")), errors);
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
        else if (it->type == QLatin1String(newType))
            graph.news << it.key();
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
    if (graph.writes.isEmpty() && graph.news.isEmpty())
        errors << invalid(QStringLiteral("This action has no Canvas Write or Canvas New, so nothing would land anywhere."));
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
    errors << checkInputTargets(reader(), action.inputs, document, registry);
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
    // `none` (Upscale): the action takes the whole picture whatever is selected; the Region keeps its own rectangle, padding and scale.
    const bool wholePicture = action.selection == QLatin1String("none");
    if (binding.rect && !wholePicture) {
        setLiteral(region, QStringLiteral("x"), binding.rect->x);
        setLiteral(region, QStringLiteral("y"), binding.rect->y);
        setLiteral(region, QStringLiteral("width"), binding.rect->width);
        setLiteral(region, QStringLiteral("height"), binding.rect->height);
    }
    // A stored selection, or none: a rectangle never reads one the workflow happened to hold.
    if (!binding.selection.isEmpty() && !wholePicture)
        setLiteral(region, QStringLiteral("selection"), binding.selection);
    else
        region.inputs.remove(QStringLiteral("selection"));
    if (!binding.layer.isEmpty()) // else the workflow's own Layer stays (an action may read one layer by design)
        setLiteral(region, QStringLiteral("layer"), binding.layer);
    if (binding.padding && !wholePicture)
        setLiteral(region, QStringLiteral("padding"), *binding.padding);
    if (binding.scale && !wholePicture)
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
        const Result<QJsonValue> resolved = resolveInputValue(reader(), input, value);
        if (!resolved)
            return resolved.errors();
        setInputValue(bound, input, resolved.value());
    }
    for (const QString &write : graph.writes) {
        Node &node = bound.graph.nodes[write];
        if (!binding.suffix.isEmpty())
            setLiteral(node, QStringLiteral("suffix"), binding.suffix);
        if (binding.hidden)
            setLiteral(node, QStringLiteral("visible"), false);
    }
    return bound;
}

} // namespace fugue::canvas
