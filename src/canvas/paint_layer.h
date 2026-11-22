#pragma once

// The paint window's canvas (#181, spec 14.4): a QQuickItem that holds a PaintDocument (the pure core,
// engine/paint_document.h) and draws its layers over each other, as MaskLayer draws one mask. It has
// the same calls as MaskLayer, so the mask editor's pointer, brush, shape, zoom and undo code drives
// either one; what a MaskLayer has no use for (layers, a color) is added here.
//
// The picture (the stack as it looks, 256 px cells, a stroke uploads only the cells it touched) is
// drawn by CellGrid. The paint and picture layers are drawn as the result will be (flatten); the visible
// mask layers are the overlay of the mask editor: `tint` over the picture, or white on black with
// `maskOnly`. `checker` puts a checkerboard under it all, for a canvas with no picture.
//
// A picture layer is a placeholder in the file; `setPicture` gives it the node's input picture to draw.
// It is never painted on. Painting with no paintable layer active does nothing, and `canPaint` says so.
//
// It knows nothing of nodes or of the window: it is sent picture-pixel points, and asked for undo and
// the saved file. Where the picture is in the item is `originX`, `originY` and `zoom`, as MaskLayer's.

#include <QColor>
#include <QQuickItem>
#include <QSize>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

#include "canvas/cell_grid.h"
#include "engine/paint_document.h"

namespace fugue {

class PaintLayer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    // The canvas's size in pixels; setting it starts an empty canvas of that size (a picture layer and
    // one paint layer, no history).
    Q_PROPERTY(QSize imageSize READ imageSize WRITE setImageSize NOTIFY imageSizeChanged FINAL)
    Q_PROPERTY(qreal originX READ originX WRITE setOriginX NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal originY READ originY WRITE setOriginY NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewChanged FINAL)
    // The mask layers' overlay color, with its alpha; `maskOnly` draws them alone, white on black.
    Q_PROPERTY(QColor tint READ tint WRITE setTint NOTIFY tintChanged FINAL)
    Q_PROPERTY(bool maskOnly READ maskOnly WRITE setMaskOnly NOTIFY tintChanged FINAL)
    Q_PROPERTY(bool checker READ checker WRITE setChecker NOTIFY tintChanged FINAL)
    // The brush: the color a paint layer takes (a mask layer ignores it) and its opacity 0 to 1.
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY brushChanged FINAL)
    Q_PROPERTY(qreal brushOpacity READ brushOpacity WRITE setBrushOpacity NOTIFY brushChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged FINAL)
    // Nothing painted: only the picture, whole and visible, as it is (no file needs saving).
    Q_PROPERTY(bool empty READ isEmpty NOTIFY historyChanged FINAL)
    // The folder `store` writes into: the inputs folder (paths.h) unless a caller says otherwise.
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged FINAL)
    // The layers, top first: [{id, name, kind ("paint", "mask", "picture"), visible, opacity, active}].
    Q_PROPERTY(QVariantList layers READ layers NOTIFY layersChanged FINAL)
    Q_PROPERTY(int activeLayer READ activeLayer NOTIFY layersChanged FINAL)
    // The active layer takes paint (it is not the picture).
    Q_PROPERTY(bool canPaint READ canPaint NOTIFY layersChanged FINAL)
    Q_PROPERTY(bool hasPicture READ hasPicture NOTIFY layersChanged FINAL)

public:
    explicit PaintLayer(QQuickItem *parent = nullptr);
    ~PaintLayer() override;

    QSize imageSize() const { return {m_document->width(), m_document->height()}; }
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
    bool checker() const { return m_checker; }
    void setChecker(bool on);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    qreal brushOpacity() const { return m_opacity; }
    void setBrushOpacity(qreal v);
    bool canUndo() const { return m_document->canUndo(); }
    bool canRedo() const { return m_document->canRedo(); }
    bool isEmpty() const;
    QString folder() const;
    void setFolder(const QString &folder);
    QVariantList layers() const;
    int activeLayer() const { return m_document->activeLayer(); }
    bool canPaint() const { return m_document->canPaint(); }
    bool hasPicture() const { return m_hasPicture; }

    // Reads the layers of an .ora file (a path or a file URL) as the start of the session, scaled to the
    // canvas's size when it differs (then `warning()` says so). False when it cannot be read. Not an
    // undo step: the history starts here. The picture given by setPicture stays in its layers.
    Q_INVOKABLE bool loadPaint(const QString &path);
    // The input picture (a path or a file URL, any picture Fugue reads), scaled to the canvas, drawn in
    // every picture layer. Empty: no picture. False when it cannot be read.
    Q_INVOKABLE bool setPicture(const QString &path);
    // The canvas size an .ora file holds, empty (0 x 0) when it cannot be read.
    Q_INVOKABLE QSize fileSize(const QString &path) const;
    // Starts again: a picture layer and one empty paint layer, no history.
    Q_INVOKABLE void reset();

    // Pointer input in picture pixels, as MaskLayer's. A stroke is one undo step. `size` is the
    // brush's diameter in picture pixels, `hardness` 0 to 1; the opacity and the color are the properties.
    Q_INVOKABLE void beginStroke(bool erase, double size, double hardness, double x, double y, double pressure = 1);
    Q_INVOKABLE void strokeTo(double x, double y, double pressure = 1);
    Q_INVOKABLE void endStroke();
    Q_INVOKABLE void fillPolygon(const QVariantList &points, bool erase);
    Q_INVOKABLE void fillRect(double x, double y, double width, double height, bool erase);
    // On the active layer: invert (a paint layer keeps its alpha), clear, fill with the brush.
    Q_INVOKABLE void invert();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void fill();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // Layers. Adding puts the new one on top and makes it active (an undo step, as removing and moving
    // are); `kind` is "paint" or "mask". The picture layer cannot be removed. `delta` +1 moves a layer up
    // (toward the top), -1 down. Visibility, opacity, name and the active layer are not undo steps.
    Q_INVOKABLE int addLayer(const QString &kind);
    Q_INVOKABLE bool removeLayer(int id);
    Q_INVOKABLE bool moveLayer(int id, int delta);
    Q_INVOKABLE bool selectLayer(int id);
    Q_INVOKABLE bool setLayerVisible(int id, bool visible);
    Q_INVOKABLE bool setLayerOpacity(int id, double opacity);
    Q_INVOKABLE bool setLayerName(int id, const QString &name);

    // What is at a pixel: the active layer's value (a mask's 0 to 255, a paint layer's alpha), -1 outside
    // the canvas or on a picture layer; and the picture as the result has it (straight RGBA).
    Q_INVOKABLE int valueAt(int x, int y) const;
    Q_INVOKABLE QColor colorAt(int x, int y) const;
    // The flattened picture as a PNG; the layers as an .ora file, and stored in the folder as
    // `<stem>-<10 hex>.ora` (the same layers are one file): the path, or empty with `lastError` set.
    Q_INVOKABLE QByteArray png() const;
    Q_INVOKABLE QString store(const QString &stem);
    // The text of the last failure, for a notice, and of the last thing worth telling (a canvas scaled).
    Q_INVOKABLE QString lastError() const { return m_error; }
    Q_INVOKABLE QString warning() const { return m_warning; }
    Q_INVOKABLE int uploadedCells() const { return m_cells.uploaded(); }

signals:
    void imageSizeChanged();
    void viewChanged();
    void tintChanged();
    void brushChanged();
    void historyChanged();
    void folderChanged();
    void layersChanged();
    // The picture changed (a dab, a step, a layer's look): the editor's readout and tests follow it.
    void edited();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &now, const QRectF &before) override;

private:
    void newDocument(int width, int height);
    void applyBrush(bool erase, double size, double hardness);
    // What changed: the pixels (re-upload the dirty cells), or the stack (everything).
    void changed(bool history);
    void stackChanged();
    void applyPicture();
    std::vector<int> order() const;
    void markAllDirty();

    std::unique_ptr<mask::PaintDocument> m_document;
    mask::MaskPaint m_picture{1, 1, 4};
    bool m_hasPicture = false;
    qreal m_originX = 0;
    qreal m_originY = 0;
    qreal m_zoom = 1;
    QColor m_tint = QColor(255, 59, 48, 128);
    bool m_maskOnly = false;
    bool m_checker = false;
    QColor m_color = QColor(0, 0, 0);
    qreal m_opacity = 1;
    QString m_folder;
    QString m_error;
    QString m_warning;
    CellGrid m_cells;
};

} // namespace fugue
