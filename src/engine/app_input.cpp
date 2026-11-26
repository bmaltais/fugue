#include "engine/app_input.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

#include "engine/registry.h"

namespace fugue::app {

bool isPictureRole(const InputBinding &input) { return input.role == QLatin1String("image") || input.role == QLatin1String("mask"); }

bool isBlank(const QJsonValue &value) { return value.isUndefined() || value.isNull() || (value.isString() && value.toString().trimmed().isEmpty()); }

bool isNumber(const QJsonValue &value) {
    if (value.isDouble())
        return true;
    bool ok = false;
    return value.isString() && !value.toString().isEmpty() && (value.toString().toDouble(&ok), ok);
}

namespace {

Error usage(const QString &message) { return makeError(QStringLiteral("usage"), message); }

// What the first target holds, when it holds something a form or a default can show.
QJsonValue heldBy(const InputBinding &input, const Document &document) {
    if (input.targets.isEmpty())
        return QJsonValue::Undefined;
    const PortRef &target = input.targets.first();
    const auto node = document.graph.nodes.constFind(target.node);
    if (node == document.graph.nodes.constEnd())
        return QJsonValue::Undefined;
    const InputValue value = node->inputs.value(target.port);
    if (value.isEmpty())
        return QJsonValue::Undefined;
    switch (value.kind()) {
    case InputValue::Kind::Literal:
        return value.literal();
    case InputValue::Kind::Resource:
    case InputValue::Kind::Auto: // a seed left to the engine: the workflow's own value is {"auto": "random"}
        return value.toJson();
    default:
        return QJsonValue::Undefined;
    }
}

QString kindOf(const InputBinding &input, const QJsonValue &held, const InputSpec *spec, bool resource) {
    if (isPictureRole(input) || input.role == QLatin1String("model") || input.role == QLatin1String("seed"))
        return input.role;
    if (input.choices && !input.choices->isEmpty())
        return QStringLiteral("choice");
    if (resource)
        return QStringLiteral("model");
    if (held.isBool() || (spec && spec->type.type.id == QLatin1String("boolean") && !spec->type.list))
        return QStringLiteral("toggle");
    if (input.role == QLatin1String("strength") || input.min || input.max || held.isDouble()
        || (spec && spec->type.type.id == QLatin1String("number") && !spec->type.list))
        return QStringLiteral("number");
    return QStringLiteral("text");
}

// What a form starts on.
QJsonValue initialOf(const InputBinding &input, const QString &kind, const InputSpec *spec, const QJsonValue &shown) {
    QJsonValue value = shown;
    // The workflow holds no value (a group instance left at its inner input's default): the spec's default.
    if (value.isUndefined() && spec && !spec->defaultValue.isUndefined()
        && (kind == QLatin1String("number") || kind == QLatin1String("text") || kind == QLatin1String("toggle")))
        value = spec->defaultValue;
    if (kind == QLatin1String("choice")) {
        // A choice always has one in force: the default or the workflow's value when it is one of the words, else the target's own
        // default when it is, else the first.
        if (!value.isString() || !input.choices->contains(value.toString()))
            value = spec && input.choices->contains(spec->defaultValue.toString()) ? spec->defaultValue : QJsonValue(input.choices->value(0));
    } else if (kind == QLatin1String("toggle")) {
        value = value.toBool();
    } else if (kind == QLatin1String("seed")) {
        if (!isNumber(value))
            value = QJsonValue::Undefined;
    }
    return value;
}

} // namespace

AppInput describeInput(const InputBinding &given, const Document &document, const Registry *registry) {
    AppInput out;
    out.spec = registry && !given.targets.isEmpty() ? targetInputSpec(document, *registry, given.targets.first()) : nullptr;
    out.binding = completedBy(given, out.spec);
    const InputBinding &input = out.binding;
    if (out.spec) {
        out.portType = out.spec->type.type.toString();
        if (registry->portKind(out.spec->type.type) == PortKind::Resource)
            out.resource = out.spec->type.type;
    }
    out.optionalPicture = input.optional && isPictureRole(input);
    // An optional picture or mask starts empty, whatever file its node holds: left empty it takes its branch out of the run (#484).
    out.held = out.optionalPicture ? QJsonValue(QJsonValue::Undefined) : heldBy(input, document);
    out.kind = kindOf(input, out.held, out.spec, out.resource.has_value());
    out.randomSeed = !input.targets.isEmpty() && heldBy(input, document).toObject().contains(QLatin1String("auto"));
    out.own = !input.defaultValue.isUndefined() ? input.defaultValue : out.held;
    out.defaultValue = !out.own.isUndefined() || out.optionalPicture ? out.own : (out.spec ? out.spec->defaultValue : QJsonValue(QJsonValue::Undefined));
    out.initial = initialOf(input, out.kind, out.spec, out.own);
    out.advanced = input.section.compare(QStringLiteral("Advanced"), Qt::CaseInsensitive) == 0;
    // An input a run cannot do without: a picture or a mask with none (left out, an optional one takes its branch out, #484), a
    // prompt that is not optional with no fallback.
    if (isBlank(out.defaultValue))
        out.required = isPictureRole(input) ? !input.optional
                                            : input.role == QLatin1String("prompt") && !input.optional && input.fallback.isEmpty();
    return out;
}

QString cliTypeOf(const AppInput &input) {
    if (input.kind == QLatin1String("seed"))
        return QStringLiteral("number");
    if (input.kind == QLatin1String("toggle"))
        return QStringLiteral("boolean");
    if (input.kind == QLatin1String("text"))
        return input.portType.isEmpty() ? QStringLiteral("any") : input.portType;
    return input.kind;
}

QJsonValue AppInput::valueIn(const QMap<QString, QJsonValue> &values) const {
    return values.contains(binding.key()) ? values.value(binding.key()) : defaultValue;
}

QJsonValue AppInput::fromTyped(const QVariant &typed) const {
    if (kind == QLatin1String("number") || kind == QLatin1String("seed")) {
        bool ok = false;
        const double number = typed.toDouble(&ok);
        if (!ok || std::isnan(number) || typed.toString().trimmed().isEmpty())
            return QJsonValue::Undefined;
        return number;
    }
    return QJsonValue::fromVariant(typed);
}

Result<QJsonValue> AppInput::fromText(const QString &text, const ModelLookup &lookup) const {
    const QString key = binding.key();
    if (kind == QLatin1String("image") || kind == QLatin1String("mask")) {
        const QString path = QFileInfo(text).absoluteFilePath();
        if (!QFileInfo(path).isFile())
            return Result<QJsonValue>(makeError(QStringLiteral("file_not_found"), QStringLiteral("There is no file %1 for the input \"%2\".").arg(path, key)));
        return QJsonValue(path);
    }
    if (kind == QLatin1String("model")) {
        const QJsonDocument parsed = QJsonDocument::fromJson(text.toUtf8());
        if (parsed.isObject())
            return QJsonValue(parsed.object());
        if (!resource || !lookup)
            return Result<QJsonValue>(usage(QStringLiteral("The input \"%1\" takes a whole resource handle as JSON.").arg(key)));
        const Result<QJsonObject> handle = lookup(*resource, text);
        if (!handle)
            return handle.errors();
        return QJsonValue(*handle);
    }
    if (kind == QLatin1String("seed") || kind == QLatin1String("number")) {
        const QString word = text.trimmed().toLower();
        if (kind == QLatin1String("seed") && (word == QLatin1String("auto") || word == QLatin1String("random")))
            return QJsonValue(QJsonObject{{QStringLiteral("auto"), QStringLiteral("random")}});
        bool ok = false;
        const double number = text.trimmed().toDouble(&ok);
        if (!ok)
            return Result<QJsonValue>(usage(QStringLiteral("The input \"%1\" takes a number, not \"%2\".").arg(key, text)));
        return QJsonValue(number);
    }
    if (kind == QLatin1String("toggle")) {
        const QString word = text.trimmed().toLower();
        if (word != QLatin1String("true") && word != QLatin1String("false"))
            return Result<QJsonValue>(usage(QStringLiteral("The input \"%1\" takes true or false, not \"%2\".").arg(key, text)));
        return QJsonValue(word == QLatin1String("true"));
    }
    return QJsonValue(text);
}

} // namespace fugue::app
