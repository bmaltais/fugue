#include "engine/canvas_layer_ops.h"

#include <algorithm>
#include <set>

#include "engine/paint_document.h"

namespace fugue::canvas {

namespace {

constexpr int kMaxName = 200; // as a layer name in an .ora (engine/paint_file.cpp)

struct OpName {
    LayerOp op;
    const char *name;
};

constexpr OpName kOps[] = {
    {LayerOp::Visible, "visible"},   {LayerOp::Opacity, "opacity"},        {LayerOp::Blend, "blend"},
    {LayerOp::Rename, "rename"},     {LayerOp::Remove, "delete"},          {LayerOp::Move, "move"},
    {LayerOp::Duplicate, "duplicate"}, {LayerOp::MaskEnabled, "mask-enabled"}, {LayerOp::RemoveMask, "mask-delete"},
    {LayerOp::SetMask, "mask-set"},   {LayerOp::New, "new"},               {LayerOp::Group, "group"},
    {LayerOp::Ungroup, "ungroup"},    {LayerOp::MergeDown, "merge-down"},  {LayerOp::Flatten, "flatten"},
    {LayerOp::Properties, "properties"}, {LayerOp::Lock, "lock"},
};

Error invalid(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("input_invalid"), message, hint); }

} // namespace

Error lockedError(const LayerRecord &layer, const QString &what) {
    return invalid(QStringLiteral("%1 is locked: %2.").arg(layer.name, what), QStringLiteral("Unlock it first (the lock in the Layers panel, or `fugue canvas layer set <canvas> <layer> --lock false`)."));
}

int topLayerNamed(const std::vector<LayerFace> &layers, const QString &name, QStringList *names) {
    for (int i = int(layers.size()) - 1; i >= 0; --i) {
        if (names)
            *names << layers[size_t(i)].name;
        if (layers[size_t(i)].name == name && layers[size_t(i)].kind != mask::LayerKind::Picture)
            return i;
    }
    return -1;
}

std::vector<LayerFace> layerFaces(const Tree &tree) {
    std::vector<LayerFace> out;
    for (const LayerRecord &layer : tree.layers)
        out.push_back({layer.name, layer.kind, layer.group});
    return out;
}

QString noPixelsReason(const LayerFace &layer) {
    if (layer.group)
        return QStringLiteral("%1 is a group: it has no pixels of its own to write.").arg(layer.name);
    if (layer.kind == mask::LayerKind::Mask)
        return QStringLiteral("%1 is a mask layer: it has no picture to write.").arg(layer.name);
    return {};
}

bool layerOpNeedsPixels(LayerOp op) { return op == LayerOp::MergeDown || op == LayerOp::Flatten; }

QString layerOpName(LayerOp op) {
    for (const OpName &o : kOps)
        if (o.op == op)
            return QString::fromLatin1(o.name);
    return {};
}

bool layerOpFromName(const QString &name, LayerOp *op) {
    for (const OpName &o : kOps)
        if (name == QLatin1String(o.name)) {
            *op = o.op;
            return true;
        }
    return false;
}

QString duplicateName(const Tree &tree, const QString &name) {
    const auto taken = [&tree](const QString &candidate) {
        return std::any_of(tree.layers.begin(), tree.layers.end(), [&candidate](const LayerRecord &l) { return l.name == candidate; });
    };
    QString out = QStringLiteral("%1 copy").arg(name).left(kMaxName);
    for (int n = 2; taken(out) && n < 1000; ++n)
        out = QStringLiteral("%1 copy %2").arg(name).arg(n).right(kMaxName);
    return out;
}

QString numberedName(const Tree &tree, const QString &stem) {
    const auto taken = [&tree](const QString &candidate) {
        return std::any_of(tree.layers.begin(), tree.layers.end(), [&candidate](const LayerRecord &l) { return l.name == candidate; });
    };
    int n = int(tree.layers.size()) + 1;
    while (taken(QStringLiteral("%1 %2").arg(stem).arg(n)) && n < 100000)
        ++n;
    return QStringLiteral("%1 %2").arg(stem).arg(n);
}

namespace {

// Applies an edit to the layers of one level: `base` is how many groups deep that level lies (a Group there nests that much further).
Result<Tree> applyLevel(const Tree &tree, const LayerEdit &edit, int base);

// What a locked group protects inside it: the stack and the pixels, not the look (showing, opacity, blend, names, the mask switch).
bool protectedInLockedGroup(LayerOp op) {
    switch (op) {
    case LayerOp::Remove:
    case LayerOp::Move:
    case LayerOp::Duplicate:
    case LayerOp::RemoveMask:
    case LayerOp::SetMask:
    case LayerOp::New:
    case LayerOp::Group:
    case LayerOp::Ungroup:
        return true;
    default:
        return false;
    }
}

// An edit of the layers inside the groups named by `edit.parent`: the same edit on the group's children, which are put back.
Result<Tree> applyInside(const Tree &tree, const LayerEdit &edit) {
    if (edit.op == LayerOp::Properties)
        return invalid(QStringLiteral("Layer properties name the group of each step, not their own."));
    LayerEdit inner = edit;
    inner.parent.clear();
    // Walk down: a copy of each group on the way is rewritten with its changed children.
    std::vector<const LayerRecord *> chain;
    const std::vector<LayerRecord> *level = &tree.layers;
    const LayerRecord *lockedGroup = nullptr;
    for (int index : edit.parent) {
        if (index < 0 || index >= int(level->size()) || !(*level)[size_t(index)].group)
            return invalid(QStringLiteral("There is no group at %1.").arg(index), QStringLiteral("`fugue canvas layer list <canvas>` lists the layers."));
        const LayerRecord &group = (*level)[size_t(index)];
        if (group.locked && !lockedGroup)
            lockedGroup = &group;
        chain.push_back(&group);
        level = &group.children;
    }
    if (lockedGroup && protectedInLockedGroup(edit.op))
        return lockedError(*lockedGroup, QStringLiteral("what is inside it cannot be added, removed, moved or have its layer mask changed"));
    Tree sub;
    sub.width = tree.width;
    sub.height = tree.height;
    sub.layers = *level;
    Result<Tree> changed = applyLevel(sub, inner, int(edit.parent.size()));
    if (!changed)
        return changed;
    Tree out = tree;
    std::vector<LayerRecord> *at = &out.layers;
    for (int index : edit.parent) {
        LayerRecord &group = (*at)[size_t(index)];
        at = &group.children;
    }
    *at = std::move(changed.value().layers);
    return out;
}

} // namespace

Result<Tree> applyLayerEdit(const Tree &tree, const LayerEdit &edit) {
    if (!edit.parent.empty())
        return applyInside(tree, edit);
    return applyLevel(tree, edit, 0);
}

namespace {

Result<Tree> applyLevel(const Tree &tree, const LayerEdit &edit, int base) {
    const int count = int(tree.layers.size());
    if (layerOpNeedsPixels(edit.op))
        return invalid(QStringLiteral("Merge down and Flatten read pixels: they go through CanvasStore::editLayers."));
    if (edit.op == LayerOp::Properties) {
        if (edit.steps.empty())
            return invalid(QStringLiteral("Layer properties name nothing to change."));
        Tree out = tree;
        for (const LayerEdit &step : edit.steps) {
            if (step.op == LayerOp::Properties || layerOpNeedsPixels(step.op) || step.op == LayerOp::SetMask)
                return invalid(QStringLiteral("Layer properties are made of a name, an opacity, a blend and a mask switch."));
            Result<Tree> next = applyLayerEdit(out, step);
            if (!next)
                return next;
            out = std::move(next.value());
        }
        return out;
    }
    if (edit.op == LayerOp::New) {
        if (edit.layers.size() > 1 || (edit.layers.size() == 1 && (edit.layers.front() < 0 || edit.layers.front() >= count)))
            return invalid(QStringLiteral("A new layer goes above one layer, or on top."));
        const QString name = edit.text.trimmed().isEmpty() ? numberedName(tree, QStringLiteral("Layer")) : edit.text.trimmed();
        if (name.size() > kMaxName)
            return invalid(QStringLiteral("A layer's name is 1 to %1 characters.").arg(kMaxName));
        Tree out = tree;
        LayerRecord layer;
        layer.name = name;
        out.layers.insert(out.layers.begin() + (edit.layers.empty() ? count : edit.layers.front() + 1), layer);
        return out;
    }
    if (edit.layers.empty())
        return invalid(QStringLiteral("No layer is named."));
    std::set<int> chosen;
    for (int position : edit.layers) {
        if (position < 0 || position >= count)
            return invalid(QStringLiteral("There is no layer %1: the revision has %2 (0 is the bottom).").arg(position).arg(count == 1 ? QStringLiteral("1 layer") : QStringLiteral("%1 layers").arg(count)),
                           QStringLiteral("`fugue canvas layer list <canvas>` lists them with their positions."));
        if (!chosen.insert(position).second)
            return invalid(QStringLiteral("Layer %1 is named twice.").arg(position));
    }
    Tree out = tree;
    switch (edit.op) {
    case LayerOp::Visible:
        for (int p : chosen)
            out.layers[size_t(p)].visible = edit.flag;
        break;
    case LayerOp::Opacity:
        if (!(edit.number >= 0 && edit.number <= 1))
            return invalid(QStringLiteral("An opacity of %1 is outside 0 to 1.").arg(edit.number));
        for (int p : chosen)
            out.layers[size_t(p)].opacity = edit.number;
        break;
    case LayerOp::Blend: {
        mask::Blend blend = mask::Blend::Normal;
        if (!mask::blendFromId(edit.text, &blend))
            return invalid(QStringLiteral("\"%1\" is not a blend mode.").arg(edit.text.left(40)),
                           QStringLiteral("Use normal, multiply, screen, overlay, soft-light, darken, lighten or add."));
        for (int p : chosen)
            out.layers[size_t(p)].compositeOp = blend == mask::Blend::Normal ? QString() : mask::opOfBlend(blend);
        break;
    }
    case LayerOp::Lock:
        for (int p : chosen)
            out.layers[size_t(p)].locked = edit.flag;
        break;
    case LayerOp::Rename: {
        const QString name = edit.text.trimmed();
        if (chosen.size() != 1)
            return invalid(QStringLiteral("Rename takes one layer."));
        if (name.isEmpty() || name.size() > kMaxName)
            return invalid(QStringLiteral("A layer's name is 1 to %1 characters.").arg(kMaxName));
        out.layers[size_t(*chosen.begin())].name = name;
        break;
    }
    case LayerOp::Remove:
        for (int p : chosen)
            if (tree.layers[size_t(p)].locked)
                return lockedError(tree.layers[size_t(p)], QStringLiteral("it cannot be deleted"));
        for (auto it = chosen.rbegin(); it != chosen.rend(); ++it)
            out.layers.erase(out.layers.begin() + *it);
        break;
    case LayerOp::Move: {
        std::vector<LayerRecord> moved, rest;
        for (int p = 0; p < count; ++p)
            (chosen.count(p) ? moved : rest).push_back(tree.layers[size_t(p)]);
        const int at = std::clamp(edit.to, 0, int(rest.size()));
        rest.insert(rest.begin() + at, moved.begin(), moved.end());
        out.layers = std::move(rest);
        break;
    }
    case LayerOp::Duplicate:
        // Each copy goes right above its layer; from the top down, so the positions below stay right.
        for (auto it = chosen.rbegin(); it != chosen.rend(); ++it) {
            LayerRecord copy = tree.layers[size_t(*it)];
            copy.name = duplicateName(out, copy.name);
            out.layers.insert(out.layers.begin() + *it + 1, copy);
        }
        break;
    case LayerOp::MaskEnabled:
    case LayerOp::RemoveMask:
        for (int p : chosen) {
            LayerRecord &layer = out.layers[size_t(p)];
            if (!layer.hasMask)
                return invalid(QStringLiteral("Layer %1 (%2) has no layer mask.").arg(p).arg(layer.name));
            if (edit.op == LayerOp::RemoveMask && layer.locked)
                return lockedError(layer, QStringLiteral("its layer mask cannot be deleted"));
            if (edit.op == LayerOp::MaskEnabled) {
                layer.maskEnabled = edit.flag;
            } else {
                layer.hasMask = false;
                layer.maskEnabled = true;
                layer.maskTiles.clear();
            }
        }
        break;
    case LayerOp::Group: {
        int deepest = 0;
        for (int p : chosen) {
            const LayerRecord &layer = tree.layers[size_t(p)];
            if (layer.kind != mask::LayerKind::Paint)
                return invalid(QStringLiteral("Layer %1 (%2) is a %3 layer: only paint layers and groups can be grouped.")
                                   .arg(p).arg(layer.name, layer.kind == mask::LayerKind::Mask ? QStringLiteral("mask") : QStringLiteral("picture")));
            deepest = std::max(deepest, groupDepth(layer));
        }
        if (deepest + 1 + base > maxGroupDepth)
            return invalid(QStringLiteral("Groups nest at most %1 deep.").arg(maxGroupDepth), QStringLiteral("Ungroup one of the groups first."));
        const QString name = edit.text.trimmed().isEmpty() ? numberedName(tree, QStringLiteral("Group")) : edit.text.trimmed();
        if (name.size() > kMaxName)
            return invalid(QStringLiteral("A layer's name is 1 to %1 characters.").arg(kMaxName));
        LayerRecord group;
        group.name = name;
        group.group = true;
        for (int p : chosen)
            group.children.push_back(tree.layers[size_t(p)]);
        const int at = *chosen.rbegin(); // the place of the highest chosen layer, once the others are out
        std::vector<LayerRecord> layers;
        for (int p = 0; p < count; ++p) {
            if (p == at)
                layers.push_back(group);
            else if (!chosen.count(p))
                layers.push_back(tree.layers[size_t(p)]);
        }
        out.layers = std::move(layers);
        break;
    }
    case LayerOp::Ungroup: {
        for (int p : chosen)
            if (!tree.layers[size_t(p)].group)
                return invalid(QStringLiteral("Layer %1 (%2) is not a group.").arg(p).arg(tree.layers[size_t(p)].name));
            else if (tree.layers[size_t(p)].locked)
                return lockedError(tree.layers[size_t(p)], QStringLiteral("it cannot be ungrouped"));
            else if (const LayerRecord &g = tree.layers[size_t(p)]; !g.visible || g.opacity != 1 || !g.compositeOp.isEmpty() || g.hasMask)
                return invalid(QStringLiteral("The group %1 is hidden, has an opacity or blend mode of its own, or has a layer mask: ungrouping would lose that.").arg(g.name),
                               QStringLiteral("Show it, and set its opacity to 100%, its blend to Normal and delete its mask first (Layer properties…)."));
        std::vector<LayerRecord> layers;
        for (int p = 0; p < count; ++p) {
            if (chosen.count(p))
                layers.insert(layers.end(), tree.layers[size_t(p)].children.begin(), tree.layers[size_t(p)].children.end());
            else
                layers.push_back(tree.layers[size_t(p)]);
        }
        out.layers = std::move(layers);
        break;
    }
    case LayerOp::SetMask: {
        if (chosen.size() != 1)
            return invalid(QStringLiteral("A layer mask is set on one layer at a time."));
        LayerRecord &layer = out.layers[size_t(*chosen.begin())];
        if (layer.locked)
            return lockedError(layer, QStringLiteral("its layer mask cannot be painted or replaced"));
        if (layer.kind == mask::LayerKind::Mask)
            return invalid(QStringLiteral("Layer %1 (%2) is a mask layer: it has no layer mask.").arg(*chosen.begin()).arg(layer.name));
        for (const auto &entry : edit.maskTiles)
            if (entry.first.x >= tilesAcross(tree.width) || entry.first.y >= tilesDown(tree.height) || entry.second.isEmpty())
                return invalid(QStringLiteral("A tile of the mask lies outside the canvas."));
        layer.hasMask = true;
        layer.maskEnabled = true;
        layer.maskTiles = edit.maskTiles;
        break;
    }
    case LayerOp::New:
    case LayerOp::MergeDown:
    case LayerOp::Flatten:
    case LayerOp::Properties:
        break; // handled above
    }
    return out;
}

} // namespace

} // namespace fugue::canvas
