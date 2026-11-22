#pragma once

// The backend of the region picker window (#213, epic #206, spec 14.5): the canvas at one revision as a
// picture to draw on, and the numbers the rectangle on it means for a Canvas Region (canvas/region_pick.h).
// It reads the store and writes nothing: the window hands the rectangle to WidgetEdit::setRegion.
//
// `load()` resolves `revision` now (head is the head at this moment, as a run names it when it compiles),
// flattens it and keeps a copy of at most `maxSide` pixels on a side as a PNG in the picker's own folder, so a
// large canvas draws and pans fast; `canvasSize` is the real size, which the rectangle is in.
// A rectangle is a map {x, y, width, height} in canvas pixels, as CropGeometry's.

#include <QObject>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/canvas_store.h"

namespace fugue {

class RegionPicker : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // The folder of the store; empty is the session's (`<data>/canvases`). A test sets its own.
    Q_PROPERTY(QString storeRoot READ storeRoot WRITE setStoreRoot)
    Q_PROPERTY(QString canvasId READ canvasId WRITE setCanvasId)
    // "head", a revision's hash or the start of one (the node's Revision input).
    Q_PROPERTY(QString revision READ revision WRITE setRevision)
    // What the node's inputs say (Canvas Region's defaults until set).
    Q_PROPERTY(int padding READ padding WRITE setPadding NOTIFY settingsChanged)
    Q_PROPERTY(double scale READ scale WRITE setScale NOTIFY settingsChanged)
    Q_PROPERTY(double cap READ cap WRITE setCap NOTIFY settingsChanged)
    Q_PROPERTY(int multiple READ multiple WRITE setMultiple NOTIFY settingsChanged)
    // After load(): the full hash and its first 8 digits, the picture and the canvas's real size.
    Q_PROPERTY(QString resolved READ resolved NOTIFY loaded)
    Q_PROPERTY(QString shortRevision READ shortRevision NOTIFY loaded)
    Q_PROPERTY(QUrl source READ source NOTIFY loaded)
    Q_PROPERTY(QSize canvasSize READ canvasSize NOTIFY loaded)
    // Why the canvas could not be read, as a sentence with its hint; empty when it could.
    Q_PROPERTY(QString error READ error NOTIFY loaded)

public:
    explicit RegionPicker(QObject *parent = nullptr) : QObject(parent) {}

    // The longest side of the copy that is drawn.
    static constexpr int maxSide = 2048;

    QString storeRoot() const { return m_root; }
    void setStoreRoot(const QString &root) { m_root = root; }
    QString canvasId() const { return m_canvas; }
    void setCanvasId(const QString &id) { m_canvas = id; }
    QString revision() const { return m_revision; }
    void setRevision(const QString &revision) { m_revision = revision.isEmpty() ? QStringLiteral("head") : revision; }
    int padding() const { return m_padding; }
    void setPadding(int padding) {
        if (m_padding == padding)
            return;
        m_padding = padding;
        emit settingsChanged();
    }
    double scale() const { return m_scale; }
    void setScale(double scale) {
        if (m_scale == scale)
            return;
        m_scale = scale;
        emit settingsChanged();
    }
    double cap() const { return m_cap; }
    void setCap(double cap) {
        if (m_cap == cap)
            return;
        m_cap = cap;
        emit settingsChanged();
    }
    int multiple() const { return m_multiple; }
    void setMultiple(int multiple) {
        if (m_multiple == multiple)
            return;
        m_multiple = multiple;
        emit settingsChanged();
    }
    QString resolved() const { return m_resolved; }
    QString shortRevision() const { return m_resolved.mid(QStringLiteral("sha256:").size(), 8); }
    QUrl source() const { return m_source; }
    QSize canvasSize() const { return m_size; }
    QString error() const { return m_error; }

    // Reads the canvas at `revision` now; true when there is a picture.
    Q_INVOKABLE bool load();

    // The node's rectangle (a width or height of 0 runs to the edge) cut to the canvas; the whole canvas
    // when nothing of it is inside.
    Q_INVOKABLE QVariantMap initial(int x, int y, int width, int height) const;
    // The rectangle with its edges on the node's multiple; `keepSize` moves only the top left.
    Q_INVOKABLE QVariantMap snapped(const QVariantMap &rect, bool keepSize = false) const;
    // The rectangle grown by the padding, as the node cuts it (the dashed box).
    Q_INVOKABLE QVariantMap context(const QVariantMap &rect) const;
    // {width, height, capped}: the size the node redraws the context at, and whether the cap decided it.
    Q_INVOKABLE QVariantMap modelSize(const QVariantMap &rect) const;

signals:
    void loaded();
    void settingsChanged();

private:
    canvas::CanvasStore store() const;
    bool fail(const QString &message);

    QString m_root;
    QString m_canvas;
    QString m_revision = QStringLiteral("head");
    int m_padding = 64;
    double m_scale = 1;
    double m_cap = 1.5;
    int m_multiple = 16;
    QString m_resolved;
    QUrl m_source;
    QSize m_size;
    QString m_error;
    QTemporaryDir m_folder;
};

} // namespace fugue
