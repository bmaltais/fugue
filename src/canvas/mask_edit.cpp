// The painted mask of a node (spec 14.4, #174): which input holds it and how the editor's result is set.

#include "canvas/canvas_model.h"
#include "canvas/widget_edit.h"

using namespace fugue;

namespace {
const char *const kMaskWidget = "mask";
}

QString CanvasModel::maskInput(const QString &node) const {
    const int row = rowOf(node);
    if (row < 0)
        return {};
    for (const Port &port : m_rows[size_t(row)].inputs)
        if (port.widgetType == QLatin1String(kMaskWidget))
            return port.name;
    return {};
}

QVariantMap CanvasModel::maskState(const QString &node) const {
    const QString input = maskInput(node);
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

QString CanvasModel::setMaskPath(const QString &node, const QString &path) {
    const QString input = maskInput(node);
    if (input.isEmpty())
        return tr("This node has no mask to paint.");
    if (maskState(node).value("path").toString() == path)
        return {};
    return widgets()->setText(node, input, path);
}
