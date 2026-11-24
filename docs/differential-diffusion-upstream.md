# Upstream pull request: differential diffusion (soft masks)

For the owner to send to leejet/stable-diffusion.cpp (#179). Not sent. The change Fugue carries is in its
fork (`bmaltais/stable-diffusion.cpp`, branch `fugue`; the `fugue.soft_mask` commits); an upstream version would use the library's own
option names. Written from the paper (arXiv 2306.00950, Algorithm 1); the authors' repository has no
licence and was not read.

**Title:** Soft masks: differential diffusion for inpainting with a gray mask

**Description**

A mask image is now rounded to 0 or 1 (`sd::ops::round`) before it is scaled to the latent, so a gray
mask is the same as a hard one. This adds an opt-in `soft_mask` image option that keeps the gray and
reads it as a change map, as in Differential Diffusion (Levin and Fried, 2023): a latent cell with mask
value `m` (1 = redraw, 0 = keep) is taken from the model at step `i` of `k` when `m > (k - 1 - i) / k` and
from the start latent otherwise. White is redrawn from the first step, black never, and 25% gray only in
the last quarter of the steps. The paper's map is the inverse (`mu = 1 - m`, mask `mu < (k - t) / k`,
`t = k - 1 - i`), so the test is the paper's.

- The generation templates: `mask_pixels` is not thresholded when `soft_mask` is on (still thresholded for
  an inpaint model, whose mask is a conditioning channel it was trained to see as 0 or 1).
- `src/dgl/models/diffusion_prediction.h`: `blend_mask`, the existing replacement
  `latent * m + base * (1 - m)`, is that same expression when `soft_mask` is off, so output is
  byte-identical to before. When on, `m` is replaced by a 0/1 mask for the step.
- The DGL `sample` built-in gains a defaulted `soft_mask` argument; the option is in `config.h` and
  `options.cpp`. The CLI flag name is up to you (for example `--soft-mask`).

No new dependency, nothing changes when the option is off. A hard mask gives the same picture on or
off, since 1 passes `m > (k - 1 - i) / k` at every step and 0 never does.

**Tested** (FLUX.2 Klein 4B, 768x768, 12 steps, one RTX 3090): with the option off the picture is
byte-identical to a build with the original expression; on, a left-to-right gradient mask gives a smooth
change from the kept picture to the redrawn one, and a hard mask gives the same bytes on or off.
