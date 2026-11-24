#pragma once

// `shown_when` (spec 6.1): a condition on an input that says whether it applies with the node's
// current values. The face hides a row that does not apply (#429). This file is the pure part:
// parsing the condition and evaluating it against what a caller knows about the node. What the
// node's values and links are, and what an upstream source carries, come in through ShownContext,
// so it needs no document, registry or window; engine/input_use.h reads them from a graph.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace fugue {

// One clause of a condition. A condition is an object and every key must hold:
//   {"fill": ["edge-blur"]}          the input's value is one of these
//   {"denoise": {"lt": 1}}           a number compared: lt, le, gt, ge
//   {"linked": "mask", "is": false}  the input is (is: true, the default) or is not linked
//   {"carries": {"latent": "mask"}}  the source linked to the input carries the property
struct ShownClause {
    enum class Kind { Value, Linked, Carries };
    Kind kind = Kind::Value;
    QString input;
    QJsonArray oneOf;              // Value: the values that apply (empty with a comparison)
    std::optional<double> lt, le, gt, ge; // Value: the comparison
    bool is = true;                // Linked
    QString property;              // Carries
};

struct ShownWhen {
    QJsonObject raw; // as written, what toJson() gives back
    QList<ShownClause> clauses;

    bool isEmpty() const { return clauses.isEmpty(); }
    // Parses a condition. A problem is a sentence naming the key; the condition is empty then.
    static ShownWhen parse(const QJsonObject &object, QStringList &problems);
    // The same condition with each input name passed through `rename`, which returns an empty
    // string for a name that has no counterpart (a group face that does not expose it); the
    // result is then empty, so the row always shows.
    ShownWhen renamed(const std::function<QString(const QString &)> &rename) const;
    QStringList inputs() const;
};

// What the caller knows about the node the condition sits on.
struct ShownContext {
    // The value the input holds now: its literal or else its default. Undefined when there is
    // no static answer (the input is linked, or the node has no such input).
    std::function<QJsonValue(const QString &)> value;
    std::function<bool(const QString &)> linked;
    // Whether the source linked to `input` carries `property`; nothing when that is not known
    // statically (an unknown node type, a group), which counts as shown.
    std::function<std::optional<bool>(const QString &input, const QString &property)> carries;
    // An input's label, for the reason text; the name when empty or unset.
    std::function<QString(const QString &)> label;
};

// True when the row applies. Anything the context cannot answer counts as applying.
bool applies(const ShownWhen &condition, const ShownContext &context);

// "Applies only when Fill is edge-blur and Keep mask is not linked." (the CLI's "not used: why").
QString shownReason(const ShownWhen &condition, const ShownContext &context);

} // namespace fugue
