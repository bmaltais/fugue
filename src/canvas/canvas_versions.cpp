// The canvas model's offer to move a node to a newer major of its type (spec 3.6, #90, #118):
// a node runs on the major it recorded while that major is installed; when a newer one is, the
// node says so (the "Move to" chip) and moving shows what changes first, then rewrites the
// node's `version`, one undo step. What is a newer major, and what changes, is
// engine/node_versions.h.

#include "canvas/canvas_model.h"

#include "engine/node_versions.h"
#include "engine/registry.h"

using namespace fugue;

QString CanvasModel::newerMajor(const QString &node) const {
    const auto it = m_document.graph.nodes.constFind(node);
    if (it == m_document.graph.nodes.constEnd() || !m_registry || m_document.group(it->type))
        return {};
    const std::optional<Version> newest = newerMajorFor(*m_registry, *it);
    return newest ? newest->toString() : QString();
}

QVariantMap CanvasModel::majorMove(const QString &node) const {
    if (newerMajor(node).isEmpty())
        return {};
    const Node &made = m_document.graph.nodes.value(node);
    const NodeType *from = m_registry->nodeTypeAt(made.type, made.version.major);
    const NodeType *to = m_registry->nodeType(made.type);
    const PortChanges changes = portChanges(*from, *to);
    return {{"name", to->name},
            {"from", made.version.toString()},
            {"to", to->version.toString()},
            {"addedInputs", changes.addedInputs},
            {"removedInputs", changes.removedInputs},
            {"addedOutputs", changes.addedOutputs},
            {"removedOutputs", changes.removedOutputs}};
}

bool CanvasModel::moveToNewerMajor(const QString &node) {
    const std::optional<Version> newest = Version::parse(newerMajor(node));
    if (!newest)
        return false;
    Document document = scopeDocument();
    document.graph.nodes[node].version = *newest;
    commit(document, tr("Move to newer version"));
    return true;
}

void CanvasModel::requestMajorMove() {
    const QStringList ids = selectedIds();
    if (ids.size() == 1 && !newerMajor(ids.first()).isEmpty())
        emit nodeMoveAsked(ids.first(), majorMove(ids.first()));
}
