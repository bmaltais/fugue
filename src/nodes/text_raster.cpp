#include "nodes/text_raster.h"

// Ticket #549: stb_truetype (public domain, src/nodes/third_party) draws each glyph at its
// fractional pen position; the lines are laid out here (kern table, no shaping: Latin text).

#include <QFile>

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "nodes/third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fugue::nodes {

namespace {

constexpr int kMaxSide = 16384;

struct Line {
    std::vector<unsigned> glyphs;
    float width = 0;
};

class Metrics {
public:
    Metrics(const stbtt_fontinfo &info, float scale) : m_info(info), m_scale(scale) {}

    float advance(unsigned cp, unsigned next) const {
        int adv = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&m_info, int(cp), &adv, &bearing);
        int kern = next ? stbtt_GetCodepointKernAdvance(&m_info, int(cp), int(next)) : 0;
        return float(adv + kern) * m_scale;
    }

    // The width of the glyphs [from, to) of `cps`.
    float span(const std::vector<unsigned> &cps, size_t from, size_t to) const {
        float w = 0;
        for (size_t i = from; i < to; ++i)
            w += advance(cps[i], i + 1 < to ? cps[i + 1] : 0);
        return w;
    }

private:
    const stbtt_fontinfo &m_info;
    float m_scale;
};

bool isBreak(unsigned cp) { return cp == ' ' || cp == '\t'; }

// One paragraph (no newline) broken into lines at spaces, or inside a word that is too long.
void wrapParagraph(const std::vector<unsigned> &cps, const Metrics &metrics, int maxWidth, std::vector<Line> &lines) {
    const size_t n = cps.size();
    size_t start = 0;
    bool first = true;
    while (true) {
        if (!first) {
            while (start < n && isBreak(cps[start]))
                ++start; // a wrapped line does not begin with the space it broke at
            if (start >= n)
                return;
        }
        size_t end = n;
        if (maxWidth > 0 && metrics.span(cps, start, n) > float(maxWidth)) {
            size_t fit = start; // the characters from start that fit
            while (fit < n && metrics.span(cps, start, fit + 1) <= float(maxWidth))
                ++fit;
            size_t cut = fit;
            if (!(fit < n && isBreak(cps[fit])))
                for (size_t i = fit; i > start; --i)
                    if (isBreak(cps[i - 1])) {
                        cut = i;
                        break;
                    }
            end = cut > start ? cut : start + 1; // not even one character fits: one per line
        }
        size_t trimmed = end;
        while (trimmed > start && isBreak(cps[trimmed - 1]))
            --trimmed;
        Line line;
        line.glyphs.assign(cps.begin() + long(start), cps.begin() + long(trimmed));
        line.width = metrics.span(line.glyphs, 0, line.glyphs.size());
        lines.push_back(std::move(line));
        first = false;
        if (end >= n)
            return;
        start = end;
    }
}

} // namespace

QByteArray bundledFont() {
    QFile file(QStringLiteral(":/fugue/fonts/notosans-latin.ttf"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

Result<Pixels> rasterizeText(const QString &text, const QByteArray &fontBytes, float pixelSize, int maxWidth, TextAlign align) {
    const QByteArray bytes = fontBytes.isEmpty() ? bundledFont() : fontBytes;
    stbtt_fontinfo info;
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
    const int offset = bytes.isEmpty() ? -1 : stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&info, data, offset))
        return Result<Pixels>(Errors{makeInputError(QStringLiteral("invalid_font"), QStringLiteral("font"),
                                                    QStringLiteral("The font file is not a TrueType font."),
                                                    QStringLiteral("Pick a .ttf file, or clear the field to use the built-in font."))});
    const float scale = stbtt_ScaleForPixelHeight(&info, std::max(pixelSize, 1.0f));
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    const float lineHeight = float(ascent - descent + gap) * scale;
    const Metrics metrics(info, scale);

    std::vector<Line> lines;
    std::vector<unsigned> paragraph;
    bool any = false;
    const QList<uint> all = text.toUcs4();
    for (size_t i = 0; i <= size_t(all.size()); ++i) {
        const bool end = i == size_t(all.size());
        if (end || all[qsizetype(i)] == '\n') {
            wrapParagraph(paragraph, metrics, maxWidth, lines);
            paragraph.clear();
            continue;
        }
        if (all[qsizetype(i)] == '\r')
            continue;
        paragraph.push_back(all[qsizetype(i)]);
        any = any || !isBreak(all[qsizetype(i)]);
    }
    if (!any)
        return Pixels();

    float blockWidth = 0;
    for (const Line &line : lines)
        blockWidth = std::max(blockWidth, line.width);
    const int width = std::max(1, int(std::ceil(blockWidth)) + 1);
    const int height = std::max(1, int(std::ceil(lineHeight * float(lines.size()))));
    if (width > kMaxSide || height > kMaxSide)
        return Result<Pixels>(Errors{makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("text"),
                                                    QStringLiteral("The text would be %1 x %2 pixels; the limit is %3 a side.")
                                                        .arg(width).arg(height).arg(kMaxSide),
                                                    QStringLiteral("Use a smaller size or set a maximum width."))});

    Pixels out(width, height, 1, 0.0f);
    std::vector<unsigned char> bitmap;
    for (size_t row = 0; row < lines.size(); ++row) {
        const Line &line = lines[row];
        float pen = align == TextAlign::Left ? 0.0f : align == TextAlign::Center ? (blockWidth - line.width) / 2 : blockWidth - line.width;
        const float baseline = lineHeight * float(row) + float(ascent) * scale;
        for (size_t g = 0; g < line.glyphs.size(); ++g) {
            const unsigned cp = line.glyphs[g];
            const float fx = pen - std::floor(pen);
            const float fy = baseline - std::floor(baseline);
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBoxSubpixel(&info, int(cp), scale, scale, fx, fy, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                bitmap.assign(size_t(gw) * gh, 0);
                stbtt_MakeCodepointBitmapSubpixel(&info, bitmap.data(), gw, gh, gw, scale, scale, fx, fy, int(cp));
                const int ox = int(std::floor(pen)) + x0, oy = int(std::floor(baseline)) + y0;
                for (int j = 0; j < gh; ++j)
                    for (int i = 0; i < gw; ++i) {
                        const int x = ox + i, y = oy + j;
                        if (x < 0 || y < 0 || x >= width || y >= height)
                            continue;
                        float &v = out.at(x, y, 0);
                        v = std::min(1.0f, v + float(bitmap[size_t(j) * gw + i]) / 255.0f);
                    }
            }
            pen += metrics.advance(cp, g + 1 < line.glyphs.size() ? line.glyphs[g + 1] : 0);
        }
    }
    return out;
}

} // namespace fugue::nodes
