// The Crop node's frame and the picture under a point (#73): the model side of the crop view.

#include "canvas/canvas_model.h"
#include "canvas/widget_edit.h"

using namespace fugue;

namespace {
const char *const kCropInputs[] = {"x", "y", "width", "height"};
}

bool CanvasModel::isCrop(const QString &node) const {
    return m_document.graph.nodes.value(node).type == QLatin1String("fugue/crop");
}

QVariantMap CanvasModel::cropRect(const QString &node) const {
    if (!isCrop(node))
        return {};
    QVariantMap rect{{"editable", true}};
    for (const char *name : kCropInputs) {
        const InputValue value = m_document.graph.nodes.value(node).inputs.value(QLatin1String(name));
        if (value.kind() == InputValue::Kind::Link)
            rect.insert("editable", false);
        QVariant number = widgets()->value(node, QLatin1String(name));
        if (!number.isValid())
            number = widgets()->spec(node, QLatin1String(name)).value("default");
        rect.insert(QLatin1String(name), number.toInt());
    }
    return rect;
}

bool CanvasModel::setCropRect(const QString &node, const QVariantMap &rect) {
    const QVariantMap now = cropRect(node);
    if (now.isEmpty() || !now.value("editable").toBool())
        return false;
    QStringList changed;
    for (const char *name : kCropInputs)
        if (rect.contains(QLatin1String(name)) && rect.value(QLatin1String(name)).toInt() != now.value(QLatin1String(name)).toInt())
            changed << QLatin1String(name);
    if (changed.isEmpty())
        return false;
    beginEdit();
    for (const QString &name : std::as_const(changed))
        widgets()->setValue(node, name, rect.value(name).toInt());
    endEdit(tr("Crop"));
    return true;
}

bool CanvasModel::pictureAt(const QString &node, qreal x, qreal y) const {
    const int row = rowOf(node);
    const auto run = m_run.constFind(node);
    if (row < 0 || run == m_run.constEnd() || run->thumbnail.isEmpty() || !run->thumbnailText.isEmpty())
        return false;
    const NodeRow &r = m_rows[size_t(row)];
    return r.thumbnail.translated(r.pos).contains(x, y);
}

bool CanvasModel::textResultAt(const QString &node, qreal x, qreal y) const {
    const int row = rowOf(node);
    const auto run = m_run.constFind(node);
    if (row < 0 || run == m_run.constEnd() || run->thumbnailText.isEmpty() || run->thumbnailDim || !run->media.isEmpty())
        return false;
    const NodeRow &r = m_rows[size_t(row)];
    return !r.collapsed && r.thumbnail.translated(r.pos).contains(x, y) && resultText(node).value(QStringLiteral("has")).toBool();
}

QVariantMap CanvasModel::resultText(const QString &node) const {
    const int row = rowOf(node);
    const auto run = m_run.constFind(node);
    if (row < 0 || run == m_run.constEnd())
        return {{"has", false}};
    const NodeRow &r = m_rows[size_t(row)];
    // The text the card's excerpt shows: the key the run recorded (a Preview's input: it has no output), its label the input's or the
    // output's. Without one, the first text output in the card's order.
    if (const QString key = run->textKey; !key.isEmpty() && run->texts.contains(key)) {
        QString label = key;
        for (const Port &port : r.outputs)
            if (port.name == key)
                label = port.label;
        if (label == key)
            for (const Port &port : r.inputs)
                if (port.name == key)
                    label = port.label;
        return {{"has", true}, {"output", key}, {"label", label}, {"text", run->texts.value(key).toString()}};
    }
    for (const Port &port : r.outputs)
        if (run->texts.contains(port.name))
            return {{"has", true}, {"output", port.name}, {"label", port.label}, {"text", run->texts.value(port.name).toString()}};
    return {{"has", false}};
}

QString CanvasModel::linkedFrom(const QString &node, const QString &input) const {
    const InputValue value = m_document.graph.nodes.value(node).inputs.value(input);
    if (value.kind() != InputValue::Kind::Link)
        return {};
    const QString from = value.from().node;
    return m_document.graph.nodes.contains(from) ? from : QString();
}

