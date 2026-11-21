#pragma once

// The pure core of the mask editor (#172, spec 14.4): an 8-bit mask at the picture's size, painted
// with round dabs, a lasso or a rectangle, inverted, cleared or filled, with undo and redo kept as the
// 64 px tiles a step changed. It knows nothing of Qt Quick, so a stroke is the same numbers whether
// a window draws it or a test does. 255 is the part to change and 0 the part to keep (14.4).
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

struct Brush {
    double size = 64;      // diameter in picture pixels
    double hardness = 0.8; // 0 to 1
    double opacity = 1;    // 0 to 1
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

    // An all-0 mask of this size (at least 1 x 1).
    MaskPaint(int width, int height);
    // From 8-bit pixels, row after row (a short or long buffer is cut or padded with 0).
    MaskPaint(int width, int height, const std::vector<uint8_t> &pixels);

    int width() const { return m_width; }
    int height() const { return m_height; }
    const std::vector<uint8_t> &pixels() const { return m_pixels; }
    uint8_t at(int x, int y) const { return m_pixels[size_t(y) * size_t(m_width) + size_t(x)]; }
    bool isEmpty() const; // every pixel 0: no mask

    // A stroke: dabs along the points given until endStroke, which makes it one undo step. The first
    // strokeTo puts a dab on the spot. `pressure` (0 to 1, a tablet's) scales the size.
    void beginStroke(Mode mode, const Brush &brush);
    void strokeTo(Point point, double pressure = 1);
    void endStroke();
    bool stroking() const { return m_stroking; }

    // A shape filled with antialiased edges: a polygon (closed by its last edge; non-zero winding, so
    // a self-crossing outline fills every loop) or a rectangle. One undo step each.
    void fillPolygon(const std::vector<Point> &outline, Mode mode = Mode::Paint, double opacity = 1);
    void fillRect(double x, double y, double width, double height, Mode mode = Mode::Paint, double opacity = 1);

    void invert();
    void clear();
    void fill();

    bool canUndo() const { return !m_undo.empty() && !m_stroking; }
    bool canRedo() const { return !m_redo.empty() && !m_stroking; }
    void undo();
    void redo();
    size_t undoSteps() const { return m_undo.size(); }
    size_t redoSteps() const { return m_redo.size(); }
    size_t undoBytes() const { return m_undoBytes; }  // what the undo steps hold
    size_t redoBytes() const { return m_redoBytes; }
    void forgetHistory();

    // The rectangle changed since the last call (empty when none): what a view re-uploads.
    IRect takeDirty();

    // The mask as an 8-bit gray PNG with no metadata: the same pixels are the same bytes.
    Result<QByteArray> toPng() const;
    // A PNG (or any picture libvips reads) as a mask: its first channel, 16-bit scaled to 8.
    static Result<MaskPaint> fromPng(const QByteArray &bytes);
    // The mask at another size, bilinear (pixel centers).
    MaskPaint scaled(int width, int height) const;

private:
    struct Step {
        std::map<int, std::vector<uint8_t>> tiles; // tile index -> its pixels (the other state)
    };
    int tilesAcross() const { return (m_width + tileSize - 1) / tileSize; }
    IRect tileRect(int index) const;
    void beginStep();
    void touch(const IRect &area);
    void commitStep();
    void swapStep(Step &step);
    void trim();
    void dab(Point center, double pressure);
    void apply(int x, int y, double coverage, Mode mode, double opacity);
    void note(const IRect &area);
    static size_t bytesOf(const Step &step);

    int m_width;
    int m_height;
    std::vector<uint8_t> m_pixels;
    std::vector<Step> m_undo;
    std::vector<Step> m_redo;
    size_t m_undoBytes = 0;
    size_t m_redoBytes = 0;
    Step m_pending;
    bool m_stepOpen = false;
    bool m_stroking = false;
    Mode m_mode = Mode::Paint;
    Brush m_brush;
    bool m_haveLast = false;
    Point m_last;
    double m_lastPressure = 1;
    double m_carry = 0; // distance walked since the last dab
    IRect m_dirty;
};

} // namespace fugue::mask
