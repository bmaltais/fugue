#pragma once

// The mask editor's layer (#172, spec 14.4, 12.2): a QQuickItem that holds a MaskPaint (the pure
// core, engine/mask_paint.h) and draws it over a picture. It is a scene graph item, not a Canvas
// or a QQuickPaintedItem: the mask is cut into 256 px cells, each its own texture node, so a stroke
// uploads only the cells it touched, never the picture. The mask is tinted (the overlay) or drawn as
// white on black (`maskOnly`, the H view).
//
// It knows nothing of nodes or of the dialog: the editor's QML sends it picture-pixel points and asks
// for undo, invert and the saved file. Where the picture is in the item is `originX`, `originY` and
// `zoom` (screen pixels per picture pixel), as CropFrame is given them.

#include <QColor>
#include <QQuickItem>
#include <QSize>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

#include "canvas/cell_grid.h"
#include "engine/mask_paint.h"

namespace fugue {

class MaskLayer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    // The picture's size in pixels; setting it starts an empty mask of that size.
    Q_PROPERTY(QSize imageSize READ imageSize WRITE setImageSize NOTIFY imageSizeChanged FINAL)
    Q_PROPERTY(qreal originX READ originX WRITE setOriginX NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal originY READ originY WRITE setOriginY NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewChanged FINAL)
    // The overlay color, with its alpha: the accent at 50% by default.
    Q_PROPERTY(QColor tint READ tint WRITE setTint NOTIFY tintChanged FINAL)
    Q_PROPERTY(bool maskOnly READ maskOnly WRITE setMaskOnly NOTIFY tintChanged FINAL)
    // The tint marks the 0s instead of the 255s (#230): a layer mask's hidden part, Photoshop's red overlay of a mask.
    Q_PROPERTY(bool inverted READ inverted WRITE setInverted NOTIFY tintChanged FINAL)
    // The opacity of a stroke, 0 to 1 (1: a stroke sets the mask fully where the brush is hard).
    Q_PROPERTY(qreal brushOpacity READ brushOpacity WRITE setBrushOpacity NOTIFY tintChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged FINAL)
    // Nothing painted: every pixel is 0.
    Q_PROPERTY(bool empty READ isEmpty NOTIFY historyChanged FINAL)
    // The folder `store` writes into: the inputs folder (paths.h) unless a caller says otherwise.
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged FINAL)

public:
    explicit MaskLayer(QQuickItem *parent = nullptr);
    ~MaskLayer() override;

    QSize imageSize() const { return {m_paint->width(), m_paint->height()}; }
    void setImageSize(const QSize &size);
    qreal originX() const { return m_originX; }
    void setOriginX(qreal v);
    qreal originY() const { return m_originY; }
    void setOriginY(qreal v);
    qreal zoom() const { return m_zoom; }
    void setZoom(qreal v);
    QColor tint() const { return m_tint; }
    void setTint(const QColor &color);
    bool maskOnly() const { return m_maskOnly; }
    void setMaskOnly(bool on);
    bool inverted() const { return m_inverted; }
    void setInverted(bool on);
    qreal brushOpacity() const { return m_brushOpacity; }
    void setBrushOpacity(qreal opacity);
    // The mask itself, for a caller that stores it (the Canvas view's layer mask and selection, #230), and a mask to start
    // from: its size becomes the layer's, and the history starts here (as loadMask).
    const mask::MaskPaint &paint() const { return *m_paint; }
    void setPaint(const mask::MaskPaint &paint);
    bool canUndo() const { return m_paint->canUndo(); }
    bool canRedo() const { return m_paint->canRedo(); }
    bool isEmpty() const { return m_paint->isEmpty(); }
    QString folder() const;
    void setFolder(const QString &folder);

    // Reads the mask of a file (a path or a file URL; an 8-bit gray PNG, or any picture: its first channel) as the start
    // of the session, scaled to the picture's size when it differs. False when it cannot be read.
    // It is not an undo step, and the history starts here.
    Q_INVOKABLE bool loadMask(const QString &path);
    // Starts again with an empty mask and no history.
    Q_INVOKABLE void reset();

    // Pointer input, in picture pixels (QML maps the pointer with the viewer). A stroke is one
    // undo step. `size` is the brush's diameter in picture pixels, `hardness` and `opacity` 0 to 1.
    Q_INVOKABLE void beginStroke(bool erase, double size, double hardness, double x, double y, double pressure = 1);
    Q_INVOKABLE void strokeTo(double x, double y, double pressure = 1);
    Q_INVOKABLE void endStroke();
    // A lasso: `points` is a list of {x, y} (or QPointF) in picture pixels. A rectangle likewise.
    Q_INVOKABLE void fillPolygon(const QVariantList &points, bool erase);
    Q_INVOKABLE void fillRect(double x, double y, double width, double height, bool erase);
    Q_INVOKABLE void invert();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void fill();
    // Select > Modify (#251): the mask as a selection grown or shrunk (`kind` "grow", "shrink"), feathered, made a border or
    // smoothed by `amount` pixels (engine/mask_ops.h); one undo step. False when the kind is unknown or nothing changed.
    Q_INVOKABLE bool modify(const QString &kind, int amount);
    // Transform Selection: moved by (dx, dy) pixels, scaled (1 = as is) and turned (degrees) about the selection's center.
    Q_INVOKABLE bool transformBy(double dx, double dy, double scaleX, double scaleY, double degrees);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // What the mask is at a pixel (0 to 255), for tests and the readout; -1 outside the picture.
    Q_INVOKABLE int valueAt(int x, int y) const;
    // The mask as a PNG, and stored in the folder as `<stem>-<10 hex>.png` (the same mask is one
    // file); the path, or an empty string with `error` set.
    Q_INVOKABLE QByteArray png() const;
    Q_INVOKABLE QString store(const QString &stem);
    // The text of the last failure, for a notice.
    Q_INVOKABLE QString lastError() const { return m_error; }
    // How many cells the last paint node upload sent, for tests (a stroke sends only its own).
    Q_INVOKABLE int uploadedCells() const { return m_cells.uploaded(); }

signals:
    void imageSizeChanged();
    void viewChanged();
    void tintChanged();
    void historyChanged();
    void folderChanged();
    // The mask changed (a stroke dab, a step, an undo): the editor's readout and tests follow it.
    void edited();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &now, const QRectF &before) override;

private:
    void changed(bool history);
    void markAllDirty();

    std::unique_ptr<mask::MaskPaint> m_paint;
    qreal m_originX = 0;
    qreal m_originY = 0;
    qreal m_zoom = 1;
    QColor m_tint = QColor(255, 59, 48, 128);
    bool m_maskOnly = false;
    bool m_inverted = false;
    qreal m_brushOpacity = 1;
    QString m_folder;
    QString m_error;
    CellGrid m_cells;
};

} // namespace fugue
