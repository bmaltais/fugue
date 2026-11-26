#pragma once

// What one input of an app is, said once (#583): `fugue app info` and `fugue app run` (src/cli/app.cpp) and the App view's form
// (src/apps/app_view.cpp) both ask here and only render the answer. The module works out, from the block's binding, the document
// and the registry: the kind of form control, the default, whether a run needs the input, whether it is advanced, the range and
// the choices, and how a value typed in the form or written as text on the command line becomes the JSON value that
// engine/app_binding binds (bindApp stays the module that puts values into the graph; this one only describes and parses).
//
// Pure: no state, no window, no file written. Nothing here changes what a run binds.
//
// Kinds (one vocabulary): image, mask, model, seed, choice, toggle, number, text. The CLI's `type` strings are a mapping of
// these (cliTypeOf): image, mask, model, choice and number are the same, a seed is "number", a toggle is "boolean" (the port type), and text is the
// target's port type ("text"), or "any" when the input has no target spec.

#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QVariant>

#include <functional>
#include <optional>

#include "engine/document.h"
#include "engine/error.h"
#include "engine/input_binding.h"
#include "engine/node_type.h"
#include "engine/port_type.h"
#include "engine/result.h"

namespace fugue {

class Registry;

namespace app {

struct AppInput {
    InputBinding binding;                // as the block says it, with min, max and choices completed from the target's spec
    const InputSpec *spec = nullptr;     // the first target's spec, through a group instance to its inner input (spec 5.4); owned by the registry or the document
    QString kind;                        // image, mask, model, seed, choice, toggle, number or text
    QString portType;                    // the target's port type id ("number", "text", ...); empty with no spec
    std::optional<PortType> resource;    // the resource type a model input takes (a resource port); none otherwise
    QJsonValue held;                     // what the workflow's first target holds: a literal, a model handle or {"auto": "random"}; Undefined for none and for an optional picture (#484)
    QJsonValue defaultValue;             // what the input holds when nothing is given: the block's default, else `held`, else the spec's default
    QJsonValue own;                      // the block's default, else `held`: what the input shows before anything is typed (no spec default)
    QJsonValue initial;                  // what a form starts on (a toggle's bool, a choice in force, a seed's number; Undefined for none)
    bool required = false;               // a run cannot do without it: a picture or mask with none, a prompt with no fallback
    bool advanced = false;               // section Advanced
    bool optionalPicture = false;        // an optional image or mask: left empty it is left out of the run, its branch bypassed (#484)
    bool randomSeed = false;             // a seed the workflow leaves to the engine ({"auto": "random"})

    // The value a run binds when `values` (by key) are given: the one given, else `defaultValue`.
    QJsonValue valueIn(const QMap<QString, QJsonValue> &values) const;

    // A value typed in a form (a QML number, text, bool or model handle) as the JSON value to bind. Undefined for none: an empty
    // or not numeric number or seed.
    QJsonValue fromTyped(const QVariant &typed) const;

    // A model given by name: the handle of the file with that name, for the resource type the input takes.
    using ModelLookup = std::function<Result<QJsonObject>(const PortType &, const QString &)>;
    // The text of a `--input` value (already read from a file when it was `@<file>`) as the JSON value to bind: a picture or
    // mask is a path made absolute, which must name a file; a model is a handle as JSON, else a name `lookup` resolves; a
    // number and a seed take a number (a seed also `auto` or `random`, the engine's own); a toggle takes true or false; a
    // choice and text are as typed. Errors are `usage` (the text does not fit) or `file_not_found`.
    Result<QJsonValue> fromText(const QString &text, const ModelLookup &lookup = {}) const;
};

// Nothing, null or empty text: no value (an optional picture left like this is left out, #484).
bool isBlank(const QJsonValue &value);
// A number, or text that is one: what a seed holds ({"auto": "random"} and none are not).
bool isNumber(const QJsonValue &value);
// image or mask: the roles that name a file.
bool isPictureRole(const InputBinding &input);

// The input as it is in `document`. `registry` may be null (no spec, no resource type).
AppInput describeInput(const InputBinding &input, const Document &document, const Registry *registry);

// The CLI's name for the kind (see above).
QString cliTypeOf(const AppInput &input);

} // namespace app
} // namespace fugue
