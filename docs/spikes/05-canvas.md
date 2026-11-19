# Spike 05: canvas frame rate and the wire renderer

Issue: #5. Date: 2026-11-18. Verdict: **a Shape per wire, with the default geometry renderer.**

## Question

Does a hand-built QML canvas ([spec 12.2](../spec.md#122-construction)) meet the
[12.1](../spec.md#121-targets) target of about 60 frames per second with 300 nodes, and which wire
renderer should it use: one `Shape` holding every wire, a `Shape` per wire, or one `Shape` with
`CurveRenderer`?

## Answer

- **The target is met.** At 300 nodes and 568 wires, all three renderers hold 60 fps (the display's
  refresh rate) while panning and while dragging a node whose wires re-tessellate. The owner ran
  the prototype in their session and found it smooth; the look and feel was approved earlier.
- **CurveRenderer is out.** It costs three times as much per frame at 300 nodes, and dragging a node
  collapses at scale (15 fps at 1000 nodes, 5 fps at 2000), because every move re-triangulates.
- **One Shape and a Shape per wire cost the same** at every size measured. A Shape per wire is
  chosen because each wire is then its own item: adding or removing a wire does not rebuild the
  others' paths, and hover, selection, dimming and a running-wire highlight are per item.

## Setup

| | |
| --- | --- |
| Qt | 6.12.0 (Arch), threaded render loop, default RHI backend, Hyprland (Wayland) |
| Machine | desktop with the RTX 3090 of spikes 02 and 04; 1920x1080 at 60 Hz |
| Graph | `--grid N`: N filter nodes in a grid, each wired to its left and upper neighbour, plus the 5-node demo graph |
| Benchmarks | pan: the world slides 700 px and back over 3 s; drag: one middle node swings on a sine path for 3 s |
| Prototype | [spikes/canvas](../../spikes/canvas/) |

Because the display caps the rate at 60 fps, the frame rate alone cannot tell renderers apart.
`QSG_RENDER_TIMING=1` gives the work per frame: the GUI thread's polish and animations, and the
render thread's sync and render (excluding the wait for vsync in swap).

## Results

Average fps, pan / drag (all runs from the same session):

| Nodes (wires) | One Shape | Shape per wire | One Shape + CurveRenderer |
| --- | --- | --- | --- |
| 305 (568) | 60 / 60 | 60 / 60 | 60 / 60 |
| 1005 (1939) | 51 / 42 | 49 / 42 | 58 / 15 |
| 2005 (3911) | 17 / 16 | 17 / 16 | 30 / 5 |

Render-thread work per frame (sync + render), average and 99th percentile, ms:

| Nodes | One Shape | Shape per wire | CurveRenderer |
| --- | --- | --- | --- |
| 305 | 1.5, p99 4 | 1.5, p99 3 | 4.7, p99 11 |
| 1005 | 12.0, p99 19 | 12.2, p99 21 | 14.1, p99 53 |
| 2005 | 45.3, p99 101 | 45.9, p99 104 | 31.0, p99 116 |

GUI-thread work stayed under 4 ms on average in every run. At 300 nodes the worst gap between two
frames was 20 to 24 ms in every mode, so no dropped frames beyond one.

At 1000 nodes and above the default renderers fall under 60 fps in every case. The
same cost appears whichever wire mode is used, so it is the node items and their text, not the
wires. That is a concern for the canvas ticket at that scale, not for this choice.

## Decision

1. Wires: **one `Shape` per wire** in a `Repeater`, `Shape.GeometryRenderer` (the default). Not
   `CurveRenderer`.
2. The canvas meets 12.1 at 300 nodes with about 10x headroom on the render thread (1.5 ms of a
   16.7 ms frame).
3. For graphs over about 1000 nodes, the canvas ticket should look at the node items first
   (text, item count per node, culling off-screen nodes). Wires are not the bottleneck.

## Limits

- One machine and one 60 Hz display. A 144 Hz display was not tried; the render-thread figures
  suggest 300 nodes fit in its 6.9 ms frame for the two default modes, not measured.
- The prototype's wire list is a plain JavaScript array, so adding a wire rebuilds the
  delegates in both modes; the per-item advantage of a Shape per wire is from how Repeater and
  ListModel work, not measured here.
- Startup time and idle CPU use (also in 12.1) were not measured by this spike.
- A bug in the prototype's first version counted the idle wait before a benchmark as a frame gap
  (about 790 ms); it is fixed in the copy here.
