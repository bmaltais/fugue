#include "nodes/picture_sizes.h"

#include <algorithm>
#include <cmath>

#include "nodes/pixel_io.h"

namespace fugue::nodes::picturesize {

namespace {

// A computed side as a whole number that cannot overflow: far past kMaxSide it only has to stay past it.
int side(double value) { return int(std::min(value, 1e9)); }

} // namespace

int scaled(int length, int to, int from) { return std::max(1, int(std::nearbyint(double(length) * to / from))); }

QSize resize(QSize in, int width, int height, const QString &fit) {
    if (width == 0 && height == 0)
        return in;
    if (width == 0)
        width = scaled(in.width(), height, in.height());
    else if (height == 0)
        height = scaled(in.height(), width, in.width());
    if (fit == QLatin1String("contain")) {
        // Fit inside the box: the side that limits takes the box's size exactly.
        if (double(width) / in.width() <= double(height) / in.height())
            height = std::min(height, scaled(in.height(), width, in.width()));
        else
            width = std::min(width, scaled(in.width(), height, in.height()));
    }
    return {width, height}; // stretch, and cover (which trims the overflow)
}

QSize megapixels(QSize in, double megapixels, int multiple) {
    multiple = std::max(1, multiple);
    const double scale = std::sqrt(megapixels * 1024 * 1024 / (double(in.width()) * in.height()));
    const double width = std::nearbyint(in.width() * scale / multiple) * multiple;
    const double height = std::nearbyint(in.height() * scale / multiple) * multiple;
    return {width < 1 ? multiple : side(width), height < 1 ? multiple : side(height)};
}

QSize upscale(QSize in, double scale) {
    return {side(std::max(1.0, std::nearbyint(in.width() * scale))), side(std::max(1.0, std::nearbyint(in.height() * scale)))};
}

QSize crop(QSize in, int x, int y, int width, int height) {
    x = std::clamp(x, 0, in.width() - 1);
    y = std::clamp(y, 0, in.height() - 1);
    return {std::min(std::max(width, 1), in.width() - x), std::min(std::max(height, 1), in.height() - y)};
}

QSize pad(QSize in, int left, int top, int right, int bottom, int targetWidth, int targetHeight) {
    if (targetWidth > 0 && targetHeight > 0)
        return {targetWidth, targetHeight};
    return {in.width() + left + right, in.height() + top + bottom};
}

bool tooBig(QSize size) { return size.width() > kMaxSide || size.height() > kMaxSide; }

Error tooBigError(const QString &input, const QString &what, QSize size) {
    return makeInputError(QStringLiteral("value_out_of_range"), input,
                          QStringLiteral("%1 makes a picture of %2 x %3, past %4 on a side.")
                              .arg(what).arg(size.width()).arg(size.height()).arg(kMaxSide));
}

std::optional<int> wholeInput(const NodeContext &context, const char *name) {
    const Value value = context.input(QString::fromLatin1(name));
    return value.isEmpty() ? std::nullopt : std::optional<int>(toInt(value));
}

std::optional<double> numberInput(const NodeContext &context, const char *name) {
    const Value value = context.input(QString::fromLatin1(name));
    return value.isEmpty() ? std::nullopt : std::optional<double>(value.number());
}

PictureSizes SameSizeKernel::outputSizes(const NodeContext &context) const {
    const auto size = context.inputSizes.constFind(QStringLiteral("image"));
    return size == context.inputSizes.constEnd() ? PictureSizes() : PictureSizes{{QStringLiteral("image"), size.value()}};
}

} // namespace fugue::nodes::picturesize
