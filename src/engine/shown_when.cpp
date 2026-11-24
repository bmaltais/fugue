#include "engine/shown_when.h"

#include <QCoreApplication>

#include <algorithm>

namespace fugue {

namespace {

QString number(double value) { return QString::number(value, 'g', 12); }

QString valueText(const QJsonValue &value) {
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool() ? QStringLiteral("on") : QStringLiteral("off");
    if (value.isDouble())
        return number(value.toDouble());
    return QStringLiteral("unset");
}

bool holds(const ShownClause &clause, const ShownContext &context) {
    switch (clause.kind) {
    case ShownClause::Kind::Linked:
        return context.linked(clause.input) == clause.is;
    case ShownClause::Kind::Carries: {
        const std::optional<bool> carries = context.carries ? context.carries(clause.input, clause.property) : std::nullopt;
        return carries.value_or(true);
    }
    case ShownClause::Kind::Value: {
        if (context.linked(clause.input))
            return true; // a wire has no value to read
        const QJsonValue value = context.value(clause.input);
        if (value.isUndefined())
            return true;
        if (!clause.oneOf.isEmpty())
            return std::any_of(clause.oneOf.begin(), clause.oneOf.end(), [&](const QJsonValue &v) { return v == value; });
        if (!value.isDouble())
            return true;
        const double v = value.toDouble();
        return (!clause.lt || v < *clause.lt) && (!clause.le || v <= *clause.le) && (!clause.gt || v > *clause.gt)
               && (!clause.ge || v >= *clause.ge);
    }
    }
    return true;
}

QString clauseReason(const ShownClause &clause, const ShownContext &context) {
    const QString label = context.label ? context.label(clause.input) : QString();
    const QString name = label.isEmpty() ? clause.input : label;
    switch (clause.kind) {
    case ShownClause::Kind::Linked:
        return (clause.is ? QCoreApplication::translate("ShownWhen", "%1 is linked") : QCoreApplication::translate("ShownWhen", "%1 is not linked")).arg(name);
    case ShownClause::Kind::Carries:
        return QCoreApplication::translate("ShownWhen", "%1 comes from a source with a %2").arg(name, clause.property);
    case ShownClause::Kind::Value: {
        QStringList parts;
        if (!clause.oneOf.isEmpty()) {
            QStringList values;
            for (const QJsonValue &v : clause.oneOf)
                values << valueText(v);
            parts << QCoreApplication::translate("ShownWhen", "%1 is %2").arg(name, values.join(QCoreApplication::translate("ShownWhen", " or ")));
        }
        if (clause.lt)
            parts << QCoreApplication::translate("ShownWhen", "%1 is below %2").arg(name, number(*clause.lt));
        if (clause.le)
            parts << QCoreApplication::translate("ShownWhen", "%1 is at most %2").arg(name, number(*clause.le));
        if (clause.gt)
            parts << QCoreApplication::translate("ShownWhen", "%1 is above %2").arg(name, number(*clause.gt));
        if (clause.ge)
            parts << QCoreApplication::translate("ShownWhen", "%1 is at least %2").arg(name, number(*clause.ge));
        return parts.join(QCoreApplication::translate("ShownWhen", " and "));
    }
    }
    return {};
}

} // namespace

ShownWhen ShownWhen::parse(const QJsonObject &object, QStringList &problems) {
    ShownWhen condition;
    const int before = problems.size();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString key = it.key();
        const QJsonValue value = it.value();
        ShownClause clause;
        if (key == QLatin1String("is")) {
            if (!object.contains("linked"))
                problems << QStringLiteral("\"is\" only goes with \"linked\".");
            else if (!value.isBool())
                problems << QStringLiteral("\"is\" must be true or false.");
            continue;
        }
        if (key == QLatin1String("linked")) {
            if (!value.isString() || value.toString().isEmpty()) {
                problems << QStringLiteral("\"linked\" must name an input.");
                continue;
            }
            clause.kind = ShownClause::Kind::Linked;
            clause.input = value.toString();
            clause.is = object.value("is").isBool() ? object.value("is").toBool() : true;
        } else if (key == QLatin1String("carries")) {
            const QJsonObject inputs = value.toObject();
            if (!value.isObject() || inputs.isEmpty()) {
                problems << QStringLiteral("\"carries\" must be an object of input name to property.");
                continue;
            }
            for (auto in = inputs.begin(); in != inputs.end(); ++in) {
                if (!in.value().isString() || in.value().toString().isEmpty()) {
                    problems << QStringLiteral("\"carries\" has a property for %1 that is not a name.").arg(in.key());
                    continue;
                }
                ShownClause one;
                one.kind = ShownClause::Kind::Carries;
                one.input = in.key();
                one.property = in.value().toString();
                condition.clauses << one;
            }
            continue;
        } else if (value.isArray()) {
            if (value.toArray().isEmpty()) {
                problems << QStringLiteral("\"%1\" lists no values.").arg(key);
                continue;
            }
            clause.input = key;
            clause.oneOf = value.toArray();
        } else if (value.isObject()) {
            const QJsonObject compare = value.toObject();
            clause.input = key;
            bool any = false;
            for (auto c = compare.begin(); c != compare.end(); ++c) {
                std::optional<double> *slot = c.key() == QLatin1String("lt")   ? &clause.lt
                                              : c.key() == QLatin1String("le") ? &clause.le
                                              : c.key() == QLatin1String("gt") ? &clause.gt
                                              : c.key() == QLatin1String("ge") ? &clause.ge
                                                                               : nullptr;
                if (!slot || !c.value().isDouble()) {
                    problems << QStringLiteral("\"%1\" compares with lt, le, gt or ge and a number, not %2.").arg(key, c.key());
                    continue;
                }
                *slot = c.value().toDouble();
                any = true;
            }
            if (!any)
                continue;
        } else {
            problems << QStringLiteral("\"%1\" must be a list of values or a comparison.").arg(key);
            continue;
        }
        condition.clauses << clause;
    }
    if (problems.size() != before || condition.clauses.isEmpty())
        return {};
    condition.raw = object;
    return condition;
}

QStringList ShownWhen::inputs() const {
    QStringList names;
    for (const ShownClause &clause : clauses)
        if (!names.contains(clause.input))
            names << clause.input;
    return names;
}

ShownWhen ShownWhen::renamed(const std::function<QString(const QString &)> &rename) const {
    ShownWhen result;
    result.clauses = clauses;
    QJsonObject raw;
    for (ShownClause &clause : result.clauses) {
        const QString name = rename(clause.input);
        if (name.isEmpty())
            return {};
        clause.input = name;
    }
    // The raw form is rebuilt from the clauses so toJson() stays in step.
    QJsonObject carries;
    for (const ShownClause &clause : result.clauses) {
        switch (clause.kind) {
        case ShownClause::Kind::Linked:
            raw.insert("linked", clause.input);
            raw.insert("is", clause.is);
            break;
        case ShownClause::Kind::Carries:
            carries.insert(clause.input, clause.property);
            break;
        case ShownClause::Kind::Value: {
            if (!clause.oneOf.isEmpty()) {
                raw.insert(clause.input, clause.oneOf);
                break;
            }
            QJsonObject compare;
            for (const auto &[key, bound] : {std::pair{"lt", clause.lt}, {"le", clause.le}, {"gt", clause.gt}, {"ge", clause.ge}})
                if (bound)
                    compare.insert(QLatin1String(key), *bound);
            raw.insert(clause.input, compare);
            break;
        }
        }
    }
    if (!carries.isEmpty())
        raw.insert("carries", carries);
    result.raw = raw;
    return result;
}

bool applies(const ShownWhen &condition, const ShownContext &context) {
    return std::all_of(condition.clauses.begin(), condition.clauses.end(),
                       [&](const ShownClause &clause) { return holds(clause, context); });
}

QString shownReason(const ShownWhen &condition, const ShownContext &context) {
    QStringList parts;
    for (const ShownClause &clause : condition.clauses)
        parts << clauseReason(clause, context);
    return QCoreApplication::translate("ShownWhen", "Applies only when %1.").arg(parts.join(QCoreApplication::translate("ShownWhen", " and ")));
}

} // namespace fugue
