// Color Match (#180, spec 14.4): the registered kernel as the scheduler calls it, and the
// arithmetic under it (nodes/color_match.h) for the properties. No ComfyUI golden: the formulas are
// the paper's (Reinhard et al. 2001) and ComfyUI's color match is GPL, so the tests state what the
// transfer must do: undo a known shift, leave the masked pixels out of the statistics, and do
// nothing at strength 0.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/color_match.h"
#include "nodes/kernels.h"

using namespace fugue;
using nodes::Pixels;

namespace {

// Smooth, varied colors from a seed: every channel moves, in 0.15..0.85, so a transfer has room.
Pixels pattern(int w, int h, unsigned seed) {
    Pixels p(w, h, 3);
    unsigned s = seed * 2654435761u + 7u;
    for (float &v : p.data) {
        s = s * 1664525u + 1013904223u;
        v = 0.15f + 0.7f * float((s >> 8) & 0xFFFF) / 65535.0f;
    }
    return p;
}

// The same picture lit differently: gain and gamma on each channel.
Pixels shifted(const Pixels &in) {
    Pixels out = in;
    const float gain[3] = {1.0f, 0.9f, 0.75f};
    for (int i = 0; i < int(out.data.size()); ++i)
        out.data[i] = std::clamp(std::pow(in.data[i], 0.8f) * gain[i % 3], 0.0f, 1.0f);
    return out;
}

float maxDiff(const Pixels &a, const Pixels &b) {
    if (a.data.size() != b.data.size())
        return 1e9f;
    float m = 0;
    for (size_t i = 0; i < a.data.size(); ++i)
        m = std::max(m, std::fabs(a.data[i] - b.data[i]));
    return m;
}

float channelMean(const Pixels &p, int channel, int x0 = 0, int x1 = -1) {
    if (x1 < 0)
        x1 = p.width;
    double sum = 0;
    int n = 0;
    for (int y = 0; y < p.height; ++y)
        for (int x = x0; x < x1; ++x, ++n)
            sum += p.at(x, y, channel);
    return float(sum / n);
}

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

} // namespace

class TestColorMatch : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    Result<Values> run(const Values &inputs) {
        NodeContext context;
        context.node = "n";
        context.inputs = inputs;
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        return m_kernels.node("fugue/color-match")->run(context);
    }

private slots:
    void initTestCase() { nodes::addBuiltinKernels(m_kernels); }

    void aKnownShiftIsUndone() {
        const Pixels reference = pattern(48, 48, 1);
        const Pixels image = shifted(reference);
        QVERIFY(maxDiff(image, reference) > 0.1f);
        const Pixels out = nodes::colorMatch(image, reference, nullptr, 1.0f);
        // Each channel's mean is back, and the picture is much nearer the original than the shifted one.
        for (int c = 0; c < 3; ++c)
            QVERIFY2(std::fabs(channelMean(out, c) - channelMean(reference, c)) < 0.02f, qPrintable(QString::number(c)));
        double before = 0, after = 0;
        for (size_t i = 0; i < image.data.size(); ++i) {
            before += std::fabs(image.data[i] - reference.data[i]);
            after += std::fabs(out.data[i] - reference.data[i]);
        }
        QVERIFY(after < before * 0.35);
    }

    void aPictureMatchedToItselfStaysPut() {
        const Pixels image = pattern(32, 32, 2);
        QVERIFY(maxDiff(nodes::colorMatch(image, image, nullptr, 1.0f), image) < 1e-4f);
    }

    void strengthZeroIsTheIdentityAndHalfIsBetween() {
        const Pixels reference = pattern(32, 32, 3);
        const Pixels image = shifted(reference);
        QCOMPARE(nodes::colorMatch(image, reference, nullptr, 0.0f).data, image.data);
        const Pixels full = nodes::colorMatch(image, reference, nullptr, 1.0f);
        const Pixels half = nodes::colorMatch(image, reference, nullptr, 0.5f);
        for (int c = 0; c < 3; ++c) {
            const float a = channelMean(image, c), b = channelMean(full, c), h = channelMean(half, c);
            QVERIFY(h >= std::min(a, b) - 1e-4f && h <= std::max(a, b) + 1e-4f);
        }
        QVERIFY(maxDiff(half, image) > 1e-3f);
        QVERIFY(maxDiff(half, full) > 1e-3f);
    }

    void maskedPixelsAreLeftOutOfTheStatistics() {
        // The left half is the kept picture; the right half (masked) is a wild redraw. Measured from the
        // left half alone, the shift is undone there; measured from everything, the redraw spoils it.
        const Pixels base = pattern(40, 20, 4);
        Pixels image = shifted(base);
        Pixels mask(40, 20, 1, 0.0f);
        for (int y = 0; y < 20; ++y)
            for (int x = 20; x < 40; ++x) {
                mask.at(x, y, 0) = 1.0f;
                image.at(x, y, 0) = 0.0f;
                image.at(x, y, 1) = 1.0f;
                image.at(x, y, 2) = 0.0f;
            }
        const Pixels masked = nodes::colorMatch(image, base, &mask, 1.0f);
        const Pixels unmasked = nodes::colorMatch(image, base, nullptr, 1.0f);
        for (int c = 0; c < 3; ++c) {
            const float want = channelMean(base, c, 0, 20);
            QVERIFY(std::fabs(channelMean(masked, c, 0, 20) - want) < 0.02f);
        }
        float worst = 0;
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::fabs(channelMean(unmasked, c, 0, 20) - channelMean(base, c, 0, 20)));
        QVERIFY(worst > 0.05f);
    }

    void aMaskChangingOnlyMaskedPixelsChangesNothingInTheStatistics() {
        const Pixels reference = pattern(30, 30, 5);
        Pixels a = shifted(reference);
        Pixels mask(30, 30, 1, 0.0f);
        for (int y = 0; y < 30; ++y)
            for (int x = 20; x < 30; ++x)
                mask.at(x, y, 0) = 1.0f;
        Pixels b = a;
        for (int y = 0; y < 30; ++y)
            for (int x = 20; x < 30; ++x)
                b.at(x, y, 1) = 0.0f;
        const Pixels outA = nodes::colorMatch(a, reference, &mask, 1.0f);
        const Pixels outB = nodes::colorMatch(b, reference, &mask, 1.0f);
        for (int y = 0; y < 30; ++y)
            for (int x = 0; x < 20; ++x)
                for (int c = 0; c < 3; ++c)
                    QCOMPARE(outA.at(x, y, c), outB.at(x, y, c));
    }

    void aReferenceOfAnotherSizeIsMeasuredWhole() {
        const Pixels big = pattern(64, 64, 6);
        const Pixels image = shifted(big);
        const Pixels small = nodes::resample(big, 16, 16, nodes::ResizeMethod::Area);
        const Pixels out = nodes::colorMatch(image, small, nullptr, 1.0f);
        for (int c = 0; c < 3; ++c)
            QVERIFY(std::fabs(channelMean(out, c) - channelMean(small, c)) < 0.02f);
    }

    void aFlatPictureIsOnlyMovedAndNothingIsNaN() {
        const Pixels flat(8, 8, 3, 0.3f);
        const Pixels reference = pattern(8, 8, 7);
        const Pixels out = nodes::colorMatch(flat, reference, nullptr, 1.0f);
        for (float v : out.data)
            QVERIFY(std::isfinite(v) && v >= 0.0f && v <= 1.0f);
        for (int c = 0; c < 3; ++c)
            QVERIFY(std::fabs(channelMean(out, c) - channelMean(reference, c)) < 0.1f);
        // Black stays finite too.
        const Pixels black = nodes::colorMatch(Pixels(4, 4, 3, 0.0f), reference, nullptr, 1.0f);
        for (float v : black.data)
            QVERIFY(std::isfinite(v));
    }

    void alphaIsKept() {
        Pixels image(4, 4, 4, 0.5f);
        for (int i = 3; i < int(image.data.size()); i += 4)
            image.data[i] = 0.25f;
        const Pixels out = nodes::colorMatch(image, pattern(4, 4, 8), nullptr, 1.0f);
        QCOMPARE(out.channels, 4);
        for (int i = 3; i < int(out.data.size()); i += 4)
            QCOMPARE(out.data[i], 0.25f);
    }

    // ---- the node ----

    void theNodeMatchesTheFunctionAndIsDeterministic() {
        const Pixels reference = pattern(16, 16, 9);
        const Pixels image = shifted(reference);
        const Result<Values> r = run({{"image", Value(handleOf(image))}, {"reference", Value(handleOf(reference))}, {"strength", 1.0}});
        QVERIFY(r.ok());
        const Pixels out = pixelsFrom(r.value().value("image").media());
        QVERIFY(maxDiff(out, nodes::colorMatch(image, reference, nullptr, 1.0f)) < 1e-5f);
    }

    void theNodeTakesAMaskAndWarnsWhenItIsTheWrongSizeOrEverything() {
        const Pixels reference = pattern(16, 16, 10);
        const Pixels image = shifted(reference);
        Pixels half(8, 8, 1, 0.0f);
        const Result<Values> resized = run({{"image", Value(handleOf(image))}, {"reference", Value(handleOf(reference))},
                                            {"mask", Value(handleOf(half))}, {"strength", 1.0}});
        QVERIFY(resized.ok());
        QCOMPARE(resized.warnings().size(), 1);
        QCOMPARE(resized.warnings().value(0).code, QString("mask_resized"));

        const Result<Values> full = run({{"image", Value(handleOf(image))}, {"reference", Value(handleOf(reference))},
                                         {"mask", Value(handleOf(Pixels(16, 16, 1, 1.0f)))}, {"strength", 1.0}});
        QVERIFY(full.ok());
        QCOMPARE(full.warnings().value(0).code, QString("mask_full"));
        // The fallback measures everything: the same as no mask.
        QVERIFY(maxDiff(pixelsFrom(full.value().value("image").media()), nodes::colorMatch(image, reference, nullptr, 1.0f)) < 1e-5f);
    }

    void aStrengthOutsideZeroToOneIsRefused() {
        const Pixels p = pattern(4, 4, 11);
        for (double s : {-0.1, 1.5}) {
            const Result<Values> r = run({{"image", Value(handleOf(p))}, {"reference", Value(handleOf(p))}, {"strength", s}});
            QVERIFY(!r.ok());
            QCOMPARE(r.errors().value(0).code, QString("value_out_of_range"));
            QCOMPARE(r.errors().value(0).input, QString("strength"));
        }
    }

    // ---- Match Color (#555) ----

    static nodes::MatchColorOptions options(float luminance = 1.0f, float intensity = 1.0f, float neutralize = 0.0f) {
        nodes::MatchColorOptions o;
        o.luminance = luminance;
        o.colorIntensity = intensity;
        o.neutralize = neutralize;
        return o;
    }

    void matchColorUndoesAKnownShiftAndIsTheIdentityOnItself() {
        const Pixels reference = pattern(40, 40, 21);
        Pixels image = reference; // the same picture, much darker
        for (float &v : image.data)
            v = v * v * 0.6f;
        const Pixels out = nodes::matchColor(image, reference, nullptr, 1.0f, options());
        // L moves to the reference's mean: the mean brightness is nearer the reference's than the shifted picture's was.
        const auto luma = [](const Pixels &p) {
            double sum = 0;
            for (size_t i = 0; i < p.data.size(); i += 3)
                sum += 0.2126 * p.data[i] + 0.7152 * p.data[i + 1] + 0.0722 * p.data[i + 2];
            return sum / double(p.data.size() / 3);
        };
        QVERIFY2(std::fabs(luma(out) - luma(reference)) < 0.35 * std::fabs(luma(image) - luma(reference)), qPrintable(QString("ref %1 img %2 out %3").arg(luma(reference)).arg(luma(image)).arg(luma(out))));
        // A picture matched to itself is untouched: the ratios are 1 and the means are its own.
        QVERIFY(maxDiff(nodes::matchColor(reference, reference, nullptr, 1.0f, options()), reference) < 2e-3f);
    }

    void matchColorMovesTheColorsToTheReferences() {
        // A warm picture matched to a cool one turns cool, as Photoshop's Match Color does (cubiq's formula kept the hue).
        Pixels warm = pattern(30, 30, 24), cool = pattern(30, 30, 25);
        for (size_t i = 0; i < warm.data.size(); i += 3) {
            warm.data[i] = std::min(1.0f, warm.data[i] * 0.5f + 0.5f);
            warm.data[i + 2] *= 0.4f;
            cool.data[i] *= 0.4f;
            cool.data[i + 2] = std::min(1.0f, cool.data[i + 2] * 0.5f + 0.5f);
        }
        const Pixels out = nodes::matchColor(warm, cool, nullptr, 1.0f, options());
        QVERIFY(channelMean(warm, 0) > channelMean(warm, 2));
        QVERIFY(channelMean(out, 2) > channelMean(out, 0));
        QVERIFY(std::fabs((channelMean(out, 2) - channelMean(out, 0)) - (channelMean(cool, 2) - channelMean(cool, 0))) < 0.1);
    }

    void theSlidersDoWhatTheirNamesSay() {
        const Pixels reference = pattern(30, 30, 22);
        const Pixels image = shifted(pattern(30, 30, 23));
        // Color intensity 0 removes all color: every pixel is gray.
        const Pixels flat = nodes::matchColor(image, reference, nullptr, 1.0f, options(1.0f, 0.0f));
        for (int i = 0; i < 30 * 30; ++i) {
            QVERIFY(std::fabs(flat.data[size_t(i) * 3] - flat.data[size_t(i) * 3 + 1]) < 2e-3f);
            QVERIFY(std::fabs(flat.data[size_t(i) * 3 + 1] - flat.data[size_t(i) * 3 + 2]) < 2e-3f);
        }
        // Luminance 0 gives every pixel the reference's mean brightness: the gray one has no spread left.
        const Pixels dull = nodes::matchColor(image, reference, nullptr, 1.0f, options(0.0f, 0.0f));
        double lo = 1, hi = 0;
        for (size_t i = 0; i < dull.data.size(); i += 3) {
            lo = std::min<double>(lo, dull.data[i]);
            hi = std::max<double>(hi, dull.data[i]);
        }
        QVERIFY(hi - lo < 2e-3);
        // Neutralize takes the cast off the result: matched to a warm reference, the channel means draw together.
        Pixels warm = reference;
        for (size_t i = 0; i < warm.data.size(); i += 3) {
            warm.data[i] = std::min(1.0f, warm.data[i] * 1.0f + 0.15f);
            warm.data[i + 2] = warm.data[i + 2] * 0.6f;
        }
        const Pixels kept = nodes::matchColor(image, warm, nullptr, 1.0f, options(1.0f, 1.0f, 0.0f));
        const Pixels neutral = nodes::matchColor(image, warm, nullptr, 1.0f, options(1.0f, 1.0f, 1.0f));
        QVERIFY(maxDiff(kept, neutral) > 0.02f);
        QVERIFY(std::fabs(channelMean(neutral, 0) - channelMean(neutral, 2)) < std::fabs(channelMean(kept, 0) - channelMean(kept, 2)));
    }

    void matchColorFadesWithStrengthKeepsAlphaAndSurvivesAFlatPicture() {
        const Pixels reference = pattern(16, 16, 24);
        const Pixels image = shifted(reference);
        QCOMPARE(nodes::matchColor(image, reference, nullptr, 0.0f, options()).data, image.data);
        const Pixels full = nodes::matchColor(image, reference, nullptr, 1.0f, options());
        const Pixels half = nodes::matchColor(image, reference, nullptr, 0.5f, options());
        for (size_t i = 0; i < image.data.size(); ++i)
            QVERIFY(std::fabs(half.data[i] - (full.data[i] + image.data[i]) / 2) < 1e-5f);
        Pixels flat(8, 8, 4, 0.4f);
        flat.at(0, 0, 3) = 0.25f;
        const Pixels out = nodes::matchColor(flat, reference, nullptr, 1.0f, options());
        for (float v : out.data)
            QVERIFY(std::isfinite(v));
        QCOMPARE(out.at(0, 0, 3), 0.25f);
    }

    void matchColorMeasuresOnlyWhereTheMaskLeavesThePicture() {
        const Pixels base = pattern(40, 20, 25);
        Pixels image = shifted(base);
        Pixels mask(40, 20, 1, 0.0f);
        for (int y = 0; y < 20; ++y)
            for (int x = 20; x < 40; ++x) {
                mask.at(x, y, 0) = 1.0f;
                image.at(x, y, 0) = 0.0f;
                image.at(x, y, 1) = 1.0f;
                image.at(x, y, 2) = 0.0f;
            }
        Pixels spoiled = image; // another redraw under the mask: the kept half's result must not change
        for (int y = 0; y < 20; ++y)
            for (int x = 20; x < 40; ++x)
                spoiled.at(x, y, 2) = 1.0f;
        const Pixels a = nodes::matchColor(image, base, &mask, 1.0f, options());
        const Pixels b = nodes::matchColor(spoiled, base, &mask, 1.0f, options());
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x)
                for (int c = 0; c < 3; ++c)
                    QCOMPARE(a.at(x, y, c), b.at(x, y, c));
    }

    void theNodeKeepsOldWorkflowsAndTakesTheMethod() {
        const Pixels reference = pattern(16, 16, 26);
        const Pixels image = shifted(reference);
        const Values base{{"image", Value(handleOf(image))}, {"reference", Value(handleOf(reference))}, {"strength", 1.0}};
        const auto go = [&](Values more) {
            Values in = base;
            for (auto it = more.begin(); it != more.end(); ++it)
                in.insert(it.key(), it.value());
            return run(in);
        };
        // No method (a node saved as 1.0.0), and "reinhard": the same pixels as before.
        const Pixels old = pixelsFrom(go({}).value().value("image").media());
        QVERIFY(maxDiff(old, nodes::colorMatch(image, reference, nullptr, 1.0f)) < 1e-5f);
        QCOMPARE(pixelsFrom(go({{"method", "reinhard"}, {"luminance", 0.5}}).value().value("image").media()).data, old.data);
        const Result<Values> match = go({{"method", "match-color"}, {"luminance", 1.0}, {"color_intensity", 1.0}, {"neutralize", 0.0}});
        QVERIFY2(match.ok(), qPrintable(match.errors().value(0).message));
        QVERIFY(maxDiff(pixelsFrom(match.value().value("image").media()), nodes::matchColor(image, reference, nullptr, 1.0f, options())) < 1e-5f);
        QCOMPARE(go({{"method", "bogus"}}).errors().value(0).code, QString("value_not_in_choices"));
        const Result<Values> bad = go({{"method", "match-color"}, {"luminance", 3.0}});
        QCOMPARE(bad.errors().value(0).code, QString("value_out_of_range"));
        QCOMPARE(bad.errors().value(0).input, QString("luminance"));
    }

    void theManifestHasTheDesignsPorts() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const NodeType *type = registry.nodeType("fugue/color-match");
        QVERIFY(type);
        QVERIFY(m_kernels.node("fugue/color-match"));
        for (const InputSpec &input : type->inputs) {
            QCOMPARE(input.role == "mask", input.name == "mask");
            QCOMPARE(input.optional, input.name == "mask");
            if (input.name == "strength")
                QCOMPARE(input.defaultValue.toDouble(), 1.0);
        }
        QCOMPARE(type->inputs.size(), 8); // 1.1.0 added method, luminance, color_intensity and neutralize (#555)
        QCOMPARE(type->version.toString(), QString("1.1.0"));
    }
};

QTEST_GUILESS_MAIN(TestColorMatch)
#include "test_color_match.moc"
