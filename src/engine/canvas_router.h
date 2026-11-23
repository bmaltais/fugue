#pragma once

// The canvas action router (#233): from what the person did on the canvas (a selection or none, a
// prompt or none, a crop frame past the canvas's edge), which generative action runs and on what
// region: Generative fill, Remove, Expand or Refine. Plain numbers and the standard library: it knows
// nothing of Qt, the store or the nodes, so the task bar, the command line (`fugue canvas act`) and a
// test all route the same way. The rules and the benchmark behind the numbers are in
// docs/inpaint-outpaint-research.md ("The router").
//
// Coordinates are canvas pixels with the origin at the canvas's top left, so a selection or a crop
// frame that reaches past the left or top edge has a negative x or y. The region a route returns is in
// the pixels of the canvas the action works on: the canvas after the resize for Expand (origin at the
// new top left), the canvas as it is otherwise.

#include <optional>
#include <string>
#include <vector>

namespace fugue::canvas {

struct RouteRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const RouteRect &o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
    bool operator!=(const RouteRect &o) const { return !(*this == o); }
};

enum class RouteAction {
    None,   // nothing to generate: the reasons say why (no selection and no prompt, an empty selection, a crop)
    Fill,   // Generative fill / replace: redraw the selection from the prompt, strength 1
    Remove, // redraw the selection from its surroundings, no prompt of the person's (a fixed hidden one)
    Expand, // grow the canvas, then fill the new border from the prompt and the picture next to it
    Refine, // redraw below strength 1, keeping the shapes and colours of what is there
};

const char *routeActionName(RouteAction action); // "none", "fill", "remove", "expand", "refine"

struct RouteInput {
    int canvasWidth = 0;
    int canvasHeight = 0;
    // The selection's bounds (a rectangle, or a mask's bounding box); empty when nothing is selected.
    // It may reach past the canvas: that asks for an Expand.
    std::optional<RouteRect> selection;
    // The share of the bounds that is selected, 0 to 1: 1 for a rectangle, less for a lasso or a mask.
    double coverage = 1.0;
    // The canvas bounds the crop tool asks for, in the canvas's pixels (x, y negative to grow left and
    // up); empty when the canvas keeps its size.
    std::optional<RouteRect> resize;
    bool hasPrompt = false;
    // A strength the person set below 1 in the bar's Advanced section; empty when left at the action's own.
    std::optional<double> strength;
};

// The numbers the router works with. The defaults are the benchmark's (docs/inpaint-outpaint-research.md).
struct RouteTuning {
    int minSelectedPixels = 64;          // below this the selection is empty (mask_empty)
    double refineCoverage = 0.9;         // a selection this large of the canvas, with a prompt, refines
    double refineStrength = 0.7;         // Refine's default strength: a noise level (#244); light 0.55, restyle 0.9 (4.7)
    double targetMegapixels = 1.0;       // the size a region is redrawn at (Region scale and cap)
    double maxScale = 4.0;               // the Region's own limit
    int minPadding = 64;                 // context around a selection, in canvas pixels
    int maxPadding = 256;
    double paddingFactor = 0.5;          // padding = this x the selection's longer side, held to the two above
    int removeMinPadding = 128;          // Remove needs more surroundings to continue
    int expandOverlap = 32;              // how far an Expand's fill reaches into the old picture
    int expandContext = 256;             // how much of the old picture an Expand's region keeps beside the border
    double expandStepRatio = 0.5;        // a side grown by more than this x the old picture's side takes several passes
};

struct Route {
    RouteAction action = RouteAction::None;
    RouteRect canvasAfter;   // the canvas's bounds after the action, in the old canvas's pixels (unchanged but Expand)
    RouteRect fill;          // the part to redraw (the selection's bounds; for Expand the new border grown by the overlap)
    RouteRect region;        // the crop the model sees: fill grown by padding (or context), held inside canvasAfter
    int padding = 0;         // the Region's padding (Expand: the context margin kept from the old picture)
    int overlap = 0;         // Expand: how far the fill reaches into the old picture
    double scale = 1.0;      // the Region's scale, so a small region is redrawn near targetMegapixels
    double strength = 1.0;   // the Sampler's denoise
    bool edgeContact = false; // the selection touches the canvas's edge (the region is moved inside)
    int passes = 1;          // Expand: how many steps the growth is split into
    std::vector<std::string> reasons;
};

Route routeAction(const RouteInput &input, const RouteTuning &tuning = {});

} // namespace fugue::canvas
