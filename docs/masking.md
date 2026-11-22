# Masks and inpainting

Everything here is built (#167); the design and its reasons are in
[spec 14.4](spec.md#144-masking-and-inpainting-167).

## What a mask is

A mask is a gray picture the size of the picture it goes with. **White is the part to change,
black the part to keep.** Gray is in between: it softens an edge when the result is put back.
Mask ports are drawn as hollow squares, and picture ports as filled squares.

You get a mask from:

- **Load Image**: paint on it (the Paint mask row on the node). With nothing painted, a picture
  with transparency gives its transparent part as the mask, as in ComfyUI.
- **Paint Mask**: paint over any picture, a generated one too.
- **Pad**: the new border of an outpaint.
- **Solid Mask**, **Image to Mask**, and the mask nodes that change one: Grow Mask, Feather Mask,
  Threshold Mask, Invert Mask, Combine Masks.

<p align="center"><img src="images/inpaint-result.png" alt="Inpaint: the picture with the armchair painted over, the result, and the difference, which shows nothing outside the mask" width="760"></p>

## Painting

Paint mask opens the editor at full size. Brush (B) and Eraser (E, or X to swap them) paint with
a round brush: `[` and `]` change its size and Shift with them its hardness, or Alt+drag does
both. Lasso (L) fills a shape you draw, Rectangle (R) a box. I inverts, Shift+Delete clears.
Ctrl+Z and Ctrl+Shift+Z undo and redo each stroke. The wheel zooms, a middle drag or Space+drag
pans, 0 fits and 1 shows 100%. H shows the mask alone. OK keeps the mask, Cancel or Escape drops
what you painted since opening.

<p align="center"><img src="images/mask-editor.png" alt="The mask editor over the Inpaint template's picture: the armchair painted in the accent color" width="760"></p>

The mask is saved as its own file beside your pictures, so the picture itself never changes, and
a saved workflow, a shared package and a template carry it. Paint Mask works the same way over a
picture that comes from another node, a generated one too:

<p align="center"><img src="images/mask-paint-node.png" alt="A Load Image node wired to a Paint Mask node: the mask output port is a hollow square, the picture ports filled" width="520"></p>

## Change a region: the Inpaint template

File > Browse templates > **Inpaint: change a region**.

<p align="center"><img src="images/inpaint-graph.png" alt="The Inpaint template: Load Image with its painted mask, the Inpaint (crop and stitch) group, Save Image and Preview" width="760"></p>

1. Put your picture in Load Image and paint over what should change. Paint a little past its edge.
   The template comes with an armchair painted over; click Paint mask to change it.
2. In the group's Prompt, say what should be there ("a mustard yellow velvet armchair").
3. Run. Only the painted part changes. The rest is your picture's own pixels, apart from the
   Blend width around the mask where the two are mixed.

The template's group, **Inpaint (crop and stitch)**, cuts out a box around the mask with room
around it (Padding), redraws that box at about 1 megapixel, whatever the size of your picture,
and puts it back with a soft edge (Blend). So a small region gets the model's full detail, and
no seam shows.

The group's settings, and what the template sets:

| Setting | Template | What it does |
| --- | --- | --- |
| Prompt | what should be there | Text Encode, with the crop as its reference: FLUX.2 Klein then keeps the shape and the light of what is there. Without the reference the new chair lost its legs and its place. |
| Steps, Guidance | 4 and 1 | FLUX.2 Klein is distilled. The group has no default for these, so each model's template sets them. |
| Denoise | 1 | 1 replaces what is under the mask. |
| Padding | 64 px | Room around the mask that the model sees. More gives more context and a smaller region at the same 1 megapixel. |
| Megapixels | 1 | The size the crop is redrawn at. 0 keeps the crop's own size. |
| Blend | 16 px | The soft edge outside the mask. Beyond it the picture is untouched. |
| Seed | random | |

<p align="center"><img src="images/mask-group.png" alt="Inside the Inpaint (crop and stitch) group: Crop to Mask, Text Encode, VAE Encode, Sampler, VAE Decode, Stitch" width="760"></p>

The group is made of six ordinary nodes (Crop to Mask, Text Encode, VAE Encode with the mask, Sampler,
VAE Decode, Stitch). Enter it to change any of them, or ungroup it.

## Extend a picture: the Outpaint template

<p align="center"><img src="images/outpaint-graph.png" alt="The Outpaint template: Load Image, Pad, Text Encode, VAE Encode with Pad's mask, Sampler, VAE Decode, Composite Masked, Save Image and Preview" width="760"></p>

**Outpaint: extend a picture** adds a border with Pad (256 px on the left and right, feathering
40), redraws only the border, and puts your picture back over the result with Composite Masked
through Pad's mask. The sample picture is 768 px square, so the padded one is about 1 megapixel,
the size FLUX.2 Klein works at. Set how much to add on each side in Pad, and describe the whole
scene in the prompt, not only the new part.

The prompt is not given the padded picture as a reference here. With it, FLUX.2 Klein at 4 steps
copied the gray border and left it gray; without it, the border is drawn from the prompt and the
picture around it. (The inpaint template is the other way: there the reference helps.)

## Tips

- **Seams.** Paint a little past the object, and keep Blend (Stitch) or a Feather Mask before
  Composite Masked. The model works on blocks of 8 or 16 pixels, so the exact edge always comes
  from the blend.
- **Small regions.** Use the crop-and-stitch group (or Crop to Mask and Stitch), not the whole
  picture, so the region is redrawn at the model's resolution. Crop to Mask gives the crop, its mask
  and a stitch handle (the diamond port) that remembers where the crop came from; wire the redrawn
  crop and that handle into Stitch. The crop must keep its shape (to within 2%) or Stitch says so.
  Padding 64 px, 1 megapixel and Blend 16 px are the defaults; Megapixels 0 keeps the crop's size.
- **Denoise.** 1 replaces what is under the mask. Lower keeps its shapes and colors. Below 1, put
  a Fill Masked (blur) before VAE Encode when the old content should not show through.
- **Color shift.** An edit can come back a little lighter, darker or tinted at the seam. Color Match
  (the picture, a reference, and the mask so only the kept part is measured) moves the picture's
  colors to the reference's; put it before Composite Masked or Stitch, and lower Strength to keep
  some of the new color. The reference is the original picture.
- **Wrong way round.** If the rest of the picture changed and the painted part did not, the mask
  is inverted: use Invert in the editor or an Invert Mask node.

## Models

These work with Fugue's helper:

- **Models made for inpainting**, which take the mask themselves: FLUX.1 Fill dev, SDXL Inpaint,
  SD 1.5 and 2 Inpaint, and Flex.2.
- **Any other picture model**, where the mask holds the rest of the picture in place at every
  step: FLUX.2 Klein 4B and 9B (the templates use 9B, checked at 4 steps on an RTX 3090: the masked
  part is redrawn and the rest is the picture's own), Qwen-Image Edit, FLUX.1 Kontext, FLUX.1
  dev, Z-Image, SDXL and SD 1.5.

Video models take no mask. Generate Image takes a mask only from a provider that supports one,
and otherwise says so (`mask_unsupported`, naming the models that can). Today that is an Omarchy
model whose sd-server lists `mask_image`; the Fugue helper redraws through the staged nodes (VAE
Encode with a mask, then a Sampler) instead. The mask must be the size of the start image, and the
result is put back over the start image through it, so everything outside the mask is the start
image's, to the pixel; put a Feather Mask in front for a soft edge.

**Soft mask (gray as a strength).** The Sampler's advanced input *Soft mask* reads the mask's gray as how
much to change (differential diffusion): white is redrawn from the first step, black is kept, and a gray
cell is held to the picture until late in the run, so a 25% gray changes only a little. It gives smooth
transitions with no seam, where a hard mask plus Feather Mask puts a softened picture back. It needs a
latent from VAE Encode with a mask, and it does nothing for an inpaint model (FLUX.1 Fill, SDXL Inpaint),
which reads its mask as white or black. Off, gray is rounded to white or black as before.

## From the command line

`fugue workflow new my.fugue --template inpaint-region` writes the template with its picture and
mask beside it; `fugue run my.fugue --wait` runs it. Change the prompt with
`--set inpaint.prompt="..."`. The painted mask is a file in `inputs/` like the picture, so
`--set load.mask=inputs/other-mask.png` changes it (an 8-bit gray PNG, white is redrawn).

## Not yet

Automatic masks (remove the background, pick an object by clicking it, or describe it in words)
will come as optional packs. Color matching for outpaints, and a paint canvas with layers are later issues.
