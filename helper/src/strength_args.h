// What "strength" means to a model (spec 8.10, docs/inpaint-outpaint-research.md 4.7, #244).
//
// The library's own reading of strength below 1 is a fraction of the steps: it runs the last
// int(steps * strength) + 1 of the schedule. For a model that samples on a shifted flow schedule
// (FLUX.2, FLUX, Qwen Image, Z-Image, ...) that is a poor scale: the first sigmas are crowded near 1
// (FLUX.2 at 4 steps: 1.0, 0.96, 0.88, 0.72), so 0.5 starts at sigma 0.96 and 0.75 is a full redraw,
// and more steps move the start only a little. The library has the other reading, a noise level: the
// start picture is mixed with noise to exactly that sigma, whatever the step count
// (strength_as_noise_level, with force_first_sigma to start at the exact value rather than the
// nearest scheduled one). A model of the older kind (Stable Diffusion, SDXL), whose schedules are
// close to even and whose strength people know as a step fraction, keeps the library's reading.
#pragma once

#include <string>

namespace fugue_helper {

// Whether the library's version name (sd_get_model_version_name) is a model whose strength is read as
// a step fraction: the Stable Diffusion family and what is built on it. Anything else is read as a
// noise level; a name the library adds later therefore gets the honest scale.
inline bool strengthIsStepFraction(const std::string &version) {
    static const char *const prefixes[] = {"SD 1.x", "SD 2.x", "SDXL", "SDXS", "SVD", "Instruct-Pix2Pix", "ESRGAN"};
    for (const char *prefix : prefixes)
        if (version.rfind(prefix, 0) == 0)
            return true;
    return false;
}

// The "sample.args" of a request: `args` (what the request sent, "key=value" pairs split by , or ;)
// plus the noise-level reading of strength when the request redraws part of a start picture
// (strength below 1) with a model that takes it that way, unless the request chose a reading itself.
inline std::string strengthSampleArgs(const std::string &args, const std::string &version, double strength) {
    if (strength >= 1.0 || strengthIsStepFraction(version) || args.find("strength_as_noise_level") != std::string::npos)
        return args;
    const std::string reading = "strength_as_noise_level=true,force_first_sigma=true";
    return args.empty() ? reading : args + "," + reading;
}

} // namespace fugue_helper
