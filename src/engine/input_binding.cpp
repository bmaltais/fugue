#include "engine/input_binding.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue {

namespace {

bool isName(const QString &text) {
    static const QRegularExpression name(QStringLiteral("^[a-z][a-z0-9_]*$"));
    return name.match(text).hasMatch();
}

// The type a role's targets must have: a built-in id, or "resource" for any resource type. An image and a mask are
// named by a file (a Load Image's `file` and `mask` inputs are text).
QString roleType(const QString &role) {
    if (role == QLatin1String("seed") || role == QLatin1String("strength"))
        return QStringLiteral("number");
    if (role == QLatin1String("model"))
        return QStringLiteral("resource");
    return QStringLiteral("text"); // prompt, negative_prompt, layer, image, mask
}

enum class Target { Found, Stale, NotInstalled };

// The spec of `port` on a node of `type`: a registered type's own input, or a group's exposed input, which keeps its inner
// input's spec (spec 5.4), however deep the groups nest. `why` says what is missing otherwise.
Target portSpec(const Document &document, const Registry &registry, const QString &nodeId, const QString &type, int major,
                const QString &port, const InputSpec **spec, QString *why, int depth = 0) {
    *spec = nullptr;
    if (const NodeType *nodeType = registry.nodeTypeFor(type, major)) {
        *spec = nodeType->input(port);
        if (*spec)
            return Target::Found;
        *why = QStringLiteral("%1 has no input %2").arg(nodeId, port);
        return Target::Stale;
    }
    if (const GroupDefinition *group = document.group(type)) {
        for (const GroupInput &exposed : group->inputs) {
            if (exposed.name != port || exposed.targets.isEmpty())
                continue;
            const PortRef inner = exposed.targets.first();
            const auto innerNode = group->nodes.constFind(inner.node);
            if (innerNode == group->nodes.constEnd() || depth > 16)
                break;
            const Target found = portSpec(document, registry, inner.node, innerNode->type, innerNode->version.major, inner.port, spec, why, depth + 1);
            if (found == Target::Found)
                return found;
            if (found == Target::NotInstalled) {
                *why = QStringLiteral("%1 is a %2 node, which is not installed").arg(nodeId, innerNode->type);
                return found;
            }
            break;
        }
        *why = QStringLiteral("the group %1 exposes no input %2").arg(nodeId, port);
        return Target::Stale;
    }
    *why = QStringLiteral("%1 is a %2 node, which is not installed").arg(nodeId, type);
    return Target::NotInstalled;
}

// The spec of an input a target names: a top-level node's, or a group instance's exposed input (its inner input's).
Target targetSpec(const Document &document, const Registry &registry, const PortRef &target, const InputSpec **spec, QString *why) {
    *spec = nullptr;
    const auto node = document.graph.nodes.constFind(target.node);
    if (node == document.graph.nodes.constEnd()) {
        *why = QStringLiteral("the workflow has no node %1").arg(target.node);
        return Target::Stale;
    }
    return portSpec(document, registry, target.node, node->type, node->version.major, target.port, spec, why);
}

} // namespace

const InputSpec *targetInputSpec(const Document &document, const Registry &registry, const PortRef &target) {
    const InputSpec *spec = nullptr;
    QString why;
    targetSpec(document, registry, target, &spec, &why);
    return spec;
}

InputBinding completedBy(InputBinding input, const InputSpec *spec) {
    if (!spec)
        return input;
    if (!input.min)
        input.min = spec->min;
    if (!input.max)
        input.max = spec->max;
    if (!input.choices && !input.choiceValues && spec->choices)
        input.choices = spec->choices;
    return input;
}

const QStringList &sharedRoles() {
    static const QStringList roles{QStringLiteral("prompt"), QStringLiteral("negative_prompt"), QStringLiteral("strength"),
                                   QStringLiteral("seed"),   QStringLiteral("model"),           QStringLiteral("layer")};
    return roles;
}

const QStringList &appRoles() {
    static const QStringList roles = sharedRoles() + QStringList{QStringLiteral("image"), QStringLiteral("mask")};
    return roles;
}

QString describe(const InputBinding &input) {
    return input.name.isEmpty() ? QStringLiteral("%1 role").arg(input.role) : QStringLiteral("input \"%1\"").arg(input.name);
}

Error BindingReader::invalid(const QString &message, const QString &hint) const {
    return makeError(code, message, hint.isEmpty() ? fix : hint);
}

std::optional<double> BindingReader::number(const QJsonObject &object, const char *key, Errors &errors, const QString &what) const {
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isUndefined())
        return std::nullopt;
    if (!value.isDouble()) {
        errors << invalid(QStringLiteral("%1 has a `%2` that is not a number.").arg(what, QLatin1String(key)));
        return std::nullopt;
    }
    return value.toDouble();
}

QString BindingReader::text(const QJsonObject &object, const char *key, Errors &errors, const QString &what) const {
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isUndefined())
        return {};
    if (!value.isString()) {
        errors << invalid(QStringLiteral("%1 has a `%2` that is not text.").arg(what, QLatin1String(key)));
        return {};
    }
    return value.toString();
}

QList<InputBinding> BindingReader::inputs(const QJsonValue &json, Errors &errors) const {
    QList<InputBinding> result;
    if (json.isUndefined())
        return result;
    if (!json.isArray()) {
        errors << invalid(QStringLiteral("The %1's `inputs` is not a list.").arg(noun));
        return result;
    }
    QSet<QString> keys;
    const QJsonArray list = json.toArray();
    for (int index = 0; index < list.size(); ++index) {
        InputBinding input;
        const QString at = QStringLiteral("The %1's input %2").arg(noun).arg(index + 1);
        if (!list.at(index).isObject()) {
            errors << invalid(at + QStringLiteral(" is not an object."));
            result << input;
            continue;
        }
        const QJsonObject object = list.at(index).toObject();
        input.role = text(object, "role", errors, at);
        input.name = text(object, "name", errors, at);
        if (!input.role.isEmpty() && !roles.contains(input.role))
            errors << invalid(QStringLiteral("%1 has the role \"%2\", which this %3 does not know.").arg(at, input.role, noun),
                              QStringLiteral("Use one of %1, or a plain input with a name.").arg(roles.join(QStringLiteral(", "))));
        if ((input.role.isEmpty() || !input.name.isEmpty()) && !isName(input.name))
            errors << invalid(input.role.isEmpty() ? QStringLiteral("%1 has no role and no name (a-z, 0-9 and _).").arg(at)
                                                   : QStringLiteral("%1 has a name that is not a-z, 0-9 and _.").arg(at));
        const QString what = input.key().isEmpty() ? at : QStringLiteral("The %1's input \"%2\"").arg(noun, input.key());
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
        if (const QJsonValue bypass = object.value(QLatin1String("bypass")); !bypass.isUndefined()) {
            for (const QJsonValue &node : bypass.toArray())
                input.bypass << node.toString();
            if (!bypass.isArray() || input.bypass.contains(QString()))
                errors << invalid(what + QStringLiteral(" has a `bypass` that is not a list of node ids."));
        }
        if (const QJsonValue choices = object.value(QLatin1String("choices")); !choices.isUndefined()) {
            QStringList words;
            for (const QJsonValue &choice : choices.toArray())
                words << choice.toString();
            if (!choices.isArray() || words.contains(QString()))
                errors << invalid(what + QStringLiteral(" has choices that are not a list of words."));
            input.choices = words;
        }
        if (const QJsonValue values = object.value(QLatin1String("choice_values")); !values.isUndefined()) {
            QList<double> numbers;
            bool allNumbers = values.isArray();
            for (const QJsonValue &value : values.toArray()) {
                allNumbers = allNumbers && value.isDouble();
                numbers << value.toDouble();
            }
            if (!allNumbers) {
                errors << invalid(what + QStringLiteral(" has choice_values that are not a list of numbers."));
            } else if (!input.choices) {
                errors << invalid(what + QStringLiteral(" has choice_values but no choices to give them to."));
            } else if (input.choices->size() != numbers.size()) {
                errors << invalid(QStringLiteral("%1 has %2 choices but %3 choice_values: each choice needs its number.")
                                      .arg(what).arg(input.choices->size()).arg(numbers.size()));
            } else if (QSet<QString>(input.choices->cbegin(), input.choices->cend()).size() != input.choices->size()) {
                errors << invalid(what + QStringLiteral(" names a choice twice."));
            } else {
                input.choiceValues = numbers;
                if (!input.defaultValue.isString() || !input.choices->contains(input.defaultValue.toString()))
                    errors << invalid(what + QStringLiteral(" has choice_values, so its `default` must be one of its choices."));
            }
        }
        if (!input.key().isEmpty() && keys.contains(input.key()))
            errors << invalid(QStringLiteral("The %1 names its input \"%2\" twice.").arg(noun, input.key()));
        keys.insert(input.key());
        result << input;
    }
    return result;
}

QJsonObject toJson(const InputBinding &input) {
    QJsonObject o;
    if (!input.role.isEmpty())
        o.insert(QStringLiteral("role"), input.role);
    if (!input.name.isEmpty())
        o.insert(QStringLiteral("name"), input.name);
    QJsonArray targets;
    for (const PortRef &target : input.targets)
        targets.append(target.toString());
    o.insert(QStringLiteral("targets"), targets);
    if (!input.label.isEmpty())
        o.insert(QStringLiteral("label"), input.label);
    if (!input.section.isEmpty())
        o.insert(QStringLiteral("section"), input.section);
    if (!input.defaultValue.isUndefined())
        o.insert(QStringLiteral("default"), input.defaultValue);
    if (input.min)
        o.insert(QStringLiteral("min"), *input.min);
    if (input.max)
        o.insert(QStringLiteral("max"), *input.max);
    if (input.choices)
        o.insert(QStringLiteral("choices"), QJsonArray::fromStringList(*input.choices));
    if (input.choiceValues) {
        QJsonArray values;
        for (double value : *input.choiceValues)
            values.append(value);
        o.insert(QStringLiteral("choice_values"), values);
    }
    if (input.optional)
        o.insert(QStringLiteral("optional"), true);
    if (!input.fallback.isEmpty())
        o.insert(QStringLiteral("fallback"), input.fallback);
    if (!input.bypass.isEmpty())
        o.insert(QStringLiteral("bypass"), QJsonArray::fromStringList(input.bypass));
    return o;
}

QJsonArray toJson(const QList<InputBinding> &inputs) {
    QJsonArray list;
    for (const InputBinding &input : inputs)
        list.append(toJson(input));
    return list;
}

QStringList pruneStaleTargets(QList<InputBinding> &inputs, const Document &document, const Registry &registry) {
    QStringList report;
    for (auto input = inputs.begin(); input != inputs.end();) {
        const qsizetype before = input->targets.size();
        for (auto target = input->targets.begin(); target != input->targets.end();) {
            const InputSpec *spec = nullptr;
            QString why;
            if (targetSpec(document, registry, *target, &spec, &why) == Target::Stale) {
                report << QStringLiteral("The %1's target %2 is gone: %3.").arg(describe(*input), target->toString(), why);
                target = input->targets.erase(target);
            } else {
                ++target;
            }
        }
        if (input->targets.isEmpty() && before > 0) {
            report << QStringLiteral("The %1 has no target left, so it is dropped.").arg(describe(*input));
            input = inputs.erase(input);
        } else {
            ++input;
        }
    }
    return report;
}

Errors checkInputTargets(const BindingReader &reader, const QList<InputBinding> &inputs, const Document &document, const Registry &registry) {
    Errors errors;
    for (const InputBinding &input : inputs) {
        std::optional<PortType> seen;
        for (const QString &node : input.bypass)
            if (!document.graph.nodes.contains(node))
                errors << reader.invalid(QStringLiteral("The %1 bypasses %2, which is not a node of the workflow.").arg(describe(input), node));
        for (const PortRef &target : input.targets) {
            const InputSpec *spec = nullptr;
            QString why;
            if (targetSpec(document, registry, target, &spec, &why) != Target::Found) {
                errors << reader.invalid(QStringLiteral("The %1 goes to %2, but %3.").arg(describe(input), target.toString(), why));
                continue;
            }
            const auto node = document.graph.nodes.constFind(target.node);
            if (node->inputs.value(target.port).kind() == InputValue::Kind::Link) {
                errors << reader.invalid(QStringLiteral("The %1 goes to %2, which is linked to another node.").arg(describe(input), target.toString()));
                continue;
            }
            if (seen && *seen != spec->type.type) {
                errors << reader.invalid(QStringLiteral("The %1 goes to inputs of different types (%2 and %3).")
                                             .arg(describe(input), seen->toString(), spec->type.type.toString()));
                continue;
            }
            seen = spec->type.type;
            if (input.choiceValues && (spec->type.type.id != QLatin1String("number") || spec->type.list))
                errors << reader.invalid(QStringLiteral("The %1 has choice_values, but %2 takes %3, not a number.")
                                             .arg(describe(input), target.toString(), spec->type.toString()));
            if (!input.choiceValues && input.choices && spec->choices)
                for (const QString &word : *input.choices)
                    if (!spec->choices->contains(word))
                        errors << reader.invalid(QStringLiteral("The %1 offers \"%2\", which %3 does not take (%4).")
                                                     .arg(describe(input), word, target.toString(), spec->choices->join(QStringLiteral(", "))));
            if (!input.role.isEmpty()) {
                const QString want = roleType(input.role);
                const bool fits = want == QLatin1String("resource") ? registry.portKind(spec->type.type) == PortKind::Resource
                                                                    : spec->type.type.id == want && !spec->type.list;
                if (!fits)
                    errors << reader.invalid(QStringLiteral("The %1 goes to %2, which takes %3, not %4.")
                                                 .arg(describe(input), target.toString(), spec->type.toString(),
                                                      want == QLatin1String("resource") ? QStringLiteral("a model") : want));
            }
        }
    }
    return errors;
}

Result<QJsonValue> resolveInputValue(const BindingReader &reader, const InputBinding &input, QJsonValue value) {
    if (value.isUndefined())
        value = input.defaultValue;
    if (value.isUndefined())
        return QJsonValue(QJsonValue::Undefined); // the workflow's own value
    if (input.choiceValues) {
        // A word of the choices stands for its number.
        const qsizetype at = input.choices->indexOf(value.toString());
        if (at < 0)
            return reader.invalid(QStringLiteral("\"%1\" is not one of the choices of the %2's %3 (%4).")
                                      .arg(value.toString(), reader.noun, describe(input), input.choices->join(QStringLiteral(", "))));
        value = input.choiceValues->at(at);
    }
    if (value.isDouble() && (input.min || input.max)) {
        double v = value.toDouble();
        if (input.min)
            v = std::max(v, *input.min);
        if (input.max)
            v = std::min(v, *input.max);
        value = v;
    }
    return value;
}

void setInputValue(Document &document, const InputBinding &input, const QJsonValue &value) {
    if (value.isUndefined())
        return;
    for (const PortRef &target : input.targets)
        if (const auto node = document.graph.nodes.find(target.node); node != document.graph.nodes.end()) // a deleted node stays deleted
            node->inputs.insert(target.port, InputValue::fromJson(value));
}

} // namespace fugue
