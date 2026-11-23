#pragma once

// The Move and Free Transform gestures for QML (#236, epic #216): canvas/transform_math.h over the placement maps the Canvas view
// uses (CanvasViewModel::layerBox / transformLayer), with the snap function of the view's guides and grid
// (GuidesModel::targets, canvas/snap.h) put on top. It holds no state of a drag: QML gives it the placement the drag started with
// and the two points, so a drag is the same numbers whether a person or a test makes it.
//
// A placement map is {cx, cy, width, height, angle, flipX, flipY, filter, x, y (the unturned box's top left), corners [{x, y} x4],
// bounds {x, y, width, height}}; the answers carry all of them so the frame can be drawn from the answer alone.

#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/guides_model.h"
#include "canvas/transform_math.h"

namespace fugue {

class TransformGesture : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // The guides and grid a drag is pulled to, and what of them is on now (GuidesModel's options: {guides, grid, zoom}); no guides: no snap.
    Q_PROPERTY(fugue::GuidesModel *guides READ guides WRITE setGuides NOTIFY changed)
    Q_PROPERTY(QVariantMap snapOptions READ snapOptions WRITE setSnapOptions NOTIFY changed)

public:
    explicit TransformGesture(QObject *parent = nullptr) : QObject(parent) {}

    GuidesModel *guides() const { return m_guides; }
    void setGuides(GuidesModel *guides);
    QVariantMap snapOptions() const { return m_options; }
    void setSnapOptions(const QVariantMap &options);

    // `start` is a placement map; `handle` one of "top-left", "top", "top-right", "right", "bottom-right", "bottom", "bottom-left", "left",
    // "move", "rotate"; `from` and `to` are {x, y} in canvas pixels. `snap` pulls the result to the guides and grid. The placement map out.
    Q_INVOKABLE QVariantMap dragged(const QVariantMap &start, const QString &handle, const QVariantMap &from, const QVariantMap &to, bool shift, bool alt, bool snap = true) const;
    // The placement moved by whole pixels (the arrow keys).
    Q_INVOKABLE QVariantMap nudged(const QVariantMap &start, double dx, double dy) const;
    // The placement with `changes` ({cx, cy, width, height, angle, flipX, flipY, filter}, any of them) put in, as the options bar's fields do;
    // `keepRatio` scales the other side with width or height.
    Q_INVOKABLE QVariantMap changed(const QVariantMap &start, const QVariantMap &changes, bool keepRatio) const;
    // The placement map of `placement` (every derived field filled in), also for a map that only has the six numbers.
    Q_INVOKABLE QVariantMap describe(const QVariantMap &placement) const;

    static canvas::LayerTransform placementOf(const QVariantMap &map);
    static QVariantMap mapOf(const canvas::LayerTransform &placement);

signals:
    void changed();

private:
    QPointer<GuidesModel> m_guides;
    QVariantMap m_options;
};

} // namespace fugue
