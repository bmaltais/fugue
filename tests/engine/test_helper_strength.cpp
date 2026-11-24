// How the helper reads a request's strength (helper/src/strength_args.h, #244): a noise level for a
// model on a shifted flow schedule, the library's step fraction for the Stable Diffusion family.
//
// Seam: the two pure functions of that header; the numbers behind it are in
// docs/inpaint-outpaint-research.md 4.7.

#include <QtTest>

#include <string>

#include "../../helper/src/strength_args.h"

using namespace fugue_helper;

class TestHelperStrength : public QObject {
    Q_OBJECT

private slots:
    void aFlowModelReadsStrengthAsANoiseLevel() {
        for (const char *model : {"Flux.2 klein", "Flux.2", "Flux", "Qwen Image", "Z-Image", "SD3.x", "Chroma"})
            QCOMPARE(strengthSampleArgs("", model, 0.5), std::string("strength_as_noise_level=true,force_first_sigma=true"));
    }

    void theStableDiffusionFamilyKeepsTheStepFraction() {
        for (const char *model : {"SD 1.x", "SD 2.x", "SDXL", "SDXL (Vega)", "SDXS (09)", "Instruct-Pix2Pix"}) {
            QVERIFY2(strengthIsStepFraction(model), model);
            QCOMPARE(strengthSampleArgs("", model, 0.5), std::string());
        }
    }

    void aFullRedrawNeedsNoReading() {
        QCOMPARE(strengthSampleArgs("", "Flux.2 klein", 1.0), std::string());
        QCOMPARE(strengthSampleArgs("max_shift=1", "Flux.2 klein", 1.0), std::string("max_shift=1"));
    }

    void theRequestsOwnArgumentsStay() {
        QCOMPARE(strengthSampleArgs("max_shift=1", "Flux.2 klein", 0.5),
                 std::string("max_shift=1,strength_as_noise_level=true,force_first_sigma=true"));
        // A request that chose a reading itself is not overridden.
        QCOMPARE(strengthSampleArgs("strength_as_noise_level=false", "Flux.2 klein", 0.5), std::string("strength_as_noise_level=false"));
    }

    // The preview is paced by the steps that run, not by the longer schedule a cut asks for (#359).
    void theStepsThatRunAreCountedForThePreview() {
        QCOMPARE(sampledSteps(10, "strength_as_noise_level=false", "Flux.2 klein", 0.55), 6);
        QCOMPARE(sampledSteps(10, "", "Flux.2 klein", 0.55), 10); // a noise level: the library's own count
        QCOMPARE(sampledSteps(20, "", "SDXL", 0.5), 11);
        QCOMPARE(sampledSteps(20, "", "SDXL", 1.0), 20);
    }
};

QTEST_GUILESS_MAIN(TestHelperStrength)
#include "test_helper_strength.moc"
