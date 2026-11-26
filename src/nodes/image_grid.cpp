#include "nodes/image_grid.h"

// Image Grid (#552 first slice, spec 14.2): many pictures laid out as one sheet, with optional labels.

#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <memory>

#include "nodes/kernels.h"
#include "nodes/pixel_io.h"
#include "nodes/resample.h"
#include "nodes/text_raster.h"

namespace fugue::nodes {

namespace {

constexpr int kMaxImages = 64;
constexpr int kMaxSide = 16384;

// The text color that reads on `background`: near white on a dark one, near black on a light one.
std::array<float, 3> inkFor(const std::array<float, 4> &background) {
    const float luma = 0.2126f * background[0] + 0.7152f * background[1] + 0.0722f * background[2];
    return luma < 0.5f ? std::array<float, 3>{0.92f, 0.92f, 0.92f} : std::array<float, 3>{0.1f, 0.1f, 0.1f};
}

// `coverage` blended in `ink` over `out` with its top left corner at (left, top); clipped.
void blit(Pixels &out, const Pixels &coverage, int left, int top, const std::array<float, 3> &ink) {
    for (int y = 0; y < coverage.height; ++y) {
        const int py = top + y;
        if (py < 0 || py >= out.height)
            continue;
        for (int x = 0; x < coverage.width; ++x) {
            const int px = left + x;
            const float c = coverage.at(x, y, 0);
            if (px < 0 || px >= out.width || c <= 0.0f)
                continue;
            for (int k = 0; k < 3; ++k)
                out.at(px, py, k) = ink[k] * c + out.at(px, py, k) * (1.0f - c);
        }
    }
}

// One label per line, in order; a blank line is a cell with no label.
QStringList linesOf(const QString &text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &line : lines)
        line = line.trimmed();
    return lines;
}

// The picture on the background: over it where it has alpha.
void place(Pixels &out, const Pixels &image, int left, int top) {
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const float a = image.channels == 4 ? image.at(x, y, 3) : 1.0f;
            for (int k = 0; k < 3; ++k)
                out.at(left + x, top + y, k) = image.at(x, y, k) * a + out.at(left + x, top + y, k) * (1.0f - a);
        }
}

Result<Pixels> labelCoverage(const QString &text, float size, int maxWidth) {
    if (text.isEmpty())
        return Pixels();
    return rasterizeText(text, {}, size, maxWidth, TextAlign::Center);
}

class ImageGridKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const QList<Value> items = context.input(QStringLiteral("images")).items();
        if (items.isEmpty())
            return makeInputError(QStringLiteral("missing_input"), QStringLiteral("images"), QStringLiteral("There are no images to lay out."),
                                  QStringLiteral("Link at least one picture to Images."));
        if (items.size() > kMaxImages)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("images"),
                                  QStringLiteral("%1 images is over the limit of %2.").arg(items.size()).arg(kMaxImages));
        const int n = int(items.size());
        const int requested = toInt(context.input(QStringLiteral("columns")));
        if (requested < 0 || requested > kMaxImages)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("columns"),
                                  QStringLiteral("%1 columns is outside 0 to %2.").arg(requested).arg(kMaxImages));
        const int gap = toInt(context.input(QStringLiteral("gap")));
        if (gap < 0 || gap > 1000)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("gap"),
                                  QStringLiteral("A gap of %1 is outside 0 to 1000.").arg(gap));
        const int labelSizeInput = toInt(context.input(QStringLiteral("label_size")));
        if (labelSizeInput < 0 || labelSizeInput > 1000)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("label_size"),
                                  QStringLiteral("A label size of %1 is outside 0 to 1000.").arg(labelSizeInput));
        const Result<std::array<float, 4>> bg = colorOf(QStringLiteral("background"), context.input(QStringLiteral("background")).text());
        if (!bg)
            return bg.errors();

        std::vector<Pixels> images;
        int cellW = 1, cellH = 1;
        for (const Value &item : items) {
            const Result<Pixels> p = pixelsOf(item, QStringLiteral("images"));
            if (!p)
                return p.errors();
            images.push_back(p.value());
            cellW = std::max(cellW, images.back().width);
            cellH = std::max(cellH, images.back().height);
        }
        const int columns = requested > 0 ? std::min(requested, n) : int(std::ceil(std::sqrt(double(n))));
        const int rows = (n + columns - 1) / columns;
        const float labelSize = float(labelSizeInput > 0 ? labelSizeInput : std::max(10, int(std::lround(cellH * 0.03))));
        const QStringList cellLabels = linesOf(context.input(QStringLiteral("labels")).text());
        const QStringList columnLabels = linesOf(context.input(QStringLiteral("column_labels")).text());
        const QStringList rowLabels = linesOf(context.input(QStringLiteral("row_labels")).text());

        // Label pictures first: their sizes set the bands around the cells.
        std::vector<Pixels> cellCover(n), colCover(columns), rowCover(rows);
        int cellBand = 0, colBand = 0, rowBand = 0;
        const auto cover = [&](const QStringList &lines, int i, int maxWidth, Pixels &into) -> Result<Values> {
            if (i >= lines.size())
                return Values();
            const Result<Pixels> c = labelCoverage(lines.at(i), labelSize, maxWidth);
            if (!c)
                return c.errors();
            into = c.value();
            return Values();
        };
        for (int i = 0; i < n; ++i)
            if (const auto r = cover(cellLabels, i, cellW, cellCover[i]); !r)
                return r.errors();
        for (int i = 0; i < columns; ++i)
            if (const auto r = cover(columnLabels, i, cellW, colCover[i]); !r)
                return r.errors();
        for (int i = 0; i < rows; ++i)
            if (const auto r = cover(rowLabels, i, cellW, rowCover[i]); !r)
                return r.errors();
        for (const Pixels &p : cellCover)
            cellBand = std::max(cellBand, p.height);
        for (const Pixels &p : colCover)
            colBand = std::max(colBand, p.height);
        for (const Pixels &p : rowCover)
            rowBand = std::max(rowBand, p.width);
        const int labelPad = std::max(4, gap / 2);
        const int underCell = cellBand > 0 ? cellBand + labelPad : 0;
        const int aboveCols = colBand > 0 ? colBand + labelPad : 0;
        const int leftRows = rowBand > 0 ? rowBand + labelPad : 0;

        const long long width = (long long)gap + leftRows + (long long)columns * cellW + (long long)(columns - 1) * gap + gap;
        const long long height =
            (long long)gap + aboveCols + (long long)rows * (cellH + underCell) + (long long)(rows - 1) * gap + gap;
        if (width > kMaxSide || height > kMaxSide)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("images"),
                                  QStringLiteral("The grid would be %1 x %2 pixels, over the limit of %3 on a side.")
                                      .arg(width).arg(height).arg(kMaxSide),
                                  QStringLiteral("Use fewer images or columns, a smaller gap, or smaller pictures."));

        Pixels out(int(width), int(height), 3);
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x)
                for (int k = 0; k < 3; ++k)
                    out.at(x, y, k) = bg.value()[k];
        const std::array<float, 3> ink = inkFor(bg.value());
        const int originX = gap + leftRows, originY = gap + aboveCols;
        for (int i = 0; i < n; ++i) {
            const int col = i % columns, row = i / columns;
            const int cx = originX + col * (cellW + gap), cy = originY + row * (cellH + underCell + gap);
            const Pixels &image = images[i];
            place(out, image, cx + (cellW - image.width) / 2, cy + (cellH - image.height) / 2);
            const Pixels &label = cellCover[i];
            blit(out, label, cx + (cellW - label.width) / 2, cy + cellH + labelPad, ink);
            if (row == 0)
                blit(out, colCover[col], cx + (cellW - colCover[col].width) / 2, originY - aboveCols + (colBand - colCover[col].height), ink);
            if (col == 0)
                blit(out, rowCover[row], originX - leftRows + (rowBand - rowCover[row].width), cy + (cellH - rowCover[row].height) / 2, ink);
        }
        return imageOutput(std::move(out));
    }
};

} // namespace

void addImageGridKernels(Kernels &kernels) {
    kernels.add("fugue/image-grid", std::make_shared<ImageGridKernel>());
}

} // namespace fugue::nodes
