# Remove Object (Fugue pack)

Two things. The node **LaMa Fill** takes a picture and a mask (white is what to take away) and gives the picture
with the masked part filled from its surroundings; only the masked pixels change. The group **Remove Object**
(`remove-object/`: Grow Mask, LaMa Fill) grows the mask a few pixels first so no rim of the object is left. It is the
**pre-fill of the Remove action**: the edit model then polishes a clean base instead of being asked to invent one
(gap G5 in [inpaint-outpaint-research.md](../../../docs/inpaint-outpaint-research.md#7-gaps-in-fugue-proposed-issues-not-filed)).
LaMa through ONNX Runtime, in this pack's own Python environment. User guide:
[docs/masking.md](../../../docs/masking.md#remove-an-object).

## Licences (checked 2026-11-22)

| What | Licence | Where |
| --- | --- | --- |
| This pack | MIT | `pack.json` |
| LaMa code and the big-lama weights | Apache-2.0 | https://github.com/advimman/lama/blob/main/LICENSE |
| The ONNX export (`lama_fp32.onnx`, Carve, commit `c3c0c9e4`) | Apache-2.0 | model card `Carve/LaMa-ONNX`; `catalog/lama.json` |
| ONNX Runtime | MIT | https://github.com/microsoft/onnxruntime |
| NumPy, Pillow | BSD-3-Clause, MIT-CMU | |

big-lama was trained on Places; the model cards do not speak for that dataset's terms (the same caveat as BiRefNet's
datasets). Not MAT (Places512 weights: "research purposes only"; docs only, never a default).
Nothing is bundled: the weights are downloaded from the model picker after a question that names the licence.
The download is pinned to a commit and checked against `sha256` `1faef530...68d6` (208,044,816 bytes).

## Why this build

`Carve/LaMa-ONNX` `lama_fp32.onnx`: Apache-2.0 on the card, the export of the original big-lama with an exportable
FourierUnit (the card says the result is identical to the original), opset 17, the author recommends it over the
dynamo export (`lama.onnx`, slower). Its input is fixed at 512 x 512, so LaMa Fill fits the picture into that square
without stretching (mirrored margins that are never part of the hole), runs, and puts back only the masked pixels at
the picture's size; the rest is byte for byte as it was. For a large picture give it a crop around the mask (Canvas
Region does, as the Remove workflow does): a 512 px model sees a 512 px context.

Inputs, as the export defines them: `image` [1, 3, 512, 512] RGB 0..1 (no mean and deviation), `mask` [1, 1, 512, 512]
0/1 where 1 is the hole; output `output` [1, 3, 512, 512] RGB 0..255.

## Develop

```sh
uv sync --extra cpu                 # with the dev group: onnx, pytest
uv run --extra cpu pytest                    # 15 tests, a 290-byte stand-in model, no network
FUGUE_LAMA_MODEL=$PWD/tests/fixtures/tiny-lama.onnx fugue node test .
```

A node test cannot give a model (a resource), so the node also reads `FUGUE_LAMA_MODEL` when its `model` input is empty.
`tests/make_fixture.py` rebuilds the stand-in (it inverts the picture, so the tests see what came from the model).
For the real model, download it from the node's picker (or the URL in `catalog/lama.json`) and point the variable at it.

GPU: `uv sync --no-dev --extra cuda` swaps in `onnxruntime-gpu` (CUDA and cuDNN must be installed); Device `auto` then uses it.

`lama-fill/onnx_node.py` (the command protocol, execution providers) is the same file as in
`io.github.bmaltais.fugue-remove-background` and `io.github.bmaltais.fugue-segment`; change all three together.
With a fourth pack, extract it to one small library each pack depends on.
