// ComfyUI's reading of a denoise below 1 as a cut of a longer schedule (#359, epic #339).
//
// ComfyUI builds int(steps / denoise) steps and keeps the last `steps + 1` sigmas, so it always runs
// `steps` steps, whatever the denoise. The engine's slice_sigmas_by_strength keeps the last
// min(floor(N * strength) + 1, N) steps of an N-step schedule, so a strength of (steps - 0.5) / N
// keeps exactly `steps` of them: floor(steps - 0.5) + 1. The half step puts the cut away from the
// integer, where float rounding in the helper (it reads strength as a float) could drop a step.
#pragma once

#include <optional>

namespace fugue {

struct DenoiseCut {
    int steps;       // the length of the schedule to ask for (sample.steps)
    double strength; // the fraction of it to keep (strength)
};

// The cut for `steps` kept steps at `denoise`, or nothing when denoise is not in (0, 1): at 1 the
// whole schedule is run as asked, and at 0 or below there is nothing to run. N is computed in double
// as ComfyUI does (6 steps at 0.6 give 10, where a float would give 9).
inline std::optional<DenoiseCut> denoiseCut(int steps, double denoise) {
    if (steps < 1 || !(denoise > 0.0 && denoise < 1.0))
        return std::nullopt;
    const int total = static_cast<int>(steps / denoise);
    return DenoiseCut{total, (steps - 0.5) / total};
}

} // namespace fugue
