#include "engine/file_inputs.h"

#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue {

void forEachFileLiteral(Document &document, const Registry &registry,
                        const std::function<void(const QString &node, const QString &input, InputValue &value)> &visit) {
    const auto scan = [&](Nodes &nodes) {
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            const NodeType *type = registry.nodeTypeFor(it->type, it->version.major);
            if (!type)
                continue;
            for (auto in = it->inputs.begin(); in != it->inputs.end(); ++in) {
                const InputSpec *spec = type->input(in.key());
                if (spec && spec->holdsInputFile() && in->kind() == InputValue::Kind::Literal &&
                    in->literal().isString())
                    visit(it.key(), in.key(), *in);
            }
        }
    };
    scan(document.graph.nodes);
    for (auto group = document.groups.begin(); group != document.groups.end(); ++group)
        scan(group->nodes);
}

} // namespace fugue
