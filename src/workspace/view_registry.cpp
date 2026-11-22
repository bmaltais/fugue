#include "workspace/view_registry.h"

#include <QCoreApplication>

namespace fugue {

const QString ViewRegistry::kGraph = QStringLiteral("graph");
const QString ViewRegistry::kCanvas = QStringLiteral("canvas");

ViewRegistry::ViewRegistry(QObject *parent) : QObject(parent) {
    // The node graph: today's whole window body. The window declares it itself (Main.qml), so its area
    // adopts that item and never makes one from the component; one per window (see the header).
    add({kGraph, QT_TRANSLATE_NOOP("View", "Graph"), QStringLiteral("workflow"), QUrl(QStringLiteral("qrc:/canvas/FugueCanvas.qml")),
         QSizeF(160, 120), Multiplicity::OnePerWindow});
    // The picture of a canvas (#221): CanvasView.qml in an area (CanvasArea.qml), following the Graph's selected
    // Canvas Region, its canvas kept in the area's state. Wide enough for its bar's first chips and a picture.
    add({kCanvas, QT_TRANSLATE_NOOP("View", "Canvas"), QStringLiteral("image"), QUrl(QStringLiteral("qrc:/workspace/CanvasArea.qml")),
         QSizeF(240, 180), Multiplicity::Many});
}

void ViewRegistry::add(const ViewType &type) {
    for (ViewType &known : m_types) {
        if (known.id == type.id) {
            known = type;
            return;
        }
    }
    m_types << type;
}

const ViewRegistry::ViewType *ViewRegistry::find(const QString &id) const {
    for (const ViewType &type : m_types)
        if (type.id == id)
            return &type;
    return nullptr;
}

QStringList ViewRegistry::ids() const {
    QStringList ids;
    for (const ViewType &type : m_types)
        ids << type.id;
    return ids;
}

QString ViewRegistry::name(const QString &id) const {
    const ViewType *type = find(id);
    return type ? QCoreApplication::translate("View", type->name) : id;
}

QString ViewRegistry::glyph(const QString &id) const {
    const ViewType *type = find(id);
    return type ? type->glyph : QString();
}

QUrl ViewRegistry::component(const QString &id) const {
    const ViewType *type = find(id);
    return type ? type->component : QUrl();
}

QSizeF ViewRegistry::minimumSize(const QString &id) const {
    const ViewType *type = find(id);
    return type ? type->minimum : kPlaceholderMinimum;
}

QString ViewRegistry::multiplicity(const QString &id) const {
    const ViewType *type = find(id);
    switch (type ? type->multiplicity : Multiplicity::Many) {
    case Multiplicity::OnePerTarget:
        return QStringLiteral("one-per-target");
    case Multiplicity::OnePerWindow:
        return QStringLiteral("one-per-window");
    case Multiplicity::Many:
        break;
    }
    return QStringLiteral("many");
}

} // namespace fugue
