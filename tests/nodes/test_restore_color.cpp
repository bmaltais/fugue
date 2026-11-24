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

    void aPictureAgainstItselfIsTheIdentity() {
        const Pixels p = sum(ramp(48, 48, 0.2f), stripes(48, 48, 0.5f, 0.06f));
        const Pixels out = waveletReconstruction(p, p);
        for (size_t i = 0; i < p.data.size(); ++i)
            QVERIFY(std::fabs(out.data[i] - p.data[i]) < 1e-4f);
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

    void aAndBHistogramsMatchTheReferenceAndLMovesAFifthOfTheWay() {
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
        double outL = 0, refL = 0, wavL = 0;
        const Pixels wavelet = waveletReconstruction(content, reference);
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {
                const Lab o = rgbToLab(out.at(x, y, 0), out.at(x, y, 1), out.at(x, y, 2));
                const Lab r = rgbToLab(reference.at(x, y, 0), reference.at(x, y, 1), reference.at(x, y, 2));
                const Lab w = rgbToLab(wavelet.at(x, y, 0), wavelet.at(x, y, 1), wavelet.at(x, y, 2));
                outA.push_back(float(o.a));
                outB.push_back(float(o.b));
                refA.push_back(float(r.a));
                refB.push_back(float(r.b));
                outL += o.l;
                refL += r.l;
                wavL += w.l;
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
        // The mean L sits between the wavelet result's and the reference's, a fifth of the way.
        const double n = 40 * 40;
        const double expected = wavL / n + 0.2 * (refL / n - wavL / n);
        QVERIFY2(std::fabs(outL / n - expected) < 1.5, qPrintable(QString("%1 %2").arg(outL / n).arg(expected)));
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
