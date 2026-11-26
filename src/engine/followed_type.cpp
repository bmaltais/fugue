#include "engine/followed_type.h"

namespace fugue {

namespace {

std::optional<PortSpec> typed(const LinkedTypeOf &linked, const QString &input) {
    const std::optional<PortSpec> found = linked(input);
    return found && !found->type.isAny() ? found : std::nullopt;
}

} // namespace

std::optional<PortSpec> followedOutputType(const OutputSpec &output, const LinkedTypeOf &linked) {
    if (!output.type.type.isAny())
        return std::nullopt;
    for (const QString &name : output.follows)
        if (const std::optional<PortSpec> found = typed(linked, name))
            return found;
    return std::nullopt;
}

std::optional<PortSpec> followedInputType(const QList<OutputSpec> &outputs, const QString &input, const LinkedTypeOf &linked) {
    for (const OutputSpec &output : outputs) {
        if (!output.type.type.isAny() || !output.follows.contains(input))
            continue;
        for (const QString &name : output.follows)
            if (name != input)
                if (const std::optional<PortSpec> found = typed(linked, name))
                    return found;
    }
    return std::nullopt;
}

} // namespace fugue
