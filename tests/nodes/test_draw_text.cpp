// Draw Text (#549, spec 14.2): the rasterizer (nodes/text_raster.h), the compositing (nodes/draw_text.h)
// and the registered kernel. The test runs under QCoreApplication (QTEST_GUILESS_MAIN), as the CLI and
// the run worker do: the text must draw with no GUI application and no system font.

#include <QtTest>

#include <cmath>

#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/draw_text.h"
#include "nodes/kernels.h"
#include "nodes/text_raster.h"

using namespace fugue;
using nodes::Pixels;
using nodes::TextAlign;

namespace {

MediaHandle handleOf(const Pixels &p) {
    const vips::VImage image = vips::VImage::new_from_memory_copy(p.data.data(), p.data.size() * sizeof(float), p.width,
                                                                  p.height, p.channels, VIPS_FORMAT_FLOAT);
    const Result<MediaHandle> handle = MediaHandle::fromImage(image);
    return handle.ok() ? handle.value() : MediaHandle();
}

Pixels pixelsFrom(const MediaHandle &handle) {
    Pixels p;
    const Result<vips::VImage> image = handle.image();
    if (!image.ok())
        return p;
    const vips::VImage f = image.value().cast(VIPS_FORMAT_FLOAT);
    p = Pixels(f.width(), f.height(), f.bands());
    size_t size = 0;
    void *mem = f.write_to_memory(&size);
    p.data.assign(static_cast<float *>(mem), static_cast<float *>(mem) + size / sizeof(float));
    g_free(mem);
    return p;
}

double total(const Pixels &p) {
    double sum = 0;
    for (float v : p.data)
        sum += v;
    return sum;
}

// The bounding box of the pixels above `limit` in channel 0 (x0, y0, x1, y1); x1 < x0 when none.
std::array<int, 4> inkBox(const Pixels &p, float limit = 0.5f, int channel = 0) {
    std::array<int, 4> b = {p.width, p.height, -1, -1};
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            if (p.at(x, y, channel) > limit) {
                b = {std::min(b[0], x), std::min(b[1], y), std::max(b[2], x), std::max(b[3], y)};
            }
    return b;
}

} // namespace

class TestDrawText : public QObject {
    Q_OBJECT

    Kernels m_kernels;

private slots:
    void initTestCase() { nodes::addBuiltinKernels(m_kernels); }

    void theBundledFontDrawsWithoutAGuiApplication() {
        QVERIFY(!qApp || qApp->metaObject()->className() == QByteArray("QCoreApplication"));
        const Result<Pixels> a = nodes::rasterizeText("Hi", {}, 32, 0, TextAlign::Left);
        QVERIFY2(a.ok(), qPrintable(a.errors().value(0).message));
        QVERIFY(a->width > 10 && a->height >= 32 && a->height < 60);
        QVERIFY(total(*a) > 20.0);
        QCOMPARE(nodes::rasterizeText("Hi", {}, 32, 0, TextAlign::Left)->data, a->data); // deterministic
        // Larger text is larger, and a blank is nothing.
        QVERIFY(nodes::rasterizeText("Hi", {}, 64, 0, TextAlign::Left)->width > a->width * 3 / 2);
        QCOMPARE(nodes::rasterizeText("   \n ", {}, 32, 0, TextAlign::Left)->width, 0);
    }

    void linesBreakAtNewlinesAndAtMaxWidth() {
        const int h = nodes::rasterizeText("A", {}, 32, 0, TextAlign::Left)->height;
        QCOMPARE(nodes::rasterizeText("A\nA", {}, 32, 0, TextAlign::Left)->height, 2 * h);
        const Pixels oneLine = nodes::rasterizeText("aaaa bbbb cccc dddd", {}, 24, 0, TextAlign::Left).value();
        const Pixels wrapped = nodes::rasterizeText("aaaa bbbb cccc dddd", {}, 24, oneLine.width / 3, TextAlign::Left).value();
        QVERIFY(wrapped.width <= oneLine.width / 3 + 1);
        QVERIFY(wrapped.height >= 3 * (oneLine.height - 1));
        // A word wider than the limit breaks between letters instead of overflowing.
        const Pixels word = nodes::rasterizeText("mmmmmmmmmm", {}, 24, 40, TextAlign::Left).value();
        QVERIFY(word.width <= 41 && word.height > oneLine.height);
    }

    void alignmentMovesTheShortLine() {
        const auto left = nodes::rasterizeText("WWWWWW\ni", {}, 32, 0, TextAlign::Left).value();
        const auto right = nodes::rasterizeText("WWWWWW\ni", {}, 32, 0, TextAlign::Right).value();
        const int lineH = left.height / 2;
        const auto lowerLeft = [&](const Pixels &p) { // the ink of the second line
            Pixels sub(p.width, lineH, 1);
            for (int y = 0; y < lineH; ++y)
                for (int x = 0; x < p.width; ++x)
                    sub.at(x, y, 0) = p.at(x, y + lineH, 0);
            return inkBox(sub, 0.3f);
        };
        QVERIFY(lowerLeft(right)[0] > lowerLeft(left)[0] + left.width / 2);
    }

    void notAFontIsAnError() {
        const Result<Pixels> r = nodes::rasterizeText("x", QByteArray("this is not a font file at all"), 32, 0, TextAlign::Left);
        QVERIFY(!r.ok());
        QCOMPARE(r.errors().value(0).code, QString("invalid_font"));
    }

    void textIsWrittenOnlyWhereItsBoxIsAndClips() {
        const Pixels cover = nodes::rasterizeText("Hi", {}, 32, 0, TextAlign::Left).value();
        Pixels image(200, 100, 3, 0.0f);
        nodes::DrawTextStyle style;
        style.x = 20;
        style.y = 30;
        const Pixels out = nodes::drawText(image, cover, style);
        const auto box = inkBox(out, 0.5f);
        QVERIFY(box[0] >= 20 && box[1] >= 30 && box[2] < 20 + cover.width && box[3] < 30 + cover.height);
        QVERIFY(total(out) > 20.0);
        // Bottom-right at the corner puts the block inside the picture; off the picture draws nothing and does not crash.
        style.anchor = nodes::TextAnchor::BottomRight;
        style.x = 200;
        style.y = 100;
        const auto corner = inkBox(nodes::drawText(image, cover, style), 0.5f);
        QVERIFY(corner[2] <= 199 && corner[3] <= 99 && corner[0] > 200 - cover.width - 1);
        style.x = 5000;
        QCOMPARE(nodes::drawText(image, cover, style).data, image.data);
    }

    void outlineAndBoxAndAlpha() {
        const Pixels cover = nodes::rasterizeText("I", {}, 40, 0, TextAlign::Left).value();
        Pixels image(100, 100, 4, 0.0f); // transparent black
        nodes::DrawTextStyle style;
        style.x = 30;
        style.y = 30;
        style.color = {1, 0, 0, 1};
        const Pixels plain = nodes::drawText(image, cover, style);
        style.outline = 3;
        style.outlineColor = {0, 1, 0, 1};
        const Pixels outlined = nodes::drawText(image, cover, style);
        // The outline lies outside the text, in its own color, and the text keeps its red on top.
        int green = 0;
        for (int y = 0; y < 100; ++y)
            for (int x = 0; x < 100; ++x)
                if (outlined.at(x, y, 1) > 0.9f && outlined.at(x, y, 3) > 0.9f && plain.at(x, y, 3) < 0.1f)
                    ++green;
        QVERIFY(green > 30);
        QVERIFY(inkBox(outlined, 0.9f, 3)[0] < inkBox(plain, 0.9f, 3)[0]);
        // A box covers the whole block with its padding; alpha of a see-through color is kept in the result.
        style.outline = 0;
        style.boxColor = {0, 0, 1, 0.5f};
        style.boxPadding = 4;
        const Pixels boxed = nodes::drawText(image, cover, style);
        QVERIFY(std::fabs(boxed.at(27, 27, 3) - 0.5f) < 1e-6f && boxed.at(27, 27, 2) > 0.99f);
        QCOMPARE(boxed.at(25, 25, 3), 0.0f);
    }

    void theKernelRunsLikeTheCli() {
        Pixels image(120, 60, 3, 0.2f);
        NodeContext context;
        context.node = "n";
        context.inputs = Values{{"image", Value(handleOf(image))}, {"text", Value(QString("Fugue"))}, {"anchor", Value(QString("center"))},
                                {"x", 60}, {"y", 30}, {"size", 24}, {"color", Value(QString("#ffffff"))}, {"max_width", 0},
                                {"align", Value(QString("left"))}, {"outline", 1}, {"outline_color", Value(QString("#000000"))},
                                {"box", Value(false)}, {"box_color", Value(QString("#000000aa"))}, {"box_padding", 8}};
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        const Result<Values> r = m_kernels.node("fugue/draw-text")->run(context);
        QVERIFY2(r.ok(), qPrintable(r.errors().value(0).message));
        const Pixels out = pixelsFrom(r.value().value("image").media());
        QCOMPARE(out.width, 120);
        QVERIFY(inkBox(out, 0.9f)[2] >= 0); // white ink on the grey
        context.inputs.insert("anchor", Value(QString("middle")));
        QCOMPARE(m_kernels.node("fugue/draw-text")->run(context).errors().value(0).code, QString("value_not_in_choices"));
        context.inputs.insert("anchor", Value(QString("center")));
        context.inputs.insert("color", Value(QString("white")));
        QCOMPARE(m_kernels.node("fugue/draw-text")->run(context).errors().value(0).code, QString("value_out_of_range"));
    }
};

QTEST_GUILESS_MAIN(TestDrawText)
#include "test_draw_text.moc"
