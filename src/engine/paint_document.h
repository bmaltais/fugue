#pragma once

// The paint canvas's core (#181, spec 14.4): a list of layers at one size, painted with the mask
// editor's own brush and tiles (engine/mask_paint.h, which holds one layer's pixels, one channel for
// a mask or four for color). It knows nothing of Qt Quick, so a canvas is the same numbers whether a
// window draws it or a test does.
//
// Layers, bottom first. A paint layer is straight RGBA you paint colors on. A picture layer is a
// picture (scaled to the canvas) that is not painted on: it is what you paint over. A mask layer is
// one channel, painted white on black as in the mask editor. Each has a name, a visibility and an
// opacity.
//
// One brush (size, hardness, opacity, color) and one mode are shared by every layer, as in the mask
// editor; the color is the shared color picker's (the caller sets it). Painting on a mask layer
// ignores the color. Painting on a picture layer does nothing and says so.
//
// One history across all layers, so undo goes back through what was done in the order it was done:
// a stroke or shape on any layer, adding a layer, removing it, moving it. The pixels of a step are
// each layer's own tiles (MaskPaint's); a removed layer is kept whole by its step. Changing a layer's
// visibility, opacity or name is not a step.
//
// flatten() puts the visible paint and picture layers over each other, bottom up, in straight
// RGBA; flattenMask() is the largest value of the visible mask layers (times their opacity). They are
// two outputs because a mask is a separate port in Fugue (14.4), not a color.

#include <memory>
#include <vector>

#include <QByteArray>
#include <QString>

#include "engine/mask_paint.h"
#include "engine/result.h"

namespace fugue::mask {

enum class LayerKind { Paint, Mask, Picture };

struct LayerInfo {
    int id = 0;
    QString name;
    LayerKind kind = LayerKind::Paint;
    bool visible = true;
    double opacity = 1; // 0 to 1
};

class PaintDocument {
public:
    PaintDocument(int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }

    // ---- layers (each a history step; ids are never reused) -----------------------------------
    // The new layer goes on top and becomes the active one. A picture layer takes `picture` (any
    // size, scaled to the canvas; a mask layer's `picture` is not used).
    int addLayer(LayerKind kind, const QString &name = QString(), const MaskPaint &picture = MaskPaint(1, 1, 4));
    bool removeLayer(int id);
    // `position` 0 is the bottom; one past the last is clamped.
    bool moveLayer(int id, int position);

    // Loading (a file's layers, paint_file.h): puts a layer on top with these pixels and settings, as
    // is, and not a history step. `pixels` is scaled to the canvas and given the channels of the kind
    // (one for a mask, four for the others). Returns the new id.
    int loadLayer(LayerKind kind, const QString &name, bool visible, double opacity, const MaskPaint &pixels);
    // Puts a picture into a picture layer (scaled to the canvas); false for any other layer. Not a
    // history step: the picture is the caller's input, not something painted.
    bool setPicture(int id, const MaskPaint &picture);
    // The same layers on a canvas of another size, each scaled (bilinear); no history.
    PaintDocument scaledTo(int width, int height) const;
    // Drops every undo and redo step (a document just loaded starts with none).
    void forgetHistory();

    int layerCount() const { return int(m_layers.size()); }
    LayerInfo layer(int position) const; // by position, bottom first; an empty LayerInfo (id 0) past the ends
    int positionOf(int id) const;        // -1 when there is no such layer
    // The layer's pixels, for a view to draw (nullptr for no such layer).
    const MaskPaint *pixels(int id) const;

    // The rectangle of the canvas whose pixels changed (a stroke, a step, an undo) since the last call, over
    // every layer; empty when none. What a view draws again.
    IRect takeDirty();

    int activeLayer() const { return m_active; }
    bool setActiveLayer(int id);
    // Not history steps.
    bool setVisible(int id, bool visible);
    bool setOpacity(int id, double opacity);
    bool setName(int id, const QString &name);

    // ---- painting on the active layer ---------------------------------------------------------
    Brush brush; // shared by every layer
    Mode mode = Mode::Paint;

    // False when nothing can be painted: no active layer, or a picture layer.
    bool canPaint() const;
    bool beginStroke();
    void strokeTo(Point point, double pressure = 1);
    void endStroke();
    bool stroking() const { return m_stroking; }
    bool fillPolygon(const std::vector<Point> &outline);
    bool fillRect(double x, double y, double width, double height);
    // The active layer whole: cleared, filled with the brush's color (white for a mask), inverted.
    bool clearLayer();
    bool fillLayer();
    bool invertLayer();

    // ---- history ------------------------------------------------------------------------------
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    size_t undoSteps() const { return m_undo.size(); }

    // ---- the result ---------------------------------------------------------------------------
    // Visible paint and picture layers over a transparent canvas. Straight RGBA, 4 channels.
    MaskPaint flatten() const;
    // Visible mask layers, 1 channel (all 0 when there are none).
    MaskPaint flattenMask() const;
    // The same for one rectangle of the canvas (cut to it): a view draws only what it shows.
    MaskPaint flattenArea(const IRect &area) const;
    MaskPaint flattenMaskArea(const IRect &area) const;
    Result<QByteArray> flattenToPng() const { return flatten().toPng(); }
    // The same flattened picture as a "png", "jpg" (on white) or "webp" file (MaskPaint::toPicture).
    Result<QByteArray> flattenToPicture(const QString &suffix) const { return flatten().toPicture(suffix); }

private:
    struct Layer {
        LayerInfo info;
        std::shared_ptr<MaskPaint> pixels;
    };
    struct Step {
        enum class Kind { Pixels, Add, Remove, Move } kind = Kind::Pixels;
        int id = 0;       // the layer
        int from = 0;     // Remove: where it was; Move: where it was
        int to = 0;       // Move: where it went
        int active = 0;   // Remove and Add: the active layer before
        std::shared_ptr<MaskPaint> kept; // Remove: the layer, whole
        LayerInfo info;                  // Remove: its settings
    };

    Layer *find(int id);
    const Layer *find(int id) const;
    Layer *paintable();
    void push(Step step);
    void recordIfChanged(Layer *layer); // a step for what the layer's buffer just committed
    void detach(Step &step);
    void attach(Step &step);
    bool apply(Step &step, bool undoing);

    int m_width;
    int m_height;
    std::vector<Layer> m_layers;
    int m_nextId = 1;
    int m_active = 0;
    std::vector<Step> m_undo;
    std::vector<Step> m_redo;
    bool m_stroking = false;
    int m_strokeLayer = 0;
    uint64_t m_revisionBefore = 0;
};

} // namespace fugue::mask
