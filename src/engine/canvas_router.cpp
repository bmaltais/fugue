#include "engine/canvas_router.h"

#include <algorithm>
#include <cmath>

namespace fugue::canvas {

namespace {

long long area(const RouteRect &r) { return (long long)std::max(0, r.width) * std::max(0, r.height); }

RouteRect united(const RouteRect &a, const RouteRect &b) {
    const int left = std::min(a.x, b.x), top = std::min(a.y, b.y);
    const int right = std::max(a.x + a.width, b.x + b.width), bottom = std::max(a.y + a.height, b.y + b.height);
    return {left, top, right - left, bottom - top};
}

RouteRect intersected(const RouteRect &a, const RouteRect &b) {
    const int left = std::max(a.x, b.x), top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.width, b.x + b.width), bottom = std::min(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top)
        return {};
    return {left, top, right - left, bottom - top};
}

// One axis of a region: [start, start + length) grown by `grow` on each side, then moved inside
// [0, limit) with its length kept (all of the limit when it is longer), as Crop to Mask moves its box.
void placed(int start, int length, int grow, int limit, int *outStart, int *outLength) {
    const int len = length + 2 * grow;
    if (len >= limit) {
        *outStart = 0;
        *outLength = limit;
        return;
    }
    *outStart = std::clamp(start - grow, 0, limit - len);
    *outLength = len;
}

RouteRect grownInside(const RouteRect &r, int grow, int width, int height) {
    RouteRect out;
    placed(r.x, r.width, grow, width, &out.x, &out.width);
    placed(r.y, r.height, grow, height, &out.y, &out.height);
    return out;
}

// The Region's scale for a crop of this size: up to the target when it is clearly smaller (below 0.8 of
// it, InvokeAI's "near optimal" band), never down (the Region's megapixel cap does that).
double scaleFor(const RouteRect &region, const RouteTuning &t) {
    const double target = t.targetMegapixels * 1e6;
    const double a = double(area(region));
    if (a <= 0 || a >= 0.8 * target)
        return 1.0;
    return std::min(t.maxScale, std::sqrt(target / a));
}

Route none(const RouteRect &canvas, std::string why) {
    Route r;
    r.canvasAfter = canvas;
    r.reasons.push_back(std::move(why));
    return r;
}

// Expand: `after` is the canvas's new bounds in the old canvas's pixels.
Route expand(const RouteRect &canvas, const RouteRect &after, bool hasPrompt, const RouteTuning &t, std::string why) {
    Route r;
    r.action = RouteAction::Expand;
    r.canvasAfter = after;
    r.overlap = t.expandOverlap;
    r.padding = t.expandContext;
    r.strength = 1.0;
    r.reasons.push_back(std::move(why));
    // The old picture in the new canvas's pixels, and how much each side grows.
    const RouteRect old{canvas.x - after.x, canvas.y - after.y, canvas.width, canvas.height};
    const int growLeft = std::max(0, old.x), growTop = std::max(0, old.y);
    const int growRight = std::max(0, after.width - (old.x + old.width));
    const int growBottom = std::max(0, after.height - (old.y + old.height));
    // The fill: each grown strip, reaching `overlap` into the old picture; their bounds.
    std::optional<RouteRect> fill;
    const auto add = [&](RouteRect strip) {
        strip = intersected(strip, {0, 0, after.width, after.height});
        if (area(strip) > 0)
            fill = fill ? united(*fill, strip) : strip;
    };
    const int ov = t.expandOverlap;
    if (growLeft)
        add({0, 0, growLeft + ov, after.height});
    if (growTop)
        add({0, 0, after.width, growTop + ov});
    if (growRight)
        add({after.width - growRight - ov, 0, growRight + ov, after.height});
    if (growBottom)
        add({0, after.height - growBottom - ov, after.width, growBottom + ov});
    r.fill = fill.value_or(RouteRect{0, 0, after.width, after.height});
    // The region: the fill and `expandContext` of the old picture beside it, cut at the new canvas (not
    // moved: the fill already runs to the canvas's edge, so the context is all on the old picture's side).
    const int ctx = t.expandContext;
    r.region = intersected({r.fill.x - ctx, r.fill.y - ctx, r.fill.width + 2 * ctx, r.fill.height + 2 * ctx},
                           {0, 0, after.width, after.height});
    r.scale = scaleFor(r.region, t);
    // A side grown by more than expandStepRatio of the old picture's side is done in passes, each
    // continuing the last (Fooocus's advice: one side and about a megapixel at a time).
    const double ratio = std::max({double(growLeft) / canvas.width, double(growRight) / canvas.width,
                                   double(growTop) / canvas.height, double(growBottom) / canvas.height});
    r.passes = std::max(1, int(std::ceil(ratio / t.expandStepRatio - 1e-9)));
    if (r.passes > 1)
        r.reasons.push_back("The canvas grows by more than half its size on a side: " + std::to_string(r.passes)
                            + " passes, each continuing the last.");
    if (!hasPrompt)
        r.reasons.push_back("No prompt: the action's own instruction is used (\"Extend the picture beyond its edges, "
                            "continuing the scene.\"), which the benchmark found as good as a description of the scene.");
    return r;
}

} // namespace

const char *routeActionName(RouteAction action) {
    switch (action) {
    case RouteAction::None:
        return "none";
    case RouteAction::Fill:
        return "fill";
    case RouteAction::Remove:
        return "remove";
    case RouteAction::Expand:
        return "expand";
    case RouteAction::Refine:
        return "refine";
    }
    return "none";
}

Route routeAction(const RouteInput &in, const RouteTuning &t) {
    const RouteRect canvas{0, 0, in.canvasWidth, in.canvasHeight};
    if (in.canvasWidth <= 0 || in.canvasHeight <= 0)
        return none(canvas, "There is no canvas.");

    // 1. The crop frame: past the canvas on any side is Expand; wholly inside it is a crop.
    if (in.resize && area(*in.resize) > 0) {
        const RouteRect &frame = *in.resize;
        const bool grows = frame.x < 0 || frame.y < 0 || frame.x + frame.width > canvas.width
                           || frame.y + frame.height > canvas.height;
        if (!grows)
            return none(canvas, "The frame is inside the canvas: that is a crop, which needs no generation.");
        return expand(canvas, frame, in.hasPrompt, t, "The crop frame reaches past the canvas: Expand.");
    }

    // 2. No selection: a prompt refines the whole picture; nothing else can run.
    if (!in.selection || area(*in.selection) <= 0) {
        if (!in.hasPrompt)
            return none(canvas, "Select an area first, or type what to change in the whole picture.");
        Route r;
        r.action = RouteAction::Refine;
        r.canvasAfter = canvas;
        r.fill = r.region = canvas;
        r.strength = in.strength && *in.strength < 1.0 ? *in.strength : t.refineStrength;
        r.scale = scaleFor(r.region, t);
        r.reasons.push_back("Nothing is selected and there is a prompt: the whole picture is refined.");
        return r;
    }

    // 3. A selection past the edge grows the canvas to hold it: Expand, the whole new strip filled.
    const RouteRect &sel = *in.selection;
    if (sel.x < 0 || sel.y < 0 || sel.x + sel.width > canvas.width || sel.y + sel.height > canvas.height)
        return expand(canvas, united(canvas, sel), in.hasPrompt, t,
                      "The selection reaches past the canvas's edge: the canvas grows to hold it (Expand).");

    // 4. A selection too small to redraw.
    const double selected = double(area(sel)) * std::clamp(in.coverage, 0.0, 1.0);
    if (selected < t.minSelectedPixels)
        return none(canvas, "The selection is empty. Draw one, then Generate.");

    Route r;
    r.canvasAfter = canvas;
    r.fill = sel;
    r.edgeContact = sel.x == 0 || sel.y == 0 || sel.x + sel.width == canvas.width || sel.y + sel.height == canvas.height;
    const double share = selected / double(area(canvas));
    if (in.strength && *in.strength < 1.0) {
        r.action = RouteAction::Refine;
        r.strength = *in.strength;
        r.reasons.push_back("A strength below 1: Refine keeps the shapes and colours under the selection.");
    } else if (share >= t.refineCoverage) {
        if (!in.hasPrompt)
            return none(canvas, "The selection covers almost the whole picture and there is no prompt: nothing is left "
                                "to continue it from. Type what should be there.");
        r.action = RouteAction::Refine;
        r.strength = t.refineStrength;
        r.reasons.push_back("The selection covers almost the whole picture: a full redraw would replace it, so it is "
                            "refined.");
    } else if (!in.hasPrompt) {
        r.action = RouteAction::Remove;
        r.reasons.push_back("No prompt: the selection is removed and filled from its surroundings (the action's own "
                            "removal instruction).");
    } else {
        r.action = RouteAction::Fill;
        r.reasons.push_back("A selection and a prompt: Generative fill.");
    }

    const int longer = std::max(sel.width, sel.height);
    const int least = r.action == RouteAction::Remove ? std::max(t.minPadding, t.removeMinPadding) : t.minPadding;
    r.padding = std::clamp(int(std::floor(t.paddingFactor * longer)), least, std::max(least, t.maxPadding));
    r.region = grownInside(sel, r.padding, canvas.width, canvas.height);
    r.scale = scaleFor(r.region, t);
    if (r.edgeContact)
        r.reasons.push_back("The selection touches the canvas's edge: the region is moved inside, keeping its size.");
    return r;
}

} // namespace fugue::canvas
