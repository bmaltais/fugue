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
    {LayerOp::SetMask, "mask-set"},
};

Error invalid(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("input_invalid"), message, hint); }

} // namespace

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

Result<Tree> applyLayerEdit(const Tree &tree, const LayerEdit &edit) {
    const int count = int(tree.layers.size());
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
            if (edit.op == LayerOp::MaskEnabled) {
                layer.maskEnabled = edit.flag;
            } else {
                layer.hasMask = false;
                layer.maskEnabled = true;
                layer.maskTiles.clear();
            }
        }
        break;
    case LayerOp::SetMask: {
        if (chosen.size() != 1)
            return invalid(QStringLiteral("A layer mask is set on one layer at a time."));
        LayerRecord &layer = out.layers[size_t(*chosen.begin())];
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
    }
    return out;
}

} // namespace fugue::canvas
