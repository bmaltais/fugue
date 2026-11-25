#include "nodes/tone_ops.h"

// Ticket #551: Desaturate, Posterize, Pixelize (spec 14.2), and their kernels.

#include <QString>

#include <algorithm>
#include <cmath>
#include <memory>

#include "nodes/kernels.h"
#include "nodes/pixel_io.h"

namespace fugue::nodes {

Pixels desaturate(const Pixels &image, float amount) {
    if (image.channels < 3)
        return image;
    Pixels out = image;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const float luma = 0.2126f * image.at(x, y, 0) + 0.7152f * image.at(x, y, 1) + 0.0722f * image.at(x, y, 2);
            for (int c = 0; c < 3; ++c)
                out.at(x, y, c) = image.at(x, y, c) + (luma - image.at(x, y, c)) * amount;
        }
    return out;
}

Pixels posterize(const Pixels &image, int levels) {
    levels = std::clamp(levels, 2, 256);
    const float steps = float(levels - 1);
    Pixels out = image;
    const int colors = std::min(image.channels, 3);
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            for (int c = 0; c < colors; ++c)
                out.at(x, y, c) = std::floor(std::clamp(image.at(x, y, c), 0.0f, 1.0f) * steps + 0.5f) / steps;
    return out;
}

Pixels pixelize(const Pixels &image, int block) {
    block = std::max(block, 1);
    Pixels out = image;
    for (int by = 0; by < image.height; by += block)
        for (int bx = 0; bx < image.width; bx += block) {
            const int x1 = std::min(bx + block, image.width), y1 = std::min(by + block, image.height);
            const float count = float((x1 - bx) * (y1 - by));
            for (int c = 0; c < image.channels; ++c) {
                double sum = 0;
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x)
                        sum += image.at(x, y, c);
                const float mean = float(sum / count);
                for (int y = by; y < y1; ++y)
                    for (int x = bx; x < x1; ++x)
                        out.at(x, y, c) = mean;
            }
        }
    return out;
}

namespace {

class DesaturateKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const double amount = context.input("amount").number();
        if (!(amount >= 0.0 && amount <= 1.0))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("amount"),
                                  QStringLiteral("An amount of %1 is outside 0 to 1.").arg(amount));
        return imageOutput(desaturate(image.value(), float(amount)));
    }
};

class PosterizeKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const int levels = toInt(context.input("levels"));
        if (levels < 2 || levels > 256)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("levels"),
                                  QStringLiteral("%1 levels is outside 2 to 256.").arg(levels));
        return imageOutput(posterize(image.value(), levels));
    }
};

class PixelizeKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const int block = toInt(context.input("block_size"));
        if (block < 1 || block > 4096)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("block_size"),
                                  QStringLiteral("A block size of %1 is outside 1 to 4096.").arg(block));
        return imageOutput(pixelize(image.value(), block));
    }
};

} // namespace

void addToneKernels(Kernels &kernels) {
    kernels.add("fugue/desaturate", std::make_shared<DesaturateKernel>());
    kernels.add("fugue/posterize", std::make_shared<PosterizeKernel>());
    kernels.add("fugue/pixelize", std::make_shared<PixelizeKernel>());
}

} // namespace fugue::nodes
