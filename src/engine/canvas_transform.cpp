#include "engine/canvas_transform.h"

#include <algorithm>
#include <cmath>

#include "engine/paint_file.h"

namespace fugue::canvas {

namespace {

constexpr double kPi = 3.14159265358979323846;

Error invalid(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("input_invalid"), message, hint); }

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

double sinc(double x) { return x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }
double lanczos3(double x) { return std::abs(x) < 3 ? sinc(x) * sinc(x / 3) : 0.0; }
double catmullRom(double x) { // cubic convolution, a = -0.5
    x = std::abs(x);
    if (x < 1)
        return 1.5 * x * x * x - 2.5 * x * x + 1;
    if (x < 2)
        return -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2;
    return 0;
}
double kernelOf(ResampleFilter filter, double x) { return filter == ResampleFilter::Lanczos ? lanczos3(x) : catmullRom(x); }
double supportOf(ResampleFilter filter) { return filter == ResampleFilter::Lanczos ? 3.0 : 2.0; }

struct Tap {
    int index;
    float weight;
};
using Taps = std::vector<std::vector<Tap>>; // for each output index of a range, its source taps

// The taps of output pixels [from, to) of an axis of `dst` pixels made from `src`, flipped or not.
Taps axisTaps(int src, int dst, ResampleFilter filter, bool flip, int from, int to) {
    Taps out;
    out.reserve(size_t(std::max(0, to - from)));
    const double scale = double(src) / dst;
    for (int i = from; i < to; ++i) {
        const int at = flip ? dst - 1 - i : i;
        std::vector<Tap> taps;
        if (filter == ResampleFilter::Nearest) {
            taps.push_back({std::clamp(int(std::floor((at + 0.5) * scale)), 0, src - 1), 1.0f});
        } else {
            const double centre = (at + 0.5) * scale - 0.5;
            const double widen = std::max(scale, 1.0), radius = supportOf(filter) * widen;
            double sum = 0;
            std::vector<double> weights;
            const int first = int(std::ceil(centre - radius)), last = int(std::floor(centre + radius));
            for (int k = first; k <= last; ++k) {
                weights.push_back(kernelOf(filter, (k - centre) / widen));
                sum += weights.back();
            }
            if (sum == 0) {
                taps.push_back({std::clamp(int(std::lround(centre)), 0, src - 1), 1.0f});
            } else {
                for (int k = first; k <= last; ++k)
                    if (const double w = weights[size_t(k - first)] / sum; w != 0)
                        taps.push_back({std::clamp(k, 0, src - 1), float(w)}); // an edge repeats its own pixel
            }
        }
        out.push_back(std::move(taps));
    }
    return out;
}

// The scaled picture (W x H, flipped) over `rect`, premultiplied float RGBA, row after row. The source rows are read one at a time and added
// into the output rows that use them, so the memory is the output rectangle and one line, however much the picture is shrunk.
std::vector<float> scaledWindow(const mask::MaskPaint &src, int W, int H, const LayerTransform &t, const Rect &rect, size_t *peak) {
    std::vector<float> out(size_t(rect.w) * size_t(rect.h) * 4, 0.0f);
    if (rect.empty())
        return out;
    const Taps rows = axisTaps(src.height(), H, t.filter, t.flipY, rect.y, rect.y + rect.h);
    const Taps cols = axisTaps(src.width(), W, t.filter, t.flipX, rect.x, rect.x + rect.w);
    int rmin = src.height(), rmax = -1;
    for (const auto &taps : rows)
        for (const Tap &tap : taps) {
            rmin = std::min(rmin, tap.index);
            rmax = std::max(rmax, tap.index);
        }
    struct Use {
        int row;
        float weight;
    };
    std::vector<std::vector<Use>> users(size_t(rmax - rmin + 1)); // for each source row, the output rows that take it
    for (int j = 0; j < rect.h; ++j)
        for (const Tap &tap : rows[size_t(j)])
            users[size_t(tap.index - rmin)].push_back({j, tap.weight});
    const std::vector<uint8_t> &px = src.pixels();
    std::vector<float> line(size_t(rect.w) * 4);
    if (peak)
        *peak = std::max(*peak, (out.size() + line.size()) * sizeof(float));
    for (int r = rmin; r <= rmax; ++r) {
        if (users[size_t(r - rmin)].empty())
            continue;
        std::fill(line.begin(), line.end(), 0.0f);
        for (int c = 0; c < rect.w; ++c)
            for (const Tap &tap : cols[size_t(c)]) {
                const uint8_t *p = &px[(size_t(r) * size_t(src.width()) + size_t(tap.index)) * 4];
                const float a = p[3] / 255.0f;
                line[size_t(c) * 4 + 0] += tap.weight * p[0] / 255.0f * a;
                line[size_t(c) * 4 + 1] += tap.weight * p[1] / 255.0f * a;
                line[size_t(c) * 4 + 2] += tap.weight * p[2] / 255.0f * a;
                line[size_t(c) * 4 + 3] += tap.weight * a;
            }
        for (const Use &use : users[size_t(r - rmin)]) {
            float *dest = &out[size_t(use.row) * size_t(rect.w) * 4];
            for (int k = 0; k < rect.w * 4; ++k)
                dest[k] += use.weight * line[size_t(k)];
        }
    }
    return out;
}

uint8_t byteOf(float v) { return uint8_t(std::clamp(int(std::lround(v * 255.0f)), 0, 255)); }

// Premultiplied float RGBA to straight 8-bit, one pixel.
void store(const float *p, uint8_t *to) {
    const float a = std::clamp(p[3], 0.0f, 1.0f);
    if (a <= 0.0f) {
        to[0] = to[1] = to[2] = to[3] = 0;
        return;
    }
    for (int k = 0; k < 3; ++k)
        to[k] = byteOf(std::clamp(p[k] / a, 0.0f, 1.0f));
    to[3] = byteOf(a);
}

int quarterTurns(double angle) { // 0..3 when the angle is a multiple of 90, else -1
    const double q = angle / 90.0;
    if (std::abs(q - std::round(q)) > 1e-9)
        return -1;
    return ((int(std::lround(q)) % 4) + 4) % 4;
}

} // namespace

QString placeableProblem(int width, int height) {
    if (width < 1 || height < 1)
        return QStringLiteral("The picture is empty.");
    if (width > mask::paintFileMaxSide || height > mask::paintFileMaxSide || qint64(width) * height > mask::paintFileMaxPixels)
        return QStringLiteral("The picture is %1 x %2 px: at most %3 px a side and %4 megapixels can be placed on a canvas.")
            .arg(width).arg(height).arg(mask::paintFileMaxSide).arg(mask::paintFileMaxPixels / 1000000);
    return {};
}

LayerTransform centeredPlacement(int sourceWidth, int sourceHeight, double cx, double cy) {
    LayerTransform t;
    t.width = sourceWidth;
    t.height = sourceHeight;
    // Whole pixels: the corner (center - side / 2) is whole.
    t.cx = std::floor(cx - sourceWidth / 2.0 + 0.5) + sourceWidth / 2.0;
    t.cy = std::floor(cy - sourceHeight / 2.0 + 0.5) + sourceHeight / 2.0;
    return t.normalized();
}

LayerTransform snappedToPixels(const LayerTransform &transform) {
    LayerTransform t = transform.normalized();
    const int q = quarterTurns(t.angle);
    if (q < 0)
        return t;
    const double w = std::max(1.0, std::round(t.width)), h = std::max(1.0, std::round(t.height));
    const double visibleW = q % 2 ? h : w, visibleH = q % 2 ? w : h;
    t.cx = std::floor(t.cx - visibleW / 2.0 + 0.5) + visibleW / 2.0;
    t.cy = std::floor(t.cy - visibleH / 2.0 + 0.5) + visibleH / 2.0;
    return t.normalized();
}

LayerTransform flippedOnScreen(const LayerTransform &transform, bool horizontal) {
    LayerTransform t = transform;
    (horizontal ? t.flipX : t.flipY) = !(horizontal ? t.flipX : t.flipY);
    t.angle = -t.angle;
    return t.normalized();
}

LayerTransform turnedQuarter(const LayerTransform &transform, bool clockwise) {
    LayerTransform t = transform;
    t.angle += clockwise ? 90 : -90;
    return snappedToPixels(t);
}

QString transformProblem(const LayerTransform &t) {
    for (const double v : {t.cx, t.cy, t.width, t.height, t.angle})
        if (!std::isfinite(v))
            return QStringLiteral("A number of the placement is not a number.");
    const double w = std::round(t.width), h = std::round(t.height);
    if (w < 1 || h < 1)
        return QStringLiteral("A layer is at least 1 x 1 px.");
    if (w > maxTransformSide || h > maxTransformSide || w * h > maxTransformArea)
        return QStringLiteral("A layer is at most %1 px a side and %2 megapixels.").arg(maxTransformSide).arg(int(maxTransformArea / 1000000));
    if (std::abs(t.cx) > 1e7 || std::abs(t.cy) > 1e7)
        return QStringLiteral("The layer is too far from the canvas.");
    return {};
}

Result<mask::MaskPaint> renderPlaced(const mask::MaskPaint &source, const LayerTransform &requested, int canvasWidth, int canvasHeight, const RenderOptions &options) {
    if (source.channels() != 4)
        return invalid(QStringLiteral("Only a color layer is placed."));
    if (const QString why = transformProblem(requested); !why.isEmpty())
        return invalid(why);
    const LayerTransform t = requested.normalized();
    const int W = int(std::lround(t.width)), H = int(std::lround(t.height));
    const int tile = std::max(16, options.tile);
    std::vector<uint8_t> out(size_t(canvasWidth) * size_t(canvasHeight) * 4, 0);
    const auto finish = [&] { return mask::MaskPaint(canvasWidth, canvasHeight, out, 4); };
    const int quarters = quarterTurns(t.angle);

    if (quarters == 0 && t.isPlacement(source.width(), source.height())) { // pixels laid down as they are
        const int left = int(std::lround(t.cx - W / 2.0)), top = int(std::lround(t.cy - H / 2.0));
        const int x0 = std::max(0, left), x1 = std::min(canvasWidth, left + W), y0 = std::max(0, top), y1 = std::min(canvasHeight, top + H);
        for (int y = y0; y < y1 && x0 < x1; ++y)
            std::copy_n(source.pixels().begin() + (size_t(y - top) * size_t(W) + size_t(x0 - left)) * 4, size_t(x1 - x0) * 4,
                        out.begin() + (size_t(y) * size_t(canvasWidth) + size_t(x0)) * 4);
        return finish();
    }

    // The rest is drawn in tiles of the canvas, each from the window of the scaled picture it needs, so the memory is a tile's, not the picture's.
    if (quarters == 0) { // scaled and not turned
        const int left = int(std::lround(t.cx - W / 2.0)), top = int(std::lround(t.cy - H / 2.0));
        const Rect show{std::max(0, left) - left, std::max(0, top) - top, std::min(canvasWidth, left + W) - std::max(0, left), std::min(canvasHeight, top + H) - std::max(0, top)};
        for (int ty = show.y; ty < show.y + show.h; ty += tile)
            for (int tx = show.x; tx < show.x + show.w; tx += tile) {
                const Rect part{tx, ty, std::min(tile, show.x + show.w - tx), std::min(tile, show.y + show.h - ty)};
                const std::vector<float> scaled = scaledWindow(source, W, H, t, part, options.peakBytes);
                for (int j = 0; j < part.h; ++j)
                    for (int i = 0; i < part.w; ++i)
                        store(&scaled[(size_t(j) * size_t(part.w) + size_t(i)) * 4], &out[(size_t(part.y + top + j) * size_t(canvasWidth) + size_t(part.x + left + i)) * 4]);
            }
        return finish();
    }

    // Turned: every canvas pixel looks back through the turn into the scaled picture (W x H, about its center).
    const double angle = t.angle * kPi / 180.0;
    double cosA = std::cos(angle), sinA = std::sin(angle);
    if (quarters > 0) { // a quarter turn is exact
        cosA = quarters == 2 ? -1 : 0;
        sinA = quarters == 1 ? 1 : quarters == 3 ? -1 : 0;
    }
    const auto back = [&](double x, double y, double *px, double *py) { // canvas point -> point in the scaled picture
        const double dx = x - t.cx, dy = y - t.cy;
        *px = dx * cosA + dy * sinA + W / 2.0;
        *py = -dx * sinA + dy * cosA + H / 2.0;
    };
    const int pad = t.filter == ResampleFilter::Nearest ? 1 : int(supportOf(t.filter)) + 1;
    const double reach = t.filter == ResampleFilter::Nearest ? 0 : supportOf(t.filter);
    const bool hardEdge = t.filter == ResampleFilter::Nearest || quarters > 0; // a quarter turn lands on pixels: no edge to soften
    for (int cy0 = 0; cy0 < canvasHeight; cy0 += tile)
        for (int cx0 = 0; cx0 < canvasWidth; cx0 += tile) {
            const int cx1 = std::min(canvasWidth, cx0 + tile), cy1 = std::min(canvasHeight, cy0 + tile);
            double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
            for (const double x : {double(cx0), double(cx1)})
                for (const double y : {double(cy0), double(cy1)}) {
                    double px, py;
                    back(x, y, &px, &py);
                    minX = std::min(minX, px);
                    maxX = std::max(maxX, px);
                    minY = std::min(minY, py);
                    maxY = std::max(maxY, py);
                }
            const int rx0 = std::max(0, int(std::floor(minX)) - pad), ry0 = std::max(0, int(std::floor(minY)) - pad);
            const int rx1 = std::min(W, int(std::ceil(maxX)) + pad), ry1 = std::min(H, int(std::ceil(maxY)) + pad);
            const Rect need{rx0, ry0, rx1 - rx0, ry1 - ry0};
            if (need.empty())
                continue;
            const std::vector<float> scaled = scaledWindow(source, W, H, t, need, options.peakBytes);
            const auto at = [&](int x, int y, int k) { // clamped to the window
                x = std::clamp(x, need.x, need.x + need.w - 1) - need.x;
                y = std::clamp(y, need.y, need.y + need.h - 1) - need.y;
                return scaled[(size_t(y) * size_t(need.w) + size_t(x)) * 4 + size_t(k)];
            };
            for (int y = cy0; y < cy1; ++y)
                for (int x = cx0; x < cx1; ++x) {
                    double px, py;
                    back(x + 0.5, y + 0.5, &px, &py);
                    // How much of this canvas pixel the picture covers: a soft one pixel edge, hard for nearest and quarter turns.
                    const double ex = std::min(px, W - px), ey = std::min(py, H - py);
                    if (ex < (hardEdge ? 0.0 : -0.5) || ey < (hardEdge ? 0.0 : -0.5))
                        continue;
                    const double cover = hardEdge ? 1.0 : std::clamp(ex + 0.5, 0.0, 1.0) * std::clamp(ey + 0.5, 0.0, 1.0);
                    float p[4] = {0, 0, 0, 0};
                    if (t.filter == ResampleFilter::Nearest) {
                        for (int k = 0; k < 4; ++k)
                            p[k] = at(int(std::floor(px)), int(std::floor(py)), k);
                    } else {
                        const double sx = px - 0.5, sy = py - 0.5;
                        const int x0 = int(std::floor(sx)) - int(reach) + 1, x1 = int(std::floor(sx)) + int(reach);
                        const int y0 = int(std::floor(sy)) - int(reach) + 1, y1 = int(std::floor(sy)) + int(reach);
                        double wsum = 0;
                        for (int yy = y0; yy <= y1; ++yy) {
                            const double wy = kernelOf(t.filter, sy - yy);
                            for (int xx = x0; xx <= x1; ++xx) {
                                const double w = wy * kernelOf(t.filter, sx - xx);
                                if (w == 0)
                                    continue;
                                wsum += w;
                                for (int k = 0; k < 4; ++k)
                                    p[k] += float(w) * at(xx, yy, k);
                            }
                        }
                        if (wsum != 0)
                            for (float &v : p)
                                v = float(v / wsum);
                        p[3] = std::clamp(p[3], 0.0f, 1.0f); // a sharp kernel overshoots: color stays within its alpha
                        for (int k = 0; k < 3; ++k)
                            p[k] = std::clamp(p[k], 0.0f, p[3]);
                    }
                    for (float &v : p)
                        v = float(v * cover);
                    store(p, &out[(size_t(y) * size_t(canvasWidth) + size_t(x)) * 4]);
                }
        }
    return finish();
}

Result<mask::MaskPaint> renderPlacedMask(const mask::MaskPaint &source, const LayerTransform &transform, int canvasWidth, int canvasHeight, const RenderOptions &options) {
    if (source.channels() != 1)
        return invalid(QStringLiteral("Only a one-channel mask is placed as a mask."));
    std::vector<uint8_t> color(size_t(source.width()) * size_t(source.height()) * 4);
    for (size_t i = 0; i < source.pixels().size(); ++i) {
        color[i * 4] = color[i * 4 + 1] = color[i * 4 + 2] = source.pixels()[i];
        color[i * 4 + 3] = 255;
    }
    const Result<mask::MaskPaint> drawn = renderPlaced(mask::MaskPaint(source.width(), source.height(), color, 4), transform, canvasWidth, canvasHeight, options);
    if (!drawn)
        return drawn;
    std::vector<uint8_t> out(size_t(canvasWidth) * size_t(canvasHeight));
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = drawn->pixels()[i * 4 + 3] == 0 ? 255 : drawn->pixels()[i * 4]; // no picture, nothing to hide
    return mask::MaskPaint(canvasWidth, canvasHeight, out, 1);
}

bool hasCurrentOriginal(const LayerRecord &layer) {
    return layer.source && layer.source->rendered == renderedKey(layer.tiles, layer.hasMask, layer.maskTiles) && layer.source->hasMask == layer.hasMask;
}

Result<LayerContent> contentOf(const LayerRecord &layer, int canvasWidth, int canvasHeight, const TileReader &read) {
    const auto assemble = [&](int channels, const TileMap &tiles) {
        return assembleLayer(canvasWidth, canvasHeight, channels, tiles, [&](const TileKey &key, const QString &hash) {
            const mask::IRect rect = tileRect(key, canvasWidth, canvasHeight);
            return read(hash, rect.width, rect.height, channels);
        });
    };
    const Result<mask::MaskPaint> pixels = assemble(4, layer.tiles);
    if (!pixels)
        return Result<LayerContent>(pixels.errors());
    int x0 = canvasWidth, y0 = canvasHeight, x1 = -1, y1 = -1;
    const std::vector<uint8_t> &px = pixels->pixels();
    for (int y = 0; y < canvasHeight; ++y)
        for (int x = 0; x < canvasWidth; ++x)
            if (px[(size_t(y) * size_t(canvasWidth) + size_t(x)) * 4 + 3] != 0) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0)
        return invalid(QStringLiteral("%1 has no pixels to move or transform: it is empty.").arg(layer.name));
    const auto crop = [&](const mask::MaskPaint &from, int channels) {
        std::vector<uint8_t> out(size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1) * size_t(channels));
        for (int y = y0; y <= y1; ++y)
            std::copy_n(from.pixels().begin() + (size_t(y) * size_t(canvasWidth) + size_t(x0)) * size_t(channels), size_t(x1 - x0 + 1) * size_t(channels),
                        out.begin() + size_t(y - y0) * size_t(x1 - x0 + 1) * size_t(channels));
        return mask::MaskPaint(x1 - x0 + 1, y1 - y0 + 1, out, channels);
    };
    LayerContent content;
    content.bounds = mask::IRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    content.pixels = crop(pixels.value(), 4);
    if (layer.hasMask) {
        const Result<mask::MaskPaint> maskPixels = assemble(1, layer.maskTiles);
        if (!maskPixels)
            return Result<LayerContent>(maskPixels.errors());
        content.hasMask = true;
        content.mask = crop(maskPixels.value(), 1);
    }
    return content;
}

Result<LayerSource> originalOf(const LayerRecord &layer, int canvasWidth, int canvasHeight, const TileReader &read, const TileSink &consume) {
    const Result<LayerContent> content = contentOf(layer, canvasWidth, canvasHeight, read);
    if (!content)
        return Result<LayerSource>(content.errors());
    LayerSource source;
    source.width = content->bounds.width;
    source.height = content->bounds.height;
    Result<TileMap> tiles = cutLayer(content->pixels, consume);
    if (!tiles)
        return Result<LayerSource>(tiles.errors());
    source.tiles = std::move(tiles.value());
    if (content->hasMask) {
        Result<TileMap> maskTiles = cutLayer(content->mask, consume);
        if (!maskTiles)
            return Result<LayerSource>(maskTiles.errors());
        source.hasMask = true;
        source.maskTiles = std::move(maskTiles.value());
    }
    source.transform = centeredPlacement(source.width, source.height, content->bounds.x + source.width / 2.0, content->bounds.y + source.height / 2.0);
    source.rendered = renderedKey(layer.tiles, layer.hasMask, layer.maskTiles);
    return source;
}

Result<CurrentPlacement> currentPlacement(const LayerRecord &layer, int canvasWidth, int canvasHeight, const TileReader &read) {
    CurrentPlacement out;
    if (hasCurrentOriginal(layer)) {
        out.transform = layer.source->transform;
        out.sourceWidth = layer.source->width;
        out.sourceHeight = layer.source->height;
        out.original = true;
        return out;
    }
    const Result<LayerSource> origin = originalOf(layer, canvasWidth, canvasHeight, read);
    if (!origin)
        return Result<CurrentPlacement>(origin.errors());
    out.transform = origin->transform;
    out.sourceWidth = origin->width;
    out.sourceHeight = origin->height;
    return out;
}

Result<Tree> transformedTree(const Tree &tree, int position, const LayerTransform &requested, const TileReader &read, const TileSink &consume, const SeenPlacement *seen) {
    if (position < 0 || position >= int(tree.layers.size()))
        return invalid(QStringLiteral("There is no layer %1: the revision has %2.").arg(position).arg(tree.layers.size()));
    const LayerRecord &layer = tree.layers[size_t(position)];
    if (layer.group)
        return invalid(QStringLiteral("%1 is a group: transform its layers one by one, or merge the group into one layer first.").arg(layer.name));
    if (layer.kind != mask::LayerKind::Paint)
        return invalid(QStringLiteral("%1 is a %2 layer: only a paint layer can be moved or transformed.").arg(layer.name, layer.kind == mask::LayerKind::Mask ? QStringLiteral("mask") : QStringLiteral("picture")));
    if (const QString why = transformProblem(requested); !why.isEmpty())
        return invalid(why);
    const LayerTransform target = requested.normalized();
    if (seen) {
        const bool current = hasCurrentOriginal(layer);
        if (current != seen->original ||
            (current && (!(layer.source->transform == seen->transform) || layer.source->width != seen->sourceWidth || layer.source->height != seen->sourceHeight)))
            return invalid(QStringLiteral("%1 changed since it was looked at: nothing was changed.").arg(layer.name), QStringLiteral("Look at it again and do it once more."));
    }
    if (hasCurrentOriginal(layer) && layer.source->transform == target)
        return tree;

    // The pixels to draw from: the layer's original while it is what the layer shows, else the layer's own pixels (its content).
    LayerSource source;
    if (hasCurrentOriginal(layer)) {
        source = *layer.source;
    } else {
        Result<LayerSource> origin = originalOf(layer, tree.width, tree.height, read, consume);
        if (!origin)
            return Result<Tree>(origin.errors());
        source = std::move(origin.value());
    }
    const auto assemble = [&](int channels, const TileMap &tiles) {
        return assembleLayer(source.width, source.height, channels, tiles, [&](const TileKey &key, const QString &hash) {
            const mask::IRect rect = tileRect(key, source.width, source.height);
            return read(hash, rect.width, rect.height, channels);
        });
    };
    const Result<mask::MaskPaint> picture = assemble(4, source.tiles);
    if (!picture)
        return Result<Tree>(picture.errors());
    const Result<mask::MaskPaint> shown = renderPlaced(picture.value(), target, tree.width, tree.height);
    if (!shown)
        return Result<Tree>(shown.errors());
    Result<TileMap> tiles = cutLayer(shown.value(), consume);
    if (!tiles)
        return Result<Tree>(tiles.errors());
    LayerRecord next = layer;
    next.tiles = std::move(tiles.value());
    if (source.hasMask) {
        const Result<mask::MaskPaint> maskPicture = assemble(1, source.maskTiles);
        if (!maskPicture)
            return Result<Tree>(maskPicture.errors());
        const Result<mask::MaskPaint> maskShown = renderPlacedMask(maskPicture.value(), target, tree.width, tree.height);
        if (!maskShown)
            return Result<Tree>(maskShown.errors());
        Result<TileMap> maskTiles = cutLayer(maskShown.value(), consume);
        if (!maskTiles)
            return Result<Tree>(maskTiles.errors());
        next.maskTiles = std::move(maskTiles.value());
    }
    source.transform = target;
    source.rendered = renderedKey(next.tiles, next.hasMask, next.maskTiles);
    next.source = std::move(source);
    Tree out = tree;
    out.layers[size_t(position)] = std::move(next);
    return out;
}

} // namespace fugue::canvas
