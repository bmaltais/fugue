#include "engine/app_binding.h"

#include <QJsonArray>
#include <QSet>

#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue::app {

namespace {

const BindingReader &reader() {
    static const BindingReader read{QStringLiteral("app"), QStringLiteral("app_invalid"), appRoles(),
                                    QStringLiteral("Edit the app's block, or the nodes it names.")};
    return read;
}

Error stale(const QString &message) {
    Error warning = makeError(QStringLiteral("app_stale"), message, QStringLiteral("Edit the app to bind it again."));
    warning.severity = Error::Severity::Warning;
    return warning;
}

} // namespace

const InputBinding *App::input(const QString &key) const {
    for (const InputBinding &in : inputs)
        if (in.key() == key)
            return &in;
    return nullptr;
}

QStringList App::outputNodes() const {
    QStringList nodes;
    for (const AppOutput &output : outputs)
        nodes << output.node;
    return nodes;
}

Result<App> parseApp(const QJsonValue &block) {
    if (!block.isObject())
        return reader().invalid(QStringLiteral("The workflow's `app` is not an object."));
    const QJsonObject object = block.toObject();
    App app;
    Errors errors;
    if (const QJsonValue format = object.value(QLatin1String("format")); !format.isUndefined()) {
        if (!format.isDouble() || format.toDouble() != double(format.toInt()) || format.toInt() < 1)
            return reader().invalid(QStringLiteral("The app's `format` is not a whole number from 1."));
        app.format = format.toInt();
    }
    if (app.format > appFormat)
        return makeError(QStringLiteral("unsupported_format_version"),
                         QStringLiteral("This app is of format %1, and this Fugue reads format %2.").arg(app.format).arg(appFormat),
                         QStringLiteral("Needs a newer Fugue: update Fugue to use it."));
    const QString what = QStringLiteral("The app");
    app.title = reader().text(object, "title", errors, what).trimmed();
    if (app.title.isEmpty())
        errors << reader().invalid(QStringLiteral("The app has no title."));
    app.description = reader().text(object, "description", errors, what);
    if (object.contains(QLatin1String("defaultView"))) {
        app.defaultView = reader().text(object, "defaultView", errors, what);
        if (app.defaultView != QLatin1String("app") && app.defaultView != QLatin1String("graph"))
            errors << reader().invalid(QStringLiteral("The app's `defaultView` is \"%1\"; it is app or graph.").arg(app.defaultView));
    }
    app.inputs = reader().inputs(object.value(QLatin1String("inputs")), errors);
    const QJsonValue outputs = object.value(QLatin1String("outputs"));
    if (!outputs.isUndefined() && !outputs.isArray())
        errors << reader().invalid(QStringLiteral("The app's `outputs` is not a list."));
    QSet<QString> seen;
    const QJsonArray list = outputs.toArray();
    for (int i = 0; i < list.size(); ++i) {
        const QString at = QStringLiteral("The app's output %1").arg(i + 1);
        if (!list.at(i).isObject()) {
            errors << reader().invalid(at + QStringLiteral(" is not an object."));
            continue;
        }
        AppOutput output;
        output.node = reader().text(list.at(i).toObject(), "node", errors, at);
        output.label = reader().text(list.at(i).toObject(), "label", errors, at);
        if (output.node.isEmpty())
            errors << reader().invalid(at + QStringLiteral(" names no node."));
        else if (seen.contains(output.node))
            errors << reader().invalid(QStringLiteral("The app names its output %1 twice.").arg(output.node));
        seen.insert(output.node);
        app.outputs << output;
    }
    if (hasErrors(errors))
        return errors;
    return app;
}

Result<std::optional<App>> appOf(const Document &document) {
    const QJsonValue block = document.extra.value(QLatin1String("app"));
    if (block.isUndefined())
        return std::optional<App>();
    const Result<App> app = parseApp(block);
    if (!app)
        return app.errors();
    return std::optional<App>(app.value());
}

QJsonObject toJson(const App &app) {
    QJsonObject o{{QStringLiteral("format"), app.format}, {QStringLiteral("title"), app.title}};
    if (!app.description.isEmpty())
        o.insert(QStringLiteral("description"), app.description);
    o.insert(QStringLiteral("defaultView"), app.defaultView);
    o.insert(QStringLiteral("inputs"), fugue::toJson(app.inputs));
    QJsonArray outputs;
    for (const AppOutput &output : app.outputs) {
        QJsonObject out{{QStringLiteral("node"), output.node}};
        if (!output.label.isEmpty())
            out.insert(QStringLiteral("label"), output.label);
        outputs.append(out);
    }
    o.insert(QStringLiteral("outputs"), outputs);
    return o;
}

void setApp(Document &document, const App &app) { document.extra.insert(QStringLiteral("app"), toJson(app)); }

void clearApp(Document &document) { document.extra.remove(QStringLiteral("app")); }

Errors pruneApp(App &app, const Document &document, const Registry &registry) {
    Errors warnings;
    for (const QString &line : pruneStaleTargets(app.inputs, document, registry))
        warnings << stale(line);
    for (auto output = app.outputs.begin(); output != app.outputs.end();) {
        if (document.graph.nodes.contains(output->node)) {
            ++output;
            continue;
        }
        warnings << stale(QStringLiteral("The app's output %1 is gone: the workflow has no node %1.").arg(output->node));
        output = app.outputs.erase(output);
    }
    return warnings;
}

Errors checkApp(const App &app, const Document &document, const Registry &registry) {
    Errors errors = checkInputTargets(reader(), app.inputs, document, registry);
    if (app.outputs.isEmpty())
        errors << reader().invalid(QStringLiteral("The app has no output: nothing would be shown."),
                                   QStringLiteral("Name a Save or Preview node in the app's `outputs`."));
    for (const AppOutput &output : app.outputs) {
        const auto node = document.graph.nodes.constFind(output.node);
        if (node == document.graph.nodes.constEnd()) {
            errors << reader().invalid(QStringLiteral("The app's output %1 is not a node of the workflow.").arg(output.node));
            continue;
        }
        const NodeType *type = registry.nodeTypeFor(node->type, node->version.major);
        if (type && !type->sink)
            errors << reader().invalid(QStringLiteral("The app's output %1 is a %2 node, which is not a Save or Preview sink.")
                                           .arg(output.node, type->name));
    }
    return errors;
}

namespace {

bool isPictureRole(const InputBinding &input) { return input.role == QLatin1String("image") || input.role == QLatin1String("mask"); }

bool isBlank(const QJsonValue &value) { return value.isUndefined() || value.isNull() || (value.isString() && value.toString().trimmed().isEmpty()); }

// The nodes an optional picture switches: its `bypass` list, else the nodes of its targets (a group instance stands for its branch).
QStringList switched(const InputBinding &input) {
    if (!input.bypass.isEmpty())
        return input.bypass;
    QStringList nodes;
    for (const PortRef &target : input.targets)
        if (!nodes.contains(target.node))
            nodes << target.node;
    return nodes;
}

} // namespace

Result<Document> bindApp(const Document &document, const App &app, const QMap<QString, QJsonValue> &values) {
    Document bound = document;
    for (const InputBinding &input : app.inputs) {
        if (input.optional && isPictureRole(input)) {
            // An optional picture left empty (nothing given and no default, or an empty one given) takes its branch out of the run;
            // filled, it puts the branch back in, whatever the file held (#484).
            const QJsonValue given = values.contains(input.key()) ? values.value(input.key()) : input.defaultValue;
            const bool empty = isBlank(given);
            for (const QString &id : switched(input))
                if (const auto node = bound.graph.nodes.find(id); node != bound.graph.nodes.end())
                    node->mode = empty ? NodeMode::Bypass : NodeMode::Normal;
            if (empty)
                continue;
        }
        const Result<QJsonValue> resolved = resolveInputValue(reader(), input, values.value(input.key(), QJsonValue::Undefined));
        if (!resolved)
            return resolved.errors();
        setInputValue(bound, input, resolved.value());
    }
    return bound;
}

} // namespace fugue::app
