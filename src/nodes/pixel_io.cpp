#include "nodes/pixel_io.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "engine/vips.h"

namespace fugue::nodes {

Result<Pixels> pixelsOf(const Value &value, const QString &input) {
    if (!value.isMedia())
        return makeInputError(QStringLiteral("missing_input"), input, QStringLiteral("Input %1 has no image.").arg(input));
    const Result<vips::VImage> loaded = value.media().image();
    if (!loaded.ok())
        return Result<Pixels>(loaded.errors());
    try {
        initVips();
        vips::VImage image = loaded.value();
        // Gray (with alpha) becomes RGB (with alpha); more than four bands keep the first three.
        if (image.bands() == 1) {
            image = vips::VImage::bandjoin(std::vector<vips::VImage>{image, image, image});
        } else if (image.bands() == 2) {
            const vips::VImage gray = image.extract_band(0);
            image = vips::VImage::bandjoin(std::vector<vips::VImage>{gray, gray, gray, image.extract_band(1)});
        } else if (image.bands() > 4) {
            image = image.extract_band(0, vips::VImage::option()->set("n", 3));
        }
        double divisor = 1.0;
        switch (image.format()) {
        case VIPS_FORMAT_UCHAR:
        case VIPS_FORMAT_CHAR:
            divisor = 255.0;
            break;
        case VIPS_FORMAT_USHORT:
        case VIPS_FORMAT_SHORT:
            divisor = 65535.0;
            break;
        default:
            break;
        }
        image = image.cast(VIPS_FORMAT_FLOAT);
        size_t size = 0;
        void *memory = image.write_to_memory(&size);
        Pixels pixels;
        pixels.width = image.width();
        pixels.height = image.height();
        pixels.channels = image.bands();
        const float *begin = static_cast<const float *>(memory);
        pixels.data.assign(begin, begin + size / sizeof(float));
        g_free(memory);
        if (divisor != 1.0)
            for (float &v : pixels.data)
                v = float(v / divisor);
        return pixels;
    } catch (const vips::VError &error) {
        Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
        unreadable.input = input;
        return unreadable;
    }
}

Result<Values> imageOutput(Pixels pixels) {
    for (float &v : pixels.data)
        v = std::min(1.0f, std::max(0.0f, v));
    try {
        initVips();
        const vips::VImage image =
            vips::VImage::new_from_memory_copy(pixels.data.data(), pixels.data.size() * sizeof(float), pixels.width,
                                               pixels.height, pixels.channels, VIPS_FORMAT_FLOAT);
        const Result<MediaHandle> handle = MediaHandle::fromImage(image);
        if (!handle.ok())
            return Result<Values>(handle.errors());
        return Values{{"image", Value(handle.value())}};
    } catch (const vips::VError &error) {
        return vipsError(error);
    }
}

// A number input as a whole number, held within plus or minus a billion so a huge value cannot
// wrap around (lround past the int range is undefined); not a number is 0.
int toInt(const Value &value) {
    const double rounded = std::round(value.number());
    return std::isfinite(rounded) ? int(std::clamp(rounded, -1e9, 1e9)) : 0;
}

// A mask input as one channel: an image's first channel (a gray mask is its own).
Result<Pixels> maskOf(const Value &value, const QString &input) {
    Result<Pixels> loaded = pixelsOf(value, input);
    if (!loaded.ok())
        return loaded;
    const Pixels &in = loaded.value();
    Pixels mask(in.width, in.height, 1);
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x)
            mask.at(x, y, 0) = in.at(x, y, 0);
    return mask;
}

// A result image under the output name `name` ("image", or "mask" for a one-channel mask).
Result<Values> namedOutput(Pixels pixels, const char *name) {
    Result<Values> made = imageOutput(std::move(pixels));
    if (!made.ok() || QLatin1String(name) == QLatin1String("image"))
        return made;
    Values out;
    out.insert(QString::fromLatin1(name), made.value().value("image"));
    return out;
}

} // namespace fugue::nodes
