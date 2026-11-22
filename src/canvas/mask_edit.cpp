// The painted mask and the painted canvas of a node (spec 14.4, #174 and #181): which input holds
// them and how the window's result is set. Two widgets, one shape: an input file in the inputs folder.

#include "canvas/canvas_model.h"
#include "canvas/widget_edit.h"

using namespace fugue;

namespace {
const char *const kMaskWidget = "mask";
const char *const kPaintWidget = "paint";
} // namespace

QString CanvasModel::widgetInput(const QString &node, const QString &widget) const {
    const int row = rowOf(node);
    if (row < 0)
        return {};
    for (const Port &port : m_rows[size_t(row)].inputs)
        if (port.widgetType == widget)
            return port.name;
    return {};
}

QVariantMap CanvasModel::fileWidgetState(const QString &node, const QString &widget) const {
    const QString input = widgetInput(node, widget);
    if (input.isEmpty())
        return {};
    const int row = rowOf(node);
    QString over;
    for (const Port &port : m_rows[size_t(row)].inputs)
        if (port.name == input)
            over = port.over;
    const InputValue value = m_document.graph.nodes.value(node).inputs.value(input);
    const bool linked = value.kind() == InputValue::Kind::Link;
    const QString path = value.kind() == InputValue::Kind::Literal ? value.literal().toString() : QString();
    return {{"input", input}, {"over", over}, {"path", path}, {"editable", !linked}};
}

QString CanvasModel::maskInput(const QString &node) const { return widgetInput(node, QLatin1String(kMaskWidget)); }

QVariantMap CanvasModel::maskState(const QString &node) const { return fileWidgetState(node, QLatin1String(kMaskWidget)); }

QString CanvasModel::setMaskPath(const QString &node, const QString &path) {
    const QString input = maskInput(node);
    if (input.isEmpty())
        return tr("This node has no mask to paint.");
    if (maskState(node).value("path").toString() == path)
        return {};
    return widgets()->setText(node, input, path);
}

QString CanvasModel::paintInput(const QString &node) const { return widgetInput(node, QLatin1String(kPaintWidget)); }

QVariantMap CanvasModel::paintState(const QString &node) const {
    QVariantMap state = fileWidgetState(node, QLatin1String(kPaintWidget));
    if (state.isEmpty())
        return state;
    const Node instance = m_document.graph.nodes.value(node);
    const auto size = [&](const char *name) {
        const InputValue value = instance.inputs.value(QLatin1String(name));
        return value.kind() == InputValue::Kind::Literal && value.literal().isDouble() ? int(value.literal().toDouble()) : 0;
    };
    state.insert("linked", instance.inputs.value(state.value("over").toString()).kind() == InputValue::Kind::Link);
    state.insert("width", size("width"));
    state.insert("height", size("height"));
    return state;
}

QString CanvasModel::setPaintPath(const QString &node, const QString &path) {
    const QString input = paintInput(node);
    if (input.isEmpty())
        return tr("This node has nothing to paint.");
    if (paintState(node).value("path").toString() == path)
        return {};
    return widgets()->setText(node, input, path);
}
