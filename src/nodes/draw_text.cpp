#include "nodes/draw_text.h"

// Ticket #549: Draw Text (spec 14.2).

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "nodes/kernels.h"
#include "nodes/pixel_io.h"
#include "nodes/text_raster.h"

namespace fugue::nodes {

namespace {

// `color` over the pixel at (x, y) with weight `cover` (0..1).
void blend(Pixels &out, int x, int y, const std::array<float, 4> &color, float cover) {
    const float a = color[3] * cover;
    if (a <= 0.0f)
        return;
    if (out.channels == 4) {
        const float dstA = out.at(x, y, 3);
        const float outA = a + dstA * (1.0f - a);
        for (int c = 0; c < 3; ++c)
            out.at(x, y, c) = outA > 0.0f ? (color[c] * a + out.at(x, y, c) * dstA * (1.0f - a)) / outA : 0.0f;
        out.at(x, y, 3) = outA;
    } else {
        for (int c = 0; c < 3; ++c)
            out.at(x, y, c) = color[c] * a + out.at(x, y, c) * (1.0f - a);
    }
}

} // namespace

Pixels drawText(const Pixels &image, const Pixels &coverage, const DrawTextStyle &style) {
    if (coverage.width == 0 || coverage.height == 0 || image.width == 0 || image.height == 0)
        return image;
    const int w = coverage.width, h = coverage.height;
    // The block's top left corner in the picture.
    int left = style.x, top = style.y;
    switch (style.anchor) {
    case TextAnchor::TopCenter: case TextAnchor::Center: case TextAnchor::BottomCenter: left -= w / 2; break;
    case TextAnchor::TopRight: case TextAnchor::CenterRight: case TextAnchor::BottomRight: left -= w; break;
    default: break;
    }
    switch (style.anchor) {
    case TextAnchor::CenterLeft: case TextAnchor::Center: case TextAnchor::CenterRight: top -= h / 2; break;
    case TextAnchor::BottomLeft: case TextAnchor::BottomCenter: case TextAnchor::BottomRight: top -= h; break;
    default: break;
    }
    Pixels out = image;
    const int r = std::max(0, style.outline);
    const auto covered = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h ? coverage.at(x, y, 0) : 0.0f; };

    // The box.
    if (style.boxColor[3] > 0.0f) {
        const int pad = style.boxPadding + r;
        for (int y = std::max(0, top - pad); y < std::min(image.height, top + h + pad); ++y)
            for (int x = std::max(0, left - pad); x < std::min(image.width, left + w + pad); ++x)
                blend(out, x, y, style.boxColor, 1.0f);
    }
    // The outline: the coverage grown by a disc of radius r. Only the glyphs' edge pixels need to
    // spread, the inside is reached by their neighbours.
    if (r > 0) {
        const int ow = w + 2 * r, oh = h + 2 * r;
        std::vector<float> grown(size_t(ow) * oh, 0.0f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float c = coverage.at(x, y, 0);
                if (c <= 0.0f || (c >= 1.0f && covered(x - 1, y) >= 1.0f && covered(x + 1, y) >= 1.0f &&
                                  covered(x, y - 1) >= 1.0f && covered(x, y + 1) >= 1.0f))
                    continue;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                        if (dx * dx + dy * dy <= r * r) {
                            float &g = grown[size_t(y + dy + r) * ow + (x + dx + r)];
                            g = std::max(g, c);
                        }
            }
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                const int px = left - r + x, py = top - r + y;
                if (px >= 0 && py >= 0 && px < image.width && py < image.height)
                    blend(out, px, py, style.outlineColor, grown[size_t(y) * ow + x]);
            }
    }
    // The text.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int px = left + x, py = top + y;
            if (px >= 0 && py >= 0 && px < image.width && py < image.height)
                blend(out, px, py, style.color, coverage.at(x, y, 0));
        }
    return out;
}

namespace {

constexpr std::pair<const char *, TextAnchor> kAnchors[] = {
    {"top-left", TextAnchor::TopLeft},         {"top-center", TextAnchor::TopCenter},       {"top-right", TextAnchor::TopRight},
    {"center-left", TextAnchor::CenterLeft},   {"center", TextAnchor::Center},              {"center-right", TextAnchor::CenterRight},
    {"bottom-left", TextAnchor::BottomLeft},   {"bottom-center", TextAnchor::BottomCenter}, {"bottom-right", TextAnchor::BottomRight},
};
constexpr std::pair<const char *, TextAlign> kAligns[] = {
    {"left", TextAlign::Left}, {"center", TextAlign::Center}, {"right", TextAlign::Right},
};

template <typename Enum, size_t N>
Result<Enum> pick(const NodeContext &context, const QString &input, const std::pair<const char *, Enum> (&names)[N]) {
    const QString text = context.input(input).text();
    for (const auto &[name, value] : names)
        if (text == QLatin1String(name))
            return value;
    return makeInputError(QStringLiteral("value_not_in_choices"), input,
                          QStringLiteral("\"%1\" is not a %2 this node knows.").arg(text, input));
}

class DrawTextKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const Result<TextAnchor> anchor = pick(context, QStringLiteral("anchor"), kAnchors);
        if (!anchor)
            return anchor.errors();
        const Result<TextAlign> align = pick(context, QStringLiteral("align"), kAligns);
        if (!align)
            return align.errors();
        DrawTextStyle style;
        style.anchor = *anchor;
        style.x = toInt(context.input("x"));
        style.y = toInt(context.input("y"));
        style.outline = toInt(context.input("outline"));
        style.boxPadding = toInt(context.input("box_padding"));
        for (const auto &[name, target] : {std::pair{"color", &style.color}, std::pair{"outline_color", &style.outlineColor},
                                           std::pair{"box_color", &style.boxColor}}) {
            if (QLatin1String(name) == QLatin1String("box_color") && !context.input("box").json().toBool(false))
                continue; // no box: its color is not read, so a bad one does not matter
            const Result<std::array<float, 4>> color = colorOf(QString::fromLatin1(name), context.input(name).text());
            if (!color)
                return color.errors();
            *target = color.value();
        }
        if (!context.input("box").json().toBool(false))
            style.boxColor[3] = 0.0f;
        const int size = toInt(context.input("size"));
        if (size < 4 || size > 2000)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("size"),
                                  QStringLiteral("A size of %1 is outside 4 to 2000.").arg(size));
        if (style.outline < 0 || style.outline > 16)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("outline"),
                                  QStringLiteral("An outline of %1 is outside 0 to 16.").arg(style.outline));
        const int maxWidth = toInt(context.input("max_width"));
        if (maxWidth < 0 || maxWidth > 16384)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("max_width"),
                                  QStringLiteral("A max width of %1 is outside 0 to 16384.").arg(maxWidth));

        // Only the bundled font: stb_truetype does no range checking ("DO NOT USE THIS ON UNTRUSTED FONT FILES"), and a
        // workflow or app shared with its files could bring a crafted .ttf. A font choice comes back with FreeType.
        const QByteArray fontBytes;
        const Result<Pixels> coverage = rasterizeText(context.input("text").text(), fontBytes, float(size), maxWidth, *align);
        if (!coverage)
            return coverage.errors();
        return imageOutput(drawText(image.value(), coverage.value(), style));
    }
};

} // namespace

void addDrawTextKernels(Kernels &kernels) {
    kernels.add("fugue/draw-text", std::make_shared<DrawTextKernel>());
}

} // namespace fugue::nodes
