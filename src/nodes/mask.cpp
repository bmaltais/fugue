#include "nodes/kernels.h"

// Ticket #171: the mask and composite nodes (spec 14.4): Composite Masked, Feather Mask,
// Threshold Mask, Combine Masks, Fill Masked, Extend Edges (#243), Overlay Mask. The kernels only read the inputs and
// write the outputs; the arithmetic is mask_ops.h's, plain functions on float buffers.
//
// Composite Masked, Threshold Mask and Combine Masks are ComfyUI's ImageCompositeMasked,
// ThresholdMask and MaskComposite, checked against its own outputs (tests/nodes/golden/make_mask.py).
// The others have no ComfyUI equivalent to run; tests/nodes/test_mask_nodes.cpp states their properties.

#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "nodes/mask_ops.h"
#include "nodes/pixel_io.h"

namespace fugue::nodes {

namespace {

// Feather Mask's radius, as its manifest's `max`.
constexpr int kMaxFeather = 512;

// `value` as a choice from `names`, else `value_not_in_choices` on `input`.
template <typename Enum, size_t N>
Result<Enum> choiceOf(const NodeContext &context, const QString &input, const std::pair<const char *, Enum> (&names)[N]) {
    const QString text = context.input(input).text();
    for (const auto &[name, value] : names)
        if (text == QLatin1String(name))
            return value;
    return makeInputError(QStringLiteral("value_not_in_choices"), input,
                          QStringLiteral("\"%1\" is not a %2 this node knows.").arg(text, input));
}

class CompositeMaskedKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> destination = pixelsOf(context.input("destination"), "destination");
        if (!destination.ok())
            return Result<Values>(destination.errors());
        Result<Pixels> source = pixelsOf(context.input("source"), "source");
        if (!source.ok())
            return Result<Values>(source.errors());
        Result<Pixels> mask(Pixels{});
        const bool masked = context.input("mask").isMedia();
        if (masked) {
            mask = maskOf(context.input("mask"), "mask");
            if (!mask.ok())
                return Result<Values>(mask.errors());
        }
        Pixels placed = std::move(source.value());
        if (context.input("resize_source").json().toBool(false))
            placed = resample(placed, destination.value().width, destination.value().height, ResizeMethod::Bilinear);
        return imageOutput(compositeMasked(destination.value(), placed, masked ? &mask.value() : nullptr,
                                           toInt(context.input("x")), toInt(context.input("y"))));
    }
};

class FeatherMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> mask = maskOf(context.input("mask"), "mask");
        if (!mask.ok())
            return Result<Values>(mask.errors());
        static constexpr std::pair<const char *, FeatherDirection> kDirections[] = {
            {"outward", FeatherDirection::Outward}, {"inward", FeatherDirection::Inward},
            {"centered", FeatherDirection::Centered}};
        const Result<FeatherDirection> direction = choiceOf(context, QStringLiteral("direction"), kDirections);
        if (!direction)
            return direction.errors();
        const int radius = toInt(context.input("radius"));
        if (radius < 0 || radius > kMaxFeather)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("radius"),
                                  QStringLiteral("A radius of %1 is outside 0 to %2.").arg(radius).arg(kMaxFeather));
        return namedOutput(featherMask(mask.value(), radius, direction.value()), "mask");
    }
};

class ThresholdMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> mask = maskOf(context.input("mask"), "mask");
        if (!mask.ok())
            return Result<Values>(mask.errors());
        return namedOutput(thresholdMask(mask.value(), float(context.input("value").number())), "mask");
    }
};

class CombineMasksKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> destination = maskOf(context.input("destination"), "destination");
        if (!destination.ok())
            return Result<Values>(destination.errors());
        const Result<Pixels> source = maskOf(context.input("source"), "source");
        if (!source.ok())
            return Result<Values>(source.errors());
        static constexpr std::pair<const char *, CombineOperation> kOperations[] = {
            {"multiply", CombineOperation::Multiply}, {"add", CombineOperation::Add},
            {"subtract", CombineOperation::Subtract}, {"and", CombineOperation::And},
            {"or", CombineOperation::Or},             {"xor", CombineOperation::Xor}};
        const Result<CombineOperation> operation = choiceOf(context, QStringLiteral("operation"), kOperations);
        if (!operation)
            return operation.errors();
        return namedOutput(combineMasks(destination.value(), source.value(), operation.value(),
                                        toInt(context.input("x")), toInt(context.input("y"))),
                           "mask");
    }
};

class FillMaskedKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const Result<Pixels> mask = maskOf(context.input("mask"), "mask");
        if (!mask.ok())
            return Result<Values>(mask.errors());
        static constexpr std::pair<const char *, FillMode> kModes[] = {{"gray", FillMode::Gray}, {"blur", FillMode::Blur}};
        const Result<FillMode> mode = choiceOf(context, QStringLiteral("mode"), kModes);
        if (!mode)
            return mode.errors();
        return imageOutput(fillMasked(image.value(), mask.value(), mode.value()));
    }
};

// Extend Edges (#243): the masked part filled from the nearest picture, stretched outward and blurred.
class ExtendEdgesKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const Result<Pixels> mask = maskOf(context.input("mask"), "mask");
        if (!mask.ok())
            return Result<Values>(mask.errors());
        static constexpr std::pair<const char *, edgefill::Mode> kModes[] = {{"edge", edgefill::Mode::Edge}, {"edge-blur", edgefill::Mode::EdgeBlur}};
        const Result<edgefill::Mode> mode = choiceOf(context, QStringLiteral("mode"), kModes);
        if (!mode)
            return mode.errors();
        const int blur = int(std::lround(context.input("blur").number()));
        if (blur < 0 || blur > kMaxFeather)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("blur"),
                                  QStringLiteral("A blur of %1 is outside 0 to %2.").arg(blur).arg(kMaxFeather));
        return imageOutput(extendEdges(image.value(), mask.value(), mode.value(), blur));
    }
};

class OverlayMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const Result<Pixels> mask = maskOf(context.input("mask"), "mask");
        if (!mask.ok())
            return Result<Values>(mask.errors());
        const Result<std::array<float, 4>> color = colorOf(QStringLiteral("color"), context.input("color").text());
        if (!color.ok())
            return Result<Values>(color.errors());
        const std::array<float, 4> &c = color.value();
        return imageOutput(overlayMask(image.value(), mask.value(), {c[0], c[1], c[2]},
                                       float(context.input("opacity").number()) * c[3]));
    }
};

class MaskToAlphaKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const Result<Pixels> mask = maskOf(context.input("mask"), "mask");
        if (!mask.ok())
            return Result<Values>(mask.errors());
        return imageOutput(maskToAlpha(image.value(), mask.value()));
    }
};

} // namespace

void addMaskKernels(Kernels &kernels) {
    kernels.add("fugue/composite-masked", std::make_shared<CompositeMaskedKernel>());
    kernels.add("fugue/feather-mask", std::make_shared<FeatherMaskKernel>());
    kernels.add("fugue/threshold-mask", std::make_shared<ThresholdMaskKernel>());
    kernels.add("fugue/combine-masks", std::make_shared<CombineMasksKernel>());
    kernels.add("fugue/fill-masked", std::make_shared<FillMaskedKernel>());
    kernels.add("fugue/extend-edges", std::make_shared<ExtendEdgesKernel>());
    kernels.add("fugue/mask-to-alpha", std::make_shared<MaskToAlphaKernel>());
    kernels.add("fugue/overlay-mask", std::make_shared<OverlayMaskKernel>());
}

} // namespace fugue::nodes
