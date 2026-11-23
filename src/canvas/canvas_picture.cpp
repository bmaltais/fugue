// Node pictures (#263): which nodes show one, and the per-node override in the layout.

#include <algorithm>
#include <QJsonObject>

#include "canvas/canvas_model.h"
#include "engine/registry.h"

using namespace fugue;

void CanvasModel::setPictureMode(PictureMode mode) {
    if (mode == m_pictureMode)
        return;
    m_pictureMode = mode;
    emit pictureModeChanged();
}

PictureOverride CanvasModel::pictureOverride(const QString &node) const {
    const int r = rowOf(node);
    return r < 0 ? PictureOverride::Default : m_rows[size_t(r)].picture;
}

bool CanvasModel::canHavePicture(const QString &node) const {
    if (rowOf(node) < 0)
        return false;
    const Node &found = m_document.graph.nodes.value(node);
    const NodeType *type = m_registry ? m_registry->nodeTypeFor(found.type, found.version.major) : nullptr;
    if (!type)
        return isGroup(node);
    return hasPictureSlot(*type);
}

bool CanvasModel::showsPicture(const QString &node) const {
    const int r = rowOf(node);
    if (r < 0)
        return true;
    const Node &found = m_document.graph.nodes.value(node);
    const NodeType *type = m_registry ? m_registry->nodeTypeFor(found.type, found.version.major) : nullptr;
    // A group instance shows the image its group makes, as a node that makes a result does; a type
    // that is not installed (a placeholder) has no role: the mode decides.
    const PictureRole role = type && !isGroup(node) ? pictureRole(*type) : PictureRole::Result;
    return ::fugue::showsPicture(m_pictureMode, role, type && !isGroup(node) && isInputNode(*type), m_rows[size_t(r)].picture);
}

bool CanvasModel::pictureCommandApplies(PictureOverride value) const {
    const QStringList ids = selectedIds();
    return std::any_of(ids.begin(), ids.end(), [&](const QString &id) {
        if (!canHavePicture(id))
            return false;
        switch (value) {
        case PictureOverride::Show:
            return !showsPicture(id);
        case PictureOverride::Hide:
            return showsPicture(id);
        case PictureOverride::Default:
            break;
        }
        return pictureOverride(id) != PictureOverride::Default;
    });
}

void CanvasModel::setPictureOverride(const QString &value) {
    const PictureOverride wanted = pictureOverrideFromName(value);
    Document document = scopeDocument();
    QJsonObject entries = document.layout.value("nodes").toObject();
    bool changed = false;
    for (const QString &id : selectedIds()) {
        if (!canHavePicture(id) || pictureOverride(id) == wanted)
            continue;
        QJsonObject entry = entries.value(id).toObject();
        if (wanted == PictureOverride::Default)
            entry.remove("picture");
        else
            entry.insert("picture", pictureOverrideName(wanted));
        entries.insert(id, entry);
        changed = true;
    }
    if (!changed)
        return;
    document.layout.insert("nodes", entries);
    commit(document, wanted == PictureOverride::Show ? tr("Show picture")
                     : wanted == PictureOverride::Hide ? tr("Hide picture") : tr("Default picture"));
}

