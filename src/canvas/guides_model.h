#pragma once

// The guides and grid of the canvas a Canvas view shows (#253, epic #216), and the one snap function others call. A
// guide is a line on the canvas, in canvas pixels, kept per canvas in a small sidecar file (engine/canvas_guides.h,
// CanvasStore::guides): not a revision, so moving one is not an Undo step and never touches the picture. This model
// reads it when the canvas changes, writes it at each change (a guide is a few bytes) and says so by `guidesChanged`;
// it knows nothing of rulers, zoom or the window. A file that does not validate reads as no guides, and `problem` says why.
//
// `snapRect` and `snapPoint` (canvas/snap.h) put a rectangle or a point on the guides, the grid and the Region node's
// Multiple of: `options` is {multiple (the Region's Multiple of, 0 for none), guides: bool, grid: bool, zoom (view
// units per canvas pixel; a line pulls from `snapDistance` screen pixels)}. The Region frame calls it through the view's
// `constrain`; a selection and Paste or Move (#236) call the same function.

#include <QObject>
#include <QSize>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/snap.h"
#include "engine/canvas_guides.h"

namespace fugue {

class GuidesModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // The folder of the store; empty is the session's (`<data>/canvases`). A test sets its own.
    Q_PROPERTY(QString storeRoot READ storeRoot WRITE setStoreRoot NOTIFY sourceChanged)
    Q_PROPERTY(QString canvasId READ canvasId WRITE setCanvasId NOTIFY sourceChanged)
    // The canvas's size, so a guide can be put only on it and snap keeps a rectangle inside it.
    Q_PROPERTY(QSize canvasSize READ canvasSize WRITE setCanvasSize NOTIFY guidesChanged)
    // [{axis: "x" | "y", at}], vertical lines first, by position.
    Q_PROPERTY(QVariantList guides READ guides NOTIFY guidesChanged)
    Q_PROPERTY(int gridSpacing READ gridSpacing NOTIFY guidesChanged)
    Q_PROPERTY(int minGrid READ minGrid CONSTANT)
    Q_PROPERTY(int maxGrid READ maxGrid CONSTANT)
    Q_PROPERTY(int defaultGrid READ defaultGrid CONSTANT)
    // Why the sidecar could not be read or written ("" when it can).
    Q_PROPERTY(QString problem READ problem NOTIFY guidesChanged)
    // How far, in screen pixels, a line pulls.
    Q_PROPERTY(double snapDistance READ snapDistance WRITE setSnapDistance NOTIFY guidesChanged)

public:
    explicit GuidesModel(QObject *parent = nullptr) : QObject(parent) {}

    QString storeRoot() const { return m_root; }
    void setStoreRoot(const QString &root);
    QString canvasId() const { return m_canvas; }
    void setCanvasId(const QString &id);
    QSize canvasSize() const { return m_size; }
    void setCanvasSize(const QSize &size);
    QVariantList guides() const;
    int gridSpacing() const { return m_set.grid; }
    int minGrid() const { return canvas::minGridSpacing; }
    int maxGrid() const { return canvas::maxGridSpacing; }
    int defaultGrid() const { return canvas::defaultGridSpacing; }
    QString problem() const { return m_problem; }
    double snapDistance() const { return m_distance; }
    void setSnapDistance(double pixels);

    // A guide at `at` on `axis` ("x": vertical, "y": horizontal). False when it is off the canvas, a repeat, one too many or
    // not written. Returns the guide's index in `guides` through `moveGuide`/`removeGuide`'s numbering.
    Q_INVOKABLE bool addGuide(const QString &axis, int at);
    // Moves the guide at `index` (as `guides` lists it); false when it is off the canvas or the index is none.
    Q_INVOKABLE bool moveGuide(int index, int at);
    Q_INVOKABLE bool removeGuide(int index);
    Q_INVOKABLE bool clearGuides();
    // Sets the grid's spacing, canvas pixels, kept within minGrid..maxGrid; false when it is the same or not written.
    Q_INVOKABLE bool setGridSpacing(int spacing);

    // The snap function (canvas/snap.h), with the guides and grid of this canvas. `rect` is {x, y, width, height}.
    Q_INVOKABLE QVariantMap snapRect(const QVariantMap &rect, bool keepSize, const QVariantMap &options) const;
    // `point` is {x, y}; answers {x, y}.
    Q_INVOKABLE QVariantMap snapPoint(const QVariantMap &point, const QVariantMap &options) const;
    // The targets `options` ask for, for a caller that works on plain structs.
    snap::Targets targets(const QVariantMap &options) const;

signals:
    void sourceChanged();
    void guidesChanged();

private:
    void load();
    bool save(const canvas::GuideSet &next);
    bool onCanvas(bool vertical, int at) const;

    QString m_root, m_canvas;
    QSize m_size;
    canvas::GuideSet m_set;
    QString m_problem;
    double m_distance = 8;
};

} // namespace fugue
