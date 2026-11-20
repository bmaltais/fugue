#include "nodes/image_size.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>

#include "nodes/kernels.h"

namespace fugue::nodes {

QSize sizeForMegapixels(double aspect, double megapixels, int multiple) {
    const double area = megapixels * kMegapixel;
    const int w = std::max(1, int(std::lround(std::sqrt(area * aspect) / multiple))) * multiple;
    const int h = std::max(1, int(std::lround(area / w / multiple))) * multiple;
    return {w, h};
}

namespace {

// Image Size: "<w>:<h>" and megapixels in, Width and Height out.
class ImageSizeKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const QStringList sides = context.input(QStringLiteral("aspect")).text().split(QLatin1Char(':'));
        const double across = sides.value(0).toDouble(), down = sides.value(1).toDouble();
        if (sides.size() != 2 || across <= 0 || down <= 0) {
            Error error = makeInputError(QStringLiteral("value_not_in_choices"), QStringLiteral("aspect"),
                                         QStringLiteral("The aspect must be two numbers such as 16:9."));
            if (const InputSpec *aspect = context.type ? context.type->input(QStringLiteral("aspect")) : nullptr;
                aspect && aspect->choices)
                error.valid = QJsonArray::fromStringList(*aspect->choices); // 13.3: the options in full
            return error;
        }
        const QSize size = sizeForMegapixels(across / down, context.input(QStringLiteral("megapixels")).number(),
                                             std::max(1, int(context.input(QStringLiteral("multiple")).number())));
        return Values{{QStringLiteral("width"), Value(size.width())}, {QStringLiteral("height"), Value(size.height())}};
    }
};

} // namespace

void addImageSizeKernels(Kernels &kernels) { kernels.add("fugue/image-size", std::make_shared<ImageSizeKernel>()); }

} // namespace fugue::nodes
