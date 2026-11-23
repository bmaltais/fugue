# Canvas action workflows (scratch, #233)

The recommended workflows behind the canvas actions of [canvas-actions-design.md](../canvas-actions-design.md):
Generative fill, Remove, Expand and Refine. They come out of the benchmark in
[inpaint-outpaint-research.md](../inpaint-outpaint-research.md) (section 5 has the reasons). They are scratch files
here, not templates: they move to fugue-workflows (category `canvas`) when the action slices of #231 land. Each is a
plain canvas workflow with the top-level `action` block of the design (1.2), so it also opens and runs from the
Graph today with the canvas id and the rectangle filled in. Generative fill is bundled with Fugue as
`share/actions/generative-fill.fugue` (#231 slice 1: Region 1.2 and a prompt that is not optional); the others follow
with their slices.

| Folder | Action | Model | Key settings |
| --- | --- | --- | --- |
| [generative-fill](generative-fill/workflow.fugue) | Generative fill / replace | FLUX.2 Klein 4B Q8 | the crop as reference, no pre-fill, 4 steps, guidance 1, strength 1, Blend 16 |
| [remove](remove/workflow.fugue) | Remove | FLUX.2 Klein 4B Q8 | Fill Masked `blur`, the blurred crop as reference, a fixed removal instruction as the prompt, padding at least 128 |
| [remove-lama](remove-lama/workflow.fugue) | Remove, with LaMa | FLUX.2 Klein 4B Q8 | as Remove, but LaMa Fill (pack [Remove Object](../../examples/packs/io.github.bmaltais.fugue-remove-object), #245) is the pre-fill instead of Fill Masked's blur; the same instruction and Klein pass follow |
| [expand](expand/workflow.fugue) | Expand (outpaint) | FLUX.2 Klein 4B Q8 | the new strip plus 32 px as the selection, 256 px of context (padding), the crop as reference, no colour match |
| [refine](refine/workflow.fugue) | Refine | FLUX.2 Klein 4B Q8 | strength 0.5 (0.75 to restyle), the crop as reference |

What the task bar sets in a run's copy (the router, `engine/canvas_router.h`, gives the numbers): the canvas and
the head on Canvas Region, the selection (rectangle or mask), the Region's `padding` and `scale`, and the bound
inputs (prompt, seed, strength, model, steps). The values in the files are what a run from the Graph uses.

**Model.** All four default to FLUX.2 Klein 4B (Apache-2.0, 8.9 GB with its encoder and VAE, 10 to 14 GB of GPU
memory at peak with a desktop running): on the benchmark it was as good as Klein 9B by eye, better on the small
rectangle and on Expand (where 9B handed back the blurred pre-fill on the portrait), and about twice as fast. Klein 9B (FLUX Non-Commercial,
19.3 GB, up to 24 GB at peak) stays a choice in the Advanced model input, with its licence shown.

**Two things the action schema does not have yet** (gap G8 in the research): a prompt the workflow uses when the
person leaves it empty. Here the Remove workflow has no prompt input at all (its instruction is the Text Encode's
literal), and Expand and Refine carry the proposed `"fallback"` field on their prompt role: an empty prompt sent to
Klein as it is changes nothing, so the runner must put the fallback in.

**Expand needs the canvas resize revision** (gap G1), which does not exist yet: the file assumes the canvas has
already grown and the new area was pre-filled by stretching the edge pixels and blurring them (24 px). On a canvas
that has not grown, its rectangle is the strip along one edge, as a test.

Checking a file: `fugue validate <folder>/workflow.fugue` (all four are valid); running one from the command line:
`fugue run <folder>/workflow.fugue --wait --set region.canvas=<id> --set region.x=... --set encode.prompt="..."`.
