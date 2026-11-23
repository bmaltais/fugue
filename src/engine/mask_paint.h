#pragma once

// The pure core of the mask editor (#172, spec 14.4): an 8-bit mask at the picture's size, painted
// with round dabs, a lasso or a rectangle, inverted, cleared or filled, with undo and redo kept as the
// 64 px tiles a step changed. It knows nothing of Qt Quick, so a stroke is the same numbers whether
// a window draws it or a test does. 255 is the part to change and 0 the part to keep (14.4).
//
// The same buffer, brush, tiles and undo also hold straight RGBA color with `channels` = 4 (the paint
// canvas's layers, #181, engine/paint_document.h): a brush paints its `color` over the pixel, an erase
// takes alpha. A one-channel buffer behaves exactly as above.
//
// Sizes are in picture pixels; a point is where in the picture (pixel centers at +0.5), so zooming
// the view never changes what a stroke covers.
//
// A brush dab has radius r = size / 2 (scaled by pressure) and coverage 1 within hardness x r, then a
// smoothstep down to 0 at r; a hard dab (a falloff under one pixel) gets one pixel of antialiasing.
// One dab lands every max(1, 0.1 x size) pixels along a stroke. Paint sets m = max(m, c x opacity)
// and erase m = min(m, 1 - c x opacity), so a stroke never builds up over itself.
//
// Every change is one undo step: a stroke (beginStroke to endStroke) or one shape or whole-mask
// operation. A step keeps the tiles it changed as they were; undo swaps them with the mask, so redo
// is the same swap and memory is the area touched, not the picture times the steps. At least
// `minimumSteps` steps are kept; beyond that the oldest go when there are more than `maximumSteps`
// or the tiles hold more than `byteLimit`.

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include <QByteArray>

#include "engine/result.h"

namespace fugue::mask {

struct IRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
    IRect united(const IRect &o) const;
    bool operator==(const IRect &o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
};

struct Point {
    double x = 0;
    double y = 0;
};

// A straight (not premultiplied) 8-bit color, what a four-channel buffer holds and a brush paints.
struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
    bool operator==(const Rgba &o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

struct Brush {
    double size = 64;      // diameter in picture pixels
    double hardness = 0.8; // 0 to 1
    double opacity = 1;    // 0 to 1
    Rgba color;            // four-channel buffers only: the paint (its alpha multiplies `opacity`)
};

enum class Mode { Paint, Erase };

// The coverage 0 to 1 of a dab of `radius` at `distance` from its center (see above).
double dabCoverage(double distance, double radius, double hardness);

class MaskPaint {
public:
    static constexpr int tileSize = 64;
    static constexpr size_t minimumSteps = 100;
    static constexpr size_t maximumSteps = 300;
    static constexpr size_t byteLimit = size_t(256) << 20;

    // An all-0 buffer of this size (at least 1 x 1). `channels` is 1 (a mask: the default) or 4 (straight
    // RGBA color, #181: a paint canvas layer); anything else is 1.
    MaskPaint(int width, int height, int channels = 1);
    // From 8-bit pixels, row after row, `channels` bytes each (a short or long buffer is cut or padded with 0).
    MaskPaint(int width, int height, const std::vector<uint8_t> &pixels, int channels = 1);

    int width() const { return m_width; }
    int height() const { return m_height; }
    const std::vector<uint8_t> &pixels() const { return m_pixels; }
    int channels() const { return m_channels; }
    // The first channel at a pixel: the mask's value, a color buffer's red.
    uint8_t at(int x, int y) const { return m_pixels[offset(x, y)]; }
    Rgba rgbaAt(int x, int y) const; // a four-channel buffer's pixel (a mask's value as gray, opaque)
    bool isEmpty() const; // every byte 0: no mask, or a color buffer with no paint (alpha 0 everywhere)

    // A stroke: dabs along the points given until endStroke, which makes it one undo step. The first
    // strokeTo puts a dab on the spot. `pressure` (0 to 1, a tablet's) scales the size.
    void beginStroke(Mode mode, const Brush &brush);
    void strokeTo(Point point, double pressure = 1);
    void endStroke();
    bool stroking() const { return m_stroking; }

    // A shape filled with antialiased edges: a polygon (closed by its last edge; non-zero winding, so
    // a self-crossing outline fills every loop) or a rectangle. One undo step each.
    void fillPolygon(const std::vector<Point> &outline, Mode mode = Mode::Paint, double opacity = 1, Rgba color = Rgba());
    void fillRect(double x, double y, double width, double height, Mode mode = Mode::Paint, double opacity = 1,
                  Rgba color = Rgba());

    // Whole-buffer operations, one step each. A mask: invert is 255 - v, clear 0, fill 255. A color buffer:
    // invert flips the colors and keeps the alpha, clear makes it transparent, fill covers it with `color`.
    void invert();
    void clear();
    void fill();
    void fill(Rgba color);
    // Replaces the pixels of a mask with `pixels` (width x height bytes; any other size is refused) as one step: only the
    // tiles that differ are kept, so a modify of a small selection costs its own size (mask_ops.h makes such a result).
    bool assign(const std::vector<uint8_t> &pixels);

    bool canUndo() const { return !m_undo.empty() && !m_stroking; }
    bool canRedo() const { return !m_redo.empty() && !m_stroking; }
    void undo();
    void redo();
    size_t undoSteps() const { return m_undo.size(); }
    size_t redoSteps() const { return m_redo.size(); }
    size_t undoBytes() const { return m_undoBytes; }  // what the undo steps hold
    size_t redoBytes() const { return m_redoBytes; }
    void forgetHistory();

    // Counts the steps ever committed (an undo or redo does not count): a layer list that orders steps of
    // several buffers (paint_document.h) sees by it that a new one began.
    uint64_t revision() const { return m_revision; }
    // Drops the redo steps (a step made in another buffer ends them).
    void forgetRedo();

    // The rectangle changed since the last call (empty when none): what a view re-uploads.
    IRect takeDirty();

    // The buffer as an 8-bit PNG with no metadata (gray for a mask, RGBA for color): the same pixels are
    // the same bytes.
    // `compression` is zlib's level 0 to 9 (6 by default; the canvas store's tiles use 1: about 3x faster for a
    // few percent more bytes). It changes the bytes, never the pixels.
    Result<QByteArray> toPng(int compression = 6) const;
    // A color buffer as a picture file by its suffix: "png" (as toPng), "jpg" (quality 92, transparency flattened
    // onto white: JPEG has no alpha) or "webp" (lossless, alpha kept). WebP is at most 16383 px a side.
    Result<QByteArray> toPicture(const QString &suffix) const;
    // The same with a say in the encoding (canvas export, engine/canvas_export.h).
    struct PictureEncoding {
        int quality = 92;      // 1 to 100: a JPEG's, and a lossy WebP's
        bool lossless = true;  // WebP only: lossless keeps every pixel and the alpha, lossy follows `quality`
    };
    Result<QByteArray> toPicture(const QString &suffix, const PictureEncoding &encoding) const;
    // A color buffer at another size, resampled with Lanczos on premultiplied alpha, so shrinking is clean (the
    // quick `scaled` below is bilinear, fine for a layer fitted to a canvas, and aliases when it shrinks a lot).
    Result<MaskPaint> resampled(int width, int height) const;
    // A color buffer laid over an opaque color (straight RGBA, alpha 255 everywhere).
    MaskPaint onBackground(uint8_t red, uint8_t green, uint8_t blue) const;
    // A PNG (or any picture libvips reads) as a mask: its first channel, 16-bit scaled to 8.
    static Result<MaskPaint> fromPng(const QByteArray &bytes);
    // Any picture libvips reads as a four-channel buffer (gray and RGB get an opaque alpha, 16-bit is
    // scaled to 8).
    static Result<MaskPaint> fromPicture(const QByteArray &bytes);
    // The buffer at another size, bilinear (pixel centers; a color buffer's channels are not premultiplied).
    MaskPaint scaled(int width, int height) const;

private:
    struct Step {
        std::map<int, std::vector<uint8_t>> tiles; // tile index -> its pixels (the other state)
    };
    size_t offset(int x, int y) const { return (size_t(y) * size_t(m_width) + size_t(x)) * size_t(m_channels); }
    int tilesAcross() const { return (m_width + tileSize - 1) / tileSize; }
    IRect tileRect(int index) const;
    void beginStep();
    void touch(const IRect &area);
    void commitStep();
    void swapStep(Step &step);
    void trim();
    void dab(Point center, double pressure);
    void apply(int x, int y, double coverage, Mode mode, double opacity, const Rgba &color);
    void applyColor(int x, int y, uint8_t cover, Mode mode, const Rgba &color);
    void note(const IRect &area);
    static size_t bytesOf(const Step &step);

    int m_width;
    int m_height;
    int m_channels;
    std::vector<uint8_t> m_pixels;
    std::vector<uint8_t> m_strokeCover; // color buffers: the most coverage each pixel got in this stroke
    std::vector<Step> m_undo;
    std::vector<Step> m_redo;
    size_t m_undoBytes = 0;
    size_t m_redoBytes = 0;
    Step m_pending;
    bool m_stepOpen = false;
    bool m_stroking = false;
    Mode m_mode = Mode::Paint;
    Brush m_brush;
    uint64_t m_revision = 0;
    bool m_haveLast = false;
    Point m_last;
    double m_lastPressure = 1;
    double m_carry = 0; // distance walked since the last dab
    IRect m_dirty;
};

} // namespace fugue::mask
