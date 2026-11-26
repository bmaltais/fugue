#pragma once

#include <QString>
#include <functional>
#include <optional>

#include "engine/node_type.h"

namespace fugue {

// The type a node's inputs have by what is linked to them, or none when nothing is known (nothing linked, or the
// source's own type is not resolved). Asked by name.
using LinkedTypeOf = std::function<std::optional<PortSpec>(const QString &input)>;

// The type an output of type "any" with `follows` resolves to: the type of the first followed input that has one
// (the compiler's rule, `Compiler::followed`, without its errors). None when no followed input has a type, so the
// output stays a wildcard (`compatibility()`), as does any output that is not of type "any".
std::optional<PortSpec> followedOutputType(const OutputSpec &output, const LinkedTypeOf &linked);

// The type a followed input must have because of the others: the first type found on an input that shares an
// output's `follows` with it, other than itself. None when it follows nothing or nothing else is typed, so
// the input stays open. Dragging into a Switch side whose other side is linked accepts only that side's type.
std::optional<PortSpec> followedInputType(const QList<OutputSpec> &outputs, const QString &input, const LinkedTypeOf &linked);

} // namespace fugue
