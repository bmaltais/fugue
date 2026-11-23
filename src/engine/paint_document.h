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
// visibility, opacity, name, blend or layer mask is not a step.
//
// flatten() puts the visible paint and picture layers over each other, bottom up, in straight
// RGBA, each through its blend mode (Blend, below: the separable modes of the W3C Compositing
// spec, then source-over) and its layer mask; flattenMask() is the largest value of the visible
// mask layers (times their opacity). They are two outputs because a mask is a separate port in
// Fugue (14.4), not a color.
//
// A layer mask (#226) is not a layer of the stack: it is one channel (a MaskPaint, the buffer a mask
// layer has, so the mask editor's brush can paint it later) owned by a paint or picture layer, so it
// moves, is duplicated and is removed with its layer and never needs a place of its own. White shows
// the layer, black hides it, grey in between: the layer's alpha is multiplied by it. It can be
// turned off (the layer then shows whole) without being lost. A mask layer (LayerKind::Mask) is the
// other thing: a selection-like mask the paint window paints, flattened to the mask output.

#include <memory>
#include <vector>

#include <QByteArray>
#include <QString>

#include "engine/mask_paint.h"
#include "engine/result.h"

namespace fugue::mask {

enum class LayerKind { Paint, Mask, Picture };

// How a layer's color meets what is under it (spec 14.4, the W3C Compositing and Blending formulas):
// B(Cb, Cs) per channel, then source-over. Each maps to the OpenRaster composite-op of the same name
// (svg:src-over, svg:multiply, ...; Add is svg:plus, read as Krita's and GIMP's Addition).
enum class Blend { Normal, Multiply, Screen, Overlay, SoftLight, Darken, Lighten, Add };

// The blend an OpenRaster composite-op names; any other op (a mode Fugue does not draw, such as
// svg:color-dodge) composites as Normal, and `known` says so.
Blend blendOfOp(const QString &compositeOp, bool *known = nullptr);
QString opOfBlend(Blend blend);         // "svg:src-over", "svg:multiply", ...
QString blendId(Blend blend);           // "normal", "multiply", "screen", "overlay", "soft-light", "darken", "lighten", "add"
bool blendFromId(const QString &id, Blend *blend);
// The value of B(cb, cs) for one channel, 0 to 1 (the formulas of spec 14.4).
double blendChannel(Blend blend, double cb, double cs);

struct LayerInfo {
    int id = 0;
    QString name;
    LayerKind kind = LayerKind::Paint;
    bool visible = true;
    double opacity = 1; // 0 to 1
    // The OpenRaster composite-op as it was read or set; empty is svg:src-over. An op Fugue does not
    // draw is kept as it is (written back the same) and composites as Normal.
    QString compositeOp;
    bool hasMask = false;     // a layer mask (paint and picture layers only)
    bool maskEnabled = true;  // off: the layer shows as if it had none
    bool locked = false;      // protected from pixel edits, delete and merge (#226); a revision's property, an .ora keeps it as fugue:locked
    // Where the layer came from (a Canvas Write's "canvas-write:<node>", an edit's producer) and when
    // (ISO 8601 UTC); empty when nobody said. Carried by moves, renames and duplicates.
    QString producer;
    QString created;
    Blend blend() const { return blendOfOp(compositeOp); }
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
    // The same with every setting of `info` (its id is not used) and, when `mask` is given and the
    // kind can have one, its layer mask (scaled to the canvas, one channel).
    int loadLayer(const LayerInfo &info, const MaskPaint &pixels, const MaskPaint *mask = nullptr);
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
    bool setBlend(int id, Blend blend);
    bool setCompositeOp(int id, const QString &compositeOp); // as read from a file; "svg:src-over" is stored as ""
    bool setOrigin(int id, const QString &producer, const QString &created);
    // The layer mask: given (scaled to the canvas; false for a mask layer), dropped, turned on or off.
    bool setLayerMask(int id, const MaskPaint &mask);
    bool removeLayerMask(int id);
    bool setMaskEnabled(int id, bool enabled);
    // The layer's mask, nullptr when it has none.
    const MaskPaint *maskPixels(int id) const;
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
        std::shared_ptr<MaskPaint> mask; // the layer mask, when info.hasMask
    };
    struct Step {
        enum class Kind { Pixels, Add, Remove, Move } kind = Kind::Pixels;
        int id = 0;       // the layer
        int from = 0;     // Remove: where it was; Move: where it was
        int to = 0;       // Move: where it went
        int active = 0;   // Remove and Add: the active layer before
        std::shared_ptr<MaskPaint> kept; // Remove: the layer, whole
        std::shared_ptr<MaskPaint> keptMask; // Remove: its layer mask
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
