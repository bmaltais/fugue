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
    // The output the card's excerpt shows: its text is that output's text (cut, with an ellipsis, when huge), whichever comes first in
    // the card's order. With no excerpt of a text to match (none yet), the first text output in the card's order.
    QString excerpt = run->thumbnailText;
    if (excerpt.endsWith(QChar(0x2026)))
        excerpt.chop(1);
    const Port *found = nullptr;
    for (const Port &port : m_rows[size_t(row)].outputs) {
        if (!run->texts.contains(port.name))
            continue;
        if (excerpt.isEmpty() || run->texts.value(port.name).toString().startsWith(excerpt)) {
            found = &port;
            break;
        }
    }
    if (!found)
        return {{"has", false}};
    return {{"has", true}, {"output", found->name}, {"label", found->label}, {"text", run->texts.value(found->name).toString()}};
}

QString CanvasModel::linkedFrom(const QString &node, const QString &input) const {
    const InputValue value = m_document.graph.nodes.value(node).inputs.value(input);
    if (value.kind() != InputValue::Kind::Link)
        return {};
    const QString from = value.from().node;
    return m_document.graph.nodes.contains(from) ? from : QString();
}

