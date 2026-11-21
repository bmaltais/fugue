#include "mask_paint.h"

#include <algorithm>
#include <cmath>

#include "engine/vips.h"

namespace fugue::mask {

namespace {

double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

uint8_t byteOf(double v) { return uint8_t(std::lround(clamp01(v) * 255.0)); }

constexpr int subRows = 8; // vertical samples per pixel row of a filled shape

} // namespace

IRect IRect::united(const IRect &o) const {
    if (empty())
        return o;
    if (o.empty())
        return *this;
    const int x0 = std::min(x, o.x), y0 = std::min(y, o.y);
    const int x1 = std::max(x + width, o.x + o.width), y1 = std::max(y + height, o.y + o.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

double dabCoverage(double distance, double radius, double hardness) {
    if (radius <= 0)
        return 0;
    hardness = clamp01(hardness);
    double inner = hardness * radius;
    double outer = radius;
    if (outer - inner < 1) { // a hard edge keeps one pixel of antialiasing, centered on the radius
        inner = radius - 0.5;
        outer = radius + 0.5;
    }
    if (distance <= inner)
        return 1;
    if (distance >= outer)
        return 0;
    const double t = (outer - distance) / (outer - inner);
    return t * t * (3 - 2 * t);
}

MaskPaint::MaskPaint(int width, int height)
    : m_width(std::max(1, width)), m_height(std::max(1, height)), m_pixels(size_t(m_width) * size_t(m_height), 0) {}

MaskPaint::MaskPaint(int width, int height, const std::vector<uint8_t> &pixels) : MaskPaint(width, height) {
    std::copy_n(pixels.begin(), std::min(pixels.size(), m_pixels.size()), m_pixels.begin());
}

bool MaskPaint::isEmpty() const {
    return std::all_of(m_pixels.begin(), m_pixels.end(), [](uint8_t v) { return v == 0; });
}

// ---- steps and tiles --------------------------------------------------------------------------

IRect MaskPaint::tileRect(int index) const {
    const int across = tilesAcross();
    const int x = (index % across) * tileSize, y = (index / across) * tileSize;
    return {x, y, std::min(tileSize, m_width - x), std::min(tileSize, m_height - y)};
}

void MaskPaint::beginStep() {
    m_pending.tiles.clear();
    m_stepOpen = true;
}

// Keeps the tiles `area` touches as they are now, once each per step.
void MaskPaint::touch(const IRect &area) {
    if (!m_stepOpen || area.empty())
        return;
    const int across = tilesAcross();
    const int x0 = std::max(0, area.x) / tileSize, x1 = std::min(m_width - 1, area.x + area.width - 1) / tileSize;
    const int y0 = std::max(0, area.y) / tileSize, y1 = std::min(m_height - 1, area.y + area.height - 1) / tileSize;
    for (int ty = y0; ty <= y1; ++ty)
        for (int tx = x0; tx <= x1; ++tx) {
            const int index = ty * across + tx;
            if (m_pending.tiles.count(index))
                continue;
            const IRect r = tileRect(index);
            std::vector<uint8_t> copy(size_t(r.width) * size_t(r.height));
            for (int row = 0; row < r.height; ++row)
                std::copy_n(&m_pixels[size_t(r.y + row) * size_t(m_width) + size_t(r.x)], r.width,
                            &copy[size_t(row) * size_t(r.width)]);
            m_pending.tiles.emplace(index, std::move(copy));
        }
}

size_t MaskPaint::bytesOf(const Step &step) {
    size_t bytes = 0;
    for (const auto &tile : step.tiles)
        bytes += tile.second.size();
    return bytes;
}

void MaskPaint::commitStep() {
    if (!m_stepOpen)
        return;
    m_stepOpen = false;
    // A tile that ended up as it began is no change; a step with none is no step.
    for (auto it = m_pending.tiles.begin(); it != m_pending.tiles.end();) {
        const IRect r = tileRect(it->first);
        bool same = true;
        for (int row = 0; row < r.height && same; ++row)
            same = std::equal(it->second.begin() + size_t(row) * size_t(r.width),
                              it->second.begin() + size_t(row + 1) * size_t(r.width),
                              m_pixels.begin() + size_t(r.y + row) * size_t(m_width) + size_t(r.x));
        it = same ? m_pending.tiles.erase(it) : std::next(it);
    }
    if (m_pending.tiles.empty())
        return;
    m_redo.clear();
    m_redoBytes = 0;
    m_undoBytes += bytesOf(m_pending);
    m_undo.push_back(std::move(m_pending));
    m_pending = Step();
    trim();
}

void MaskPaint::trim() {
    while (m_undo.size() > minimumSteps && (m_undo.size() > maximumSteps || m_undoBytes > byteLimit)) {
        m_undoBytes -= bytesOf(m_undo.front());
        m_undo.erase(m_undo.begin());
    }
}

// Swaps each tile of the step with the mask's: what was kept goes in, what was there is kept.
void MaskPaint::swapStep(Step &step) {
    for (auto &entry : step.tiles) {
        const IRect r = tileRect(entry.first);
        for (int row = 0; row < r.height; ++row)
            std::swap_ranges(entry.second.begin() + size_t(row) * size_t(r.width),
                             entry.second.begin() + size_t(row + 1) * size_t(r.width),
                             m_pixels.begin() + size_t(r.y + row) * size_t(m_width) + size_t(r.x));
        note(r);
    }
}

void MaskPaint::undo() {
    if (!canUndo())
        return;
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    const size_t bytes = bytesOf(step);
    m_undoBytes -= bytes;
    swapStep(step);
    m_redoBytes += bytes;
    m_redo.push_back(std::move(step));
}

void MaskPaint::redo() {
    if (!canRedo())
        return;
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    const size_t bytes = bytesOf(step);
    m_redoBytes -= bytes;
    swapStep(step);
    m_undoBytes += bytes;
    m_undo.push_back(std::move(step));
    trim();
}

void MaskPaint::forgetHistory() {
    m_undo.clear();
    m_redo.clear();
    m_undoBytes = m_redoBytes = 0;
}

void MaskPaint::note(const IRect &area) { m_dirty = m_dirty.united(area); }

IRect MaskPaint::takeDirty() {
    const IRect dirty = m_dirty;
    m_dirty = IRect();
    return dirty;
}

// ---- painting ---------------------------------------------------------------------------------

void MaskPaint::apply(int x, int y, double coverage, Mode mode, double opacity) {
    const uint8_t c = byteOf(coverage * clamp01(opacity));
    if (c == 0)
        return;
    uint8_t &m = m_pixels[size_t(y) * size_t(m_width) + size_t(x)];
    m = mode == Mode::Paint ? std::max(m, c) : std::min(m, uint8_t(255 - c));
}

void MaskPaint::dab(Point center, double pressure) {
    const double radius = std::max(0.5, m_brush.size * std::clamp(pressure, 0.05, 1.0) / 2);
    const int x0 = std::max(0, int(std::floor(center.x - radius - 1))), x1 = std::min(m_width - 1, int(std::ceil(center.x + radius + 1)));
    const int y0 = std::max(0, int(std::floor(center.y - radius - 1))), y1 = std::min(m_height - 1, int(std::ceil(center.y + radius + 1)));
    if (x0 > x1 || y0 > y1)
        return;
    const IRect area{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    touch(area);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            apply(x, y, dabCoverage(std::hypot(x + 0.5 - center.x, y + 0.5 - center.y), radius, m_brush.hardness), m_mode,
                  m_brush.opacity);
    note(area);
}

void MaskPaint::beginStroke(Mode mode, const Brush &brush) {
    if (m_stroking)
        endStroke();
    beginStep();
    m_stroking = true;
    m_mode = mode;
    m_brush = brush;
    m_brush.size = std::max(1.0, brush.size);
    m_haveLast = false;
    m_carry = 0;
}

void MaskPaint::strokeTo(Point point, double pressure) {
    if (!m_stroking)
        return;
    if (!m_haveLast) {
        dab(point, pressure);
        m_haveLast = true;
        m_last = point;
        m_lastPressure = pressure;
        m_carry = 0;
        return;
    }
    const double length = std::hypot(point.x - m_last.x, point.y - m_last.y);
    const double spacing = std::max(1.0, 0.1 * m_brush.size * std::clamp((pressure + m_lastPressure) / 2, 0.05, 1.0));
    double at = spacing - m_carry; // distance along this segment to the next dab
    double lastDab = -m_carry;
    while (at <= length) {
        const double t = length > 0 ? at / length : 0;
        dab({m_last.x + (point.x - m_last.x) * t, m_last.y + (point.y - m_last.y) * t},
            m_lastPressure + (pressure - m_lastPressure) * t);
        lastDab = at;
        at += spacing;
    }
    m_carry = length - lastDab;
    m_last = point;
    m_lastPressure = pressure;
}

void MaskPaint::endStroke() {
    if (!m_stroking)
        return;
    m_stroking = false;
    commitStep();
}

void MaskPaint::fillPolygon(const std::vector<Point> &outline, Mode mode, double opacity) {
    if (outline.size() < 3 || m_stroking)
        return;
    double minX = outline[0].x, maxX = outline[0].x, minY = outline[0].y, maxY = outline[0].y;
    for (const Point &p : outline) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const int x0 = std::max(0, int(std::floor(minX))), x1 = std::min(m_width - 1, int(std::ceil(maxX)));
    const int y0 = std::max(0, int(std::floor(minY))), y1 = std::min(m_height - 1, int(std::ceil(maxY)));
    if (x0 > x1 || y0 > y1)
        return;
    const IRect area{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    beginStep();
    touch(area);
    std::vector<float> cover(size_t(area.width));
    struct Crossing {
        double x;
        int direction;
    };
    std::vector<Crossing> crossings;
    for (int y = y0; y <= y1; ++y) {
        std::fill(cover.begin(), cover.end(), 0.0f);
        for (int k = 0; k < subRows; ++k) {
            const double sy = y + (k + 0.5) / subRows;
            crossings.clear();
            for (size_t i = 0; i < outline.size(); ++i) {
                const Point &a = outline[i], &b = outline[(i + 1) % outline.size()];
                if (a.y == b.y)
                    continue;
                const bool down = b.y > a.y;
                const double top = down ? a.y : b.y, bottom = down ? b.y : a.y;
                if (sy < top || sy >= bottom)
                    continue;
                crossings.push_back({a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y), down ? 1 : -1});
            }
            std::sort(crossings.begin(), crossings.end(), [](const Crossing &l, const Crossing &r) { return l.x < r.x; });
            int winding = 0;
            for (size_t i = 0; i + 1 < crossings.size(); ++i) {
                winding += crossings[i].direction;
                if (winding == 0)
                    continue;
                const double from = std::max(crossings[i].x, double(x0)), to = std::min(crossings[i + 1].x, double(x1 + 1));
                for (int x = int(std::floor(from)); x < to && x <= x1; ++x)
                    cover[size_t(x - x0)] += float((std::min(to, x + 1.0) - std::max(from, double(x))) / subRows);
            }
        }
        for (int x = x0; x <= x1; ++x)
            apply(x, y, cover[size_t(x - x0)], mode, opacity);
    }
    note(area);
    commitStep();
}

void MaskPaint::fillRect(double x, double y, double width, double height, Mode mode, double opacity) {
    if (m_stroking)
        return;
    if (width < 0) {
        x += width;
        width = -width;
    }
    if (height < 0) {
        y += height;
        height = -height;
    }
    const int x0 = std::max(0, int(std::floor(x))), x1 = std::min(m_width - 1, int(std::ceil(x + width)) - 1);
    const int y0 = std::max(0, int(std::floor(y))), y1 = std::min(m_height - 1, int(std::ceil(y + height)) - 1);
    if (x0 > x1 || y0 > y1)
        return;
    const IRect area{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    beginStep();
    touch(area);
    const auto overlap = [](double from, double size, int pixel) {
        return std::max(0.0, std::min(from + size, pixel + 1.0) - std::max(from, double(pixel)));
    };
    for (int py = y0; py <= y1; ++py) {
        const double oy = overlap(y, height, py);
        for (int px = x0; px <= x1; ++px)
            apply(px, py, overlap(x, width, px) * oy, mode, opacity);
    }
    note(area);
    commitStep();
}

void MaskPaint::invert() {
    if (m_stroking)
        return;
    beginStep();
    touch({0, 0, m_width, m_height});
    for (uint8_t &v : m_pixels)
        v = uint8_t(255 - v);
    note({0, 0, m_width, m_height});
    commitStep();
}

void MaskPaint::clear() {
    if (m_stroking)
        return;
    beginStep();
    touch({0, 0, m_width, m_height});
    std::fill(m_pixels.begin(), m_pixels.end(), uint8_t(0));
    note({0, 0, m_width, m_height});
    commitStep();
}

void MaskPaint::fill() {
    if (m_stroking)
        return;
    beginStep();
    touch({0, 0, m_width, m_height});
    std::fill(m_pixels.begin(), m_pixels.end(), uint8_t(255));
    note({0, 0, m_width, m_height});
    commitStep();
}

// ---- files and scaling ------------------------------------------------------------------------

Result<QByteArray> MaskPaint::toPng() const {
    if (!initVips())
        return vipsUnavailable();
    try {
        vips::VImage image = vips::VImage::new_from_memory(m_pixels.data(), m_pixels.size(), m_width, m_height, 1, VIPS_FORMAT_UCHAR);
        void *buffer = nullptr;
        size_t size = 0;
        // No metadata and a fixed compression: the same mask is the same bytes.
        image.write_to_buffer(".png", &buffer, &size,
                              vips::VImage::option()->set("compression", 6)->set("keep", VIPS_FOREIGN_KEEP_NONE));
        QByteArray bytes(static_cast<const char *>(buffer), qsizetype(size));
        g_free(buffer);
        return bytes;
    } catch (const vips::VError &error) {
        return vipsError(error);
    }
}

Result<MaskPaint> MaskPaint::fromPng(const QByteArray &bytes) {
    if (!initVips())
        return vipsUnavailable();
    try {
        vips::VImage image = vips::VImage::new_from_buffer(bytes.constData(), size_t(bytes.size()), "");
        if (image.width() < 1 || image.height() < 1 || double(image.width()) * image.height() > 268435456.0)
            return makeError(QStringLiteral("media_unreadable"), QStringLiteral("This mask picture is empty or too large."));
        if (image.bands() > 1)
            image = image.extract_band(0);
        if (image.format() == VIPS_FORMAT_USHORT)
            image = image.linear(1.0 / 257.0, 0.5);
        image = image.cast(VIPS_FORMAT_UCHAR);
        size_t size = 0;
        void *memory = image.write_to_memory(&size);
        std::vector<uint8_t> pixels(static_cast<const uint8_t *>(memory), static_cast<const uint8_t *>(memory) + size);
        g_free(memory);
        return MaskPaint(image.width(), image.height(), pixels);
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("media_unreadable"));
    }
}

MaskPaint MaskPaint::scaled(int width, int height) const {
    MaskPaint out(width, height);
    if (width == m_width && height == m_height) {
        out.m_pixels = m_pixels;
        return out;
    }
    const double fx = double(m_width) / out.m_width, fy = double(m_height) / out.m_height;
    for (int y = 0; y < out.m_height; ++y) {
        const double sy = std::clamp((y + 0.5) * fy - 0.5, 0.0, double(m_height - 1));
        const int y0 = int(sy), y1 = std::min(y0 + 1, m_height - 1);
        const double ty = sy - y0;
        for (int x = 0; x < out.m_width; ++x) {
            const double sx = std::clamp((x + 0.5) * fx - 0.5, 0.0, double(m_width - 1));
            const int x0 = int(sx), x1 = std::min(x0 + 1, m_width - 1);
            const double tx = sx - x0;
            const double top = at(x0, y0) * (1 - tx) + at(x1, y0) * tx;
            const double bottom = at(x0, y1) * (1 - tx) + at(x1, y1) * tx;
            out.m_pixels[size_t(y) * size_t(out.m_width) + size_t(x)] = uint8_t(std::lround(top * (1 - ty) + bottom * ty));
        }
    }
    return out;
}

} // namespace fugue::mask
