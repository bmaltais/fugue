// Restore Image's colour fix (#346, spec 14.1): the wavelet reconstruction and the `lab` fix as
// plain functions on small synthetic pictures. No reference output of ComfyUI: its code is GPL and
// was not read, so each test states what the fix must do. Under 1 s.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <vector>

#include "nodes/restore_color.h"

using namespace fugue::nodes;

namespace {

Pixels flat(int w, int h, float r, float g, float b) {
    Pixels p(w, h, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            p.at(x, y, 0) = r;
            p.at(x, y, 1) = g;
            p.at(x, y, 2) = b;
        }
    return p;
}

// A smooth ramp (the low frequencies) in every channel, offset per channel.
Pixels ramp(int w, int h, float base) {
    Pixels p(w, h, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                p.at(x, y, c) = std::clamp(base + 0.3f * float(x) / w + 0.1f * float(y) / h + 0.05f * c, 0.0f, 1.0f);
    return p;
}

// Fine alternating stripes of +-amount (the highest frequency).
Pixels stripes(int w, int h, float mid, float amount) {
    Pixels p(w, h, 3, mid);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                p.at(x, y, c) += (x % 2 ? amount : -amount);
    return p;
}

Pixels sum(const Pixels &a, const Pixels &b) {
    Pixels out = a;
    for (size_t i = 0; i < out.data.size(); ++i)
        out.data[i] = std::clamp(a.data[i] + b.data[i] - 0.5f, 0.0f, 1.0f);
    return out;
}

// The picture's mean of a channel, over the inside (the borders blur differently).
double meanOf(const Pixels &p, int c, int margin = 0) {
    double s = 0;
    int n = 0;
    for (int y = margin; y < p.height - margin; ++y)
        for (int x = margin; x < p.width - margin; ++x, ++n)
            s += p.at(x, y, c);
    return s / n;
}

// The mean absolute difference between neighbouring columns: how much fine detail a picture has.
double detail(const Pixels &p, int c) {
    double s = 0;
    int n = 0;
    for (int y = 4; y < p.height - 4; ++y)
        for (int x = 4; x < p.width - 5; ++x, ++n)
            s += std::fabs(p.at(x + 1, y, c) - p.at(x, y, c));
    return s / n;
}

// A picture with detail at every scale: a few smooth waves, blocks and per-pixel noise, in 0.1..0.9.
Pixels busy(int w, int h, unsigned seed) {
    Pixels p(w, h, 3);
    unsigned state = seed * 2654435761u + 1u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                state = state * 1664525u + 1013904223u;
                const float noise = float((state >> 9) & 0x7FFF) / 32767.0f;
                const float wave = 0.5f + 0.25f * std::sin(0.11f * x + c) * std::cos(0.07f * y);
                const float block = ((x / 9 + y / 7) % 2) ? 0.08f : -0.08f;
                p.at(x, y, c) = std::clamp(wave + block + 0.12f * (noise - 0.5f), 0.02f, 0.98f);
            }
    return p;
}

// The wavelet reconstruction written the plain way, to pin what it must be: each level blurs the
// previous one with the 3x3 kernel [1 2 1; 2 4 2; 1 2 1] / 16 whose taps are `dilation` pixels
// apart (2^level, or `fixedDilation` when set), the edge pixel repeated beyond the border; the
// result is the content minus its own last blur plus the reference's, clamped to 0..1.
Pixels naiveLow(const Pixels &in, int levels, int fixedDilation) {
    Pixels current = in;
    for (int level = 0; level < levels; ++level) {
        const int d = fixedDilation ? fixedDilation : (1 << level);
        Pixels next = current;
        static const float k[3] = {1, 2, 1};
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x)
                for (int c = 0; c < in.channels; ++c) {
                    float sum = 0;
                    for (int j = -1; j <= 1; ++j)
                        for (int i = -1; i <= 1; ++i)
                            sum += k[j + 1] * k[i + 1] * current.at(std::clamp(x + i * d, 0, in.width - 1), std::clamp(y + j * d, 0, in.height - 1), c);
                    next.at(x, y, c) = sum / 16.0f;
                }
        current = next;
    }
    return current;
}

Pixels naiveWavelet(const Pixels &content, const Pixels &reference, int levels, int fixedDilation = 0) {
    const Pixels contentLow = naiveLow(content, levels, fixedDilation), referenceLow = naiveLow(reference, levels, fixedDilation);
    Pixels out = content;
    for (size_t i = 0; i < out.data.size(); ++i)
        out.data[i] = std::clamp(content.data[i] - contentLow.data[i] + referenceLow.data[i], 0.0f, 1.0f);
    return out;
}

float maxDifference(const Pixels &a, const Pixels &b) {
    float worst = 0;
    for (size_t i = 0; i < a.data.size(); ++i)
        worst = std::max(worst, std::fabs(a.data[i] - b.data[i]));
    return worst;
}

} // namespace

class TestRestoreColor : public QObject {
    Q_OBJECT

private slots:
    void aFlatPictureStaysFlatThroughBothFixes() {
        const Pixels content = flat(40, 40, 0.3f, 0.5f, 0.7f);
        const Pixels same = waveletReconstruction(content, content);
        const Pixels lab = labColorFix(content, content);
        for (size_t i = 0; i < content.data.size(); ++i) {
            QVERIFY(std::fabs(same.data[i] - content.data[i]) < 1e-4f);
            QVERIFY(std::fabs(lab.data[i] - content.data[i]) < 2e-3f);
        }
    }

    void aFlatContentTakesAFlatReferencesColour() {
        const Pixels out = labColorFix(flat(32, 32, 0.2f, 0.2f, 0.2f), flat(32, 32, 0.8f, 0.4f, 0.3f));
        // a and b are matched fully, L is 80 % content and 20 % reference: hue follows the reference.
        QVERIFY(out.at(5, 5, 0) > out.at(5, 5, 1) && out.at(5, 5, 1) > out.at(5, 5, 2) - 0.05f);
        QVERIFY(out.at(5, 5, 0) - out.at(5, 5, 2) > 0.1f);
    }

    void theWaveletIsFiveLevelsOfDilatedBinomialBlursWithRepeatedEdges() {
        const Pixels content = busy(72, 56, 1), reference = busy(72, 56, 2);
        const Pixels out = waveletReconstruction(content, reference);
        // Exactly the plain implementation of the description...
        QVERIFY2(maxDifference(out, naiveWavelet(content, reference, 5)) < 1e-4f, qPrintable(QString::number(maxDifference(out, naiveWavelet(content, reference, 5)))));
        // ...and not a version with another level count, or with every tap one pixel apart.
        QVERIFY(maxDifference(out, naiveWavelet(content, reference, 4)) > 0.003f);
        QVERIFY(maxDifference(out, naiveWavelet(content, reference, 6)) > 0.003f);
        QVERIFY(maxDifference(out, naiveWavelet(content, reference, 5, 1)) > 0.003f);
        QVERIFY(maxDifference(out, naiveWavelet(content, reference, 1)) > 0.003f);
        // A level count given is honoured.
        QVERIFY(maxDifference(waveletReconstruction(content, reference, 3), naiveWavelet(content, reference, 3)) < 1e-4f);
    }

    void lowFrequenciesFollowTheReferenceAndHighOnesTheContent() {
        // The content has the dark ramp and the fine stripes; the reference a bright flat colour.
        const Pixels content = sum(ramp(64, 64, 0.1f), stripes(64, 64, 0.5f, 0.05f));
        const Pixels reference = flat(64, 64, 0.7f, 0.7f, 0.7f);
        const Pixels out = waveletReconstruction(content, reference);
        for (int c = 0; c < 3; ++c) {
            // The mean moves to the reference's...
            QVERIFY(std::fabs(meanOf(out, c, 8) - 0.7) < 0.02);
            // ...and the stripes (the content's detail) stay, about as strong as they were.
            QVERIFY(std::fabs(detail(out, c) - detail(content, c)) < 0.02);
        }
        // The slow ramp is gone: a flat reference has no low-frequency variation, so the result's
        // left and right halves have the same mean.
        double left = 0, right = 0;
        for (int y = 8; y < 56; ++y)
            for (int x = 0; x < 64; ++x)
                (x < 32 ? left : right) += out.at(x, y, 1);
        QVERIFY(std::fabs(left - right) / (48 * 32) < 0.03);
    }

    void aReferenceOfAnotherSizeIsResampledToTheContent() {
        const Pixels out = waveletReconstruction(ramp(48, 48, 0.3f), flat(12, 12, 0.5f, 0.5f, 0.5f));
        QCOMPARE(out.width, 48);
        QCOMPARE(out.height, 48);
    }

    void histogramMatchingGivesTheReferencesDistributionAndKeepsTheOrder() {
        std::vector<float> values{0.1f, 0.4f, 0.2f, 0.3f};
        matchHistogram(values, {10.0f, 20.0f, 30.0f, 40.0f});
        QCOMPARE(values, (std::vector<float>{10.0f, 40.0f, 20.0f, 30.0f}));
        // A reference with another count: the values spread over its range, in the same order.
        std::vector<float> few{0.0f, 1.0f};
        matchHistogram(few, {5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f});
        QVERIFY(few[0] < few[1]);
        QVERIFY(few[0] >= 5.0f && few[1] <= 10.0f);
        std::vector<float> none;
        matchHistogram(none, {1.0f});
        QVERIFY(none.empty());
    }

    void aAndBHistogramsMatchTheReference() {
        // A cast-shifted picture of the same scene: reference warm, content cool.
        Pixels reference(40, 40, 3), content(40, 40, 3);
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {
                const float v = 0.2f + 0.6f * float((x * 7 + y * 13) % 40) / 40.0f;
                reference.at(x, y, 0) = v;
                reference.at(x, y, 1) = v * 0.8f;
                reference.at(x, y, 2) = v * 0.5f;
                content.at(x, y, 0) = v * 0.6f;
                content.at(x, y, 1) = v * 0.8f;
                content.at(x, y, 2) = v * 0.9f;
            }
        const Pixels out = labColorFix(content, reference);
        std::vector<float> outA, outB, refA, refB;
                for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {
                const Lab o = rgbToLab(out.at(x, y, 0), out.at(x, y, 1), out.at(x, y, 2));
                const Lab r = rgbToLab(reference.at(x, y, 0), reference.at(x, y, 1), reference.at(x, y, 2));
                outA.push_back(float(o.a));
                outB.push_back(float(o.b));
                refA.push_back(float(r.a));
                refB.push_back(float(r.b));
            }
        std::sort(outA.begin(), outA.end());
        std::sort(refA.begin(), refA.end());
        std::sort(outB.begin(), outB.end());
        std::sort(refB.begin(), refB.end());
        // Matched in a and b (to the 8-bit-free precision of the round trip through RGB and the clamp).
        double worst = 0;
        for (size_t i = 0; i < outA.size(); i += 20) {
            worst = std::max(worst, double(std::fabs(outA[i] - refA[i])));
            worst = std::max(worst, double(std::fabs(outB[i] - refB[i])));
        }
        QVERIFY2(worst < 3.0, qPrintable(QString::number(worst)));
    }

    // Grey pictures (a = b = 0, so no colour leaves the gamut): lightness alone, where the 0.8 / 0.2 mix shows.
    static Pixels greyRamp(int w, int h, float from, float to, unsigned seed) {
        Pixels p(w, h, 3);
        unsigned state = seed;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                state = state * 1664525u + 1013904223u;
                const float v = from + (to - from) * float(x + y * w) / float(w * h) + 0.01f * (float((state >> 9) & 0xFF) / 255.0f - 0.5f);
                for (int c = 0; c < 3; ++c)
                    p.at(x, y, c) = std::clamp(v, 0.0f, 1.0f);
            }
        return p;
    }

    void lightnessIsFourFifthsTheWaveletResultAndAFifthHistogramMatched() {
        // A dull content (narrow lightness range) against a reference that spans dark to bright.
        const Pixels content = greyRamp(60, 60, 0.35f, 0.50f, 3), reference = greyRamp(60, 60, 0.05f, 0.95f, 9);
        const Pixels wavelet = waveletReconstruction(content, reference);
        std::vector<float> own, target;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 60; ++x) {
                own.push_back(float(rgbToLab(wavelet.at(x, y, 0), wavelet.at(x, y, 1), wavelet.at(x, y, 2)).l));
                target.push_back(float(rgbToLab(reference.at(x, y, 0), reference.at(x, y, 1), reference.at(x, y, 2)).l));
            }
        std::vector<float> matched = own;
        matchHistogram(matched, target);
        double gap = 0; // how far apart the two lightnesses are: the weight must be visible in it
        for (size_t i = 0; i < own.size(); ++i)
            gap += std::fabs(matched[i] - own[i]);
        QVERIFY2(gap / own.size() > 2.0, qPrintable(QString::number(gap / own.size())));

        const Pixels out = labColorFix(content, reference);
        double worst = 0;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 60; ++x) {
                const size_t i = size_t(y) * 60 + x;
                const double expected = 0.8 * own[i] + 0.2 * matched[i];
                worst = std::max(worst, std::fabs(rgbToLab(out.at(x, y, 0), out.at(x, y, 1), out.at(x, y, 2)).l - expected));
            }
        // Half and half (a 0.5 weight) would be 0.3 * gap away, all matched (1.0) 0.8 * gap.
        QVERIFY2(worst < 0.3, qPrintable(QString::number(worst)));
    }

    void labAndWaveletDifferOnAPictureWhoseColoursAndLightnessMoved() {
        const Pixels content = busy(48, 48, 4), reference = busy(48, 48, 5);
        Pixels warm = reference;
        for (int y = 0; y < warm.height; ++y)
            for (int x = 0; x < warm.width; ++x) {
                warm.at(x, y, 0) = std::min(1.0f, warm.at(x, y, 0) * 1.3f);
                warm.at(x, y, 2) *= 0.6f;
            }
        QVERIFY(maxDifference(labColorFix(content, warm), waveletReconstruction(content, warm)) > 0.02f);
    }

    void theLabFixKeepsTheContentsDetail() {
        const Pixels content = sum(ramp(64, 64, 0.2f), stripes(64, 64, 0.5f, 0.05f));
        const Pixels out = labColorFix(content, ramp(64, 64, 0.5f));
        QVERIFY(detail(out, 1) > 0.5 * detail(content, 1));
    }

    void colourSpaceRoundTripsAndKnownColours() {
        const Lab white = rgbToLab(1, 1, 1), black = rgbToLab(0, 0, 0);
        QVERIFY(std::fabs(white.l - 100.0) < 0.05 && std::fabs(white.a) < 0.05 && std::fabs(white.b) < 0.05);
        QVERIFY(std::fabs(black.l) < 0.05);
        const Lab red = rgbToLab(1, 0, 0); // CIELAB of sRGB red, D65: 53.24, 80.09, 67.20
        QVERIFY(std::fabs(red.l - 53.24) < 0.1 && std::fabs(red.a - 80.09) < 0.1 && std::fabs(red.b - 67.20) < 0.1);
        float rgb[3];
        labToRgb(rgbToLab(0.25f, 0.5f, 0.75f), rgb);
        QVERIFY(std::fabs(rgb[0] - 0.25f) < 1e-4f && std::fabs(rgb[1] - 0.5f) < 1e-4f && std::fabs(rgb[2] - 0.75f) < 1e-4f);
    }
};

QTEST_GUILESS_MAIN(TestRestoreColor)
#include "test_restore_color.moc"
