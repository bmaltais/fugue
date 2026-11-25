#pragma once

// The input list the `action` block (canvas actions, #231) and the `app` block (Fugue Apps, #384) share
// (docs/canvas-actions-design.md 1.2, docs/spec.md 4.9): `inputs[]`, each naming node inputs of the workflow (its targets)
// with a role, a label, a section, a default, a range and a subset of choices. An app is an action with no canvas Region,
// so the two blocks read, check and bind this list with the same code and differ only in the words their errors use
// (the BindingReader) and the roles they accept. The vocabulary is a group's exposed inputs (spec 5.4).
//
// Pure: a JSON value, a document and a registry in; parsed inputs, errors and a changed copy of the document out. The
// file is never changed. The order of the list is the order a form shows it in; there is no `order` field.
//
// Staleness (spec 5.4, "an exposed input whose inner node is deleted leaves the face"): pruneStaleTargets drops the
// targets whose node or input is gone, then the inputs left with no target, and says what it dropped. A node whose type
// is not installed is not stale (a pack is missing): its targets stay. checkInputTargets reports what is wrong with the
// targets that remain (linked, of the wrong type), as errors that name the fix.

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/document.h"
#include "engine/error.h"
#include "engine/result.h"

namespace fugue {

class Registry;
struct InputSpec;

struct InputBinding {
    QString role;  // one of the reader's roles, or empty for a plain input
    QString name;  // a plain input's name; with a role, an optional key of its own (an app with two images)
    QList<PortRef> targets;
    QString label;
    QString section; // "Advanced" shows only in the advanced state
    QJsonValue defaultValue = QJsonValue::Undefined;
    std::optional<double> min;
    std::optional<double> max;
    // `choices`: the words offered. On a target that is a choice itself (a text input with `choices`) they must be a subset
    // of its own. With `choice_values` (slice 4) each word stands for a number, in the choices' order: a form shows the
    // words and a binding puts the number in the targets, which must take a number; `default` is then a word.
    std::optional<QStringList> choices;
    std::optional<QList<double>> choiceValues;
    bool optional = false; // a prompt may be empty; an app's picture or mask may be left out (its branch is then bypassed)
    // Apps (#484): the nodes bypassed when an optional picture or mask is left empty; none means the targets' own nodes.
    QStringList bypass;
    QString fallback;      // the prompt used when it is left empty (G8)

    // How a form and a binding name it: the name when it has one, else the role.
    QString key() const { return name.isEmpty() ? role : name; }
};

// The roles both blocks know: prompt, negative_prompt, strength, seed, model, layer. An app adds image and mask (a file
// input and a paint-mask input of a Load Image) with appRoles().
const QStringList &sharedRoles();
const QStringList &appRoles();

// How a block reads its list: which words its errors use and which roles it takes.
struct BindingReader {
    QString noun;       // "action", "app"
    QString code;       // the error code of a block that is not well formed: "action_invalid", "app_invalid"
    QStringList roles;  // the roles it accepts
    QString fix;        // the default hint: what to edit

    Error invalid(const QString &message, const QString &hint = {}) const;
    // A text field of `object`: empty when absent, an error (and empty) when not text. `what` names the owner in the message.
    QString text(const QJsonObject &object, const char *key, Errors &errors, const QString &what) const;
    std::optional<double> number(const QJsonObject &object, const char *key, Errors &errors, const QString &what) const;
    // The `inputs` value: nothing is an empty list; errors for what is not well formed (twice-named keys included).
    QList<InputBinding> inputs(const QJsonValue &json, Errors &errors) const;
};

// The input as the block writes it: only the fields that are set, in the order the schema lists them.
QJsonObject toJson(const InputBinding &input);
QJsonArray toJson(const QList<InputBinding> &inputs);

// The spec (type, default, min/max, choices) of the input a target names: a node's own input, or, for a group instance, the
// inner input its exposed input stands for (spec 5.4), however deep the groups nest. Null when the node or input is gone
// or the node's type is not installed. The one resolver the checks, the CLI and the App form share.
const InputSpec *targetInputSpec(const Document &document, const Registry &registry, const PortRef &target);

// `input` with what it leaves unsaid taken from its target's spec: min, max and, with no choice_values, the choices. A form
// and `fugue app info` show these; the block's own words win. A null spec changes nothing.
InputBinding completedBy(InputBinding input, const InputSpec *spec);

// Drops the targets that name a node or an input the document no longer has, then the inputs left with none; returns a
// sentence per drop, in words ("The prompt's target encode.prompt is gone: the workflow has no node encode.").
QStringList pruneStaleTargets(QList<InputBinding> &inputs, const Document &document, const Registry &registry);

// What is wrong with the targets: missing, linked to another node, of different types, of the wrong type for the role,
// choice_values onto a non-number, choices that are not a subset of the target's. One error per fault.
Errors checkInputTargets(const BindingReader &reader, const QList<InputBinding> &inputs, const Document &document,
                         const Registry &registry);

// The value an input takes from `given` (a form's value, Undefined for none): its default when none, a choice word turned
// into its number, a number held in min..max. Undefined: the workflow's own value stays. `invalid` for a word that is
// none of the choices.
Result<QJsonValue> resolveInputValue(const BindingReader &reader, const InputBinding &input, QJsonValue given);

// Puts `value` in every target of `input`. Nothing for Undefined; a target whose node is gone is skipped.
void setInputValue(Document &document, const InputBinding &input, const QJsonValue &value);

// "prompt role" or "input \"steps\"": how errors name an input.
QString describe(const InputBinding &input);

} // namespace fugue
