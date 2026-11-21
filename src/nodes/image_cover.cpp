#include "nodes/image_cover.h"

#include <QFile>

#include <vips/vips8>

namespace fugue::nodes {

Result<QString> coverToFile(const MediaHandle &image, QSize size, const QString &path) {
    const auto options = [&] {
        return vips::VImage::option()->set("height", size.height())->set("crop", VIPS_INTERESTING_CENTRE);
    };
    vips::VImage fitted;
    if (image.hasFile()) {
        fitted = vips::VImage::thumbnail(QFile::encodeName(image.path()).constData(), size.width(), options());
    } else {
        const Result<vips::VImage> decoded = image.image();
        if (!decoded)
            return decoded.errors();
        fitted = decoded->thumbnail_image(size.width(), options());
    }
    fitted = fitted.colourspace(VIPS_INTERPRETATION_sRGB);
    if (fitted.bands() > 3)
        fitted = fitted.extract_band(0, vips::VImage::option()->set("n", 3));
    fitted.write_to_file(QFile::encodeName(path).constData());
    return path;
}

Errors outsideSizeLimits(const QString &model, const QJsonObject &limits, const QJsonObject &params,
                         const std::function<QString(const QString &side)> &blame) {
    Errors errors;
    for (const QString &side : {QStringLiteral("width"), QStringLiteral("height")}) {
        if (!params.contains(side))
            continue;
        const int value = params.value(side).toInt();
        const int least = limits.value(QStringLiteral("min_") + side).toInt(0);
        const int most = limits.value(QStringLiteral("max_") + side).toInt(0);
        if ((least > 0 && value < least) || (most > 0 && value > most))
            errors.append(makeInputError(QStringLiteral("value_out_of_range"), blame(side),
                                         QStringLiteral("%1 makes a %2 of %3 to %4 pixels, not %5.")
                                             .arg(model, side)
                                             .arg(least)
                                             .arg(most > 0 ? QString::number(most) : QStringLiteral("any"))
                                             .arg(value)));
    }
    return errors;
}

} // namespace fugue::nodes
