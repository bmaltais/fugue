# Masks and inpainting

Status: designed in [spec 14.4](spec.md#144-masking-and-inpainting-167) and being built (#167).
This page describes the finished workflow. Until each part is merged, the nodes it names may not
exist yet.

## What a mask is

A mask is a gray picture the size of the picture it goes with. **White is the part to change,
black the part to keep.** Gray is in between: it softens an edge when the result is put back.
Mask ports are drawn as hollow squares, and picture ports as filled squares.

You get a mask from:

- **Load Image**: paint on it (Paint mask under the picture). With nothing painted, a picture
  with transparency gives its transparent part as the mask, as in ComfyUI.
- **Paint Mask**: paint over any picture, a generated one too.
- **Pad**: the new border of an outpaint.
- **Solid Mask**, **Image to Mask**, and the mask nodes that change one: Grow Mask, Feather Mask,
  Threshold Mask, Invert Mask, Combine Masks.

## Painting

Paint mask opens the editor at full size. Brush (B) and Eraser (E, or X to swap them) paint with
a round brush: `[` and `]` change its size and Shift with them its hardness, or Alt+drag does
both. Lasso (L) fills a shape you draw, Rectangle (R) a box. I inverts, Shift+Delete clears.
Ctrl+Z and Ctrl+Shift+Z undo and redo each stroke. The wheel zooms, a middle drag or Space+drag
pans, 0 fits and 1 shows 100%. H shows the mask alone. OK keeps the mask, Cancel or Escape drops
what you painted since opening.

The mask is saved as its own file beside your pictures, so the picture itself never changes, and
a saved workflow, a shared package and a template carry it.

## Change a region: the Inpaint template

File > Browse templates > **Inpaint: change a region**.

1. Put your picture in Load Image and paint over what should change. Paint a little past its edge.
2. In the prompt, say what should be there ("a red umbrella").
3. Run. Only the painted part changes. The rest is your picture's own pixels.

The template's group, **Inpaint (crop and stitch)**, cuts out a box around the mask with room
around it (Padding), redraws that box at about 1 megapixel, whatever the size of your picture,
and puts it back with a soft edge (Blend). So a small region gets the model's full detail, and
no seam shows.

## Extend a picture: the Outpaint template

**Outpaint: extend a picture** adds a border with Pad, redraws only the border, and puts your
picture back over the result. Set how much to add on each side in Pad.

## Tips

- **Seams.** Paint a little past the object, and keep Blend (Stitch) or a Feather Mask before
  Composite Masked. The model works on blocks of 8 or 16 pixels, so the exact edge always comes
  from the blend.
- **Small regions.** Use the crop-and-stitch group (or Crop to Mask and Stitch), not the whole
  picture, so the region is redrawn at the model's resolution.
- **Denoise.** 1 replaces what is under the mask. Lower keeps its shapes and colors. Below 1, put
  a Fill Masked (blur) before VAE Encode when the old content should not show through.
- **Wrong way round.** If the rest of the picture changed and the painted part did not, the mask
  is inverted: use Invert in the editor or an Invert Mask node.

## Models

These work with Fugue's helper:

- **Models made for inpainting**, which take the mask themselves: FLUX.1 Fill dev, SDXL Inpaint,
  SD 1.5 and 2 Inpaint, and Flex.2.
- **Any other picture model**, where the mask holds the rest of the picture in place at every
  step: FLUX.2 Klein 4B and 9B (the templates use 9B), Qwen-Image Edit, FLUX.1 Kontext, FLUX.1
  dev, Z-Image, SDXL and SD 1.5.

Video models take no mask. Generate Image takes a mask only from a provider that supports one,
and otherwise says so.

## Not yet

Automatic masks (remove the background, pick an object by clicking it, or describe it in words)
will come as optional packs. Soft masks inside the model (differential diffusion), color
matching for outpaints, and a paint canvas with layers are later issues.
