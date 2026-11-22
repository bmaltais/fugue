# Segment by Points or Box (Fugue pack)

One node, **Segment by Points or Box**: gives one object of a picture as a mask (`role: mask`, white is the object).
You say which object with points to include, points to leave out and/or a box, in the picture's pixels. SAM 2.1 through
ONNX Runtime, in this pack's own Python environment. User guide:
[docs/masking.md](../../../docs/masking.md#select-an-object-by-points-or-a-box).

## Licences (checked 2026-11-22)

| What | Licence | Where |
| --- | --- | --- |
| This pack | MIT | `pack.json` |
| SAM 2.1 code and checkpoints (Meta) | Apache-2.0 | https://github.com/facebookresearch/sam2/blob/main/LICENSE ("The SAM 2 model checkpoints, SAM 2 demo code and SAM 2 training code are licensed under Apache 2.0"); model cards `facebook/sam2.1-hiera-*` |
| The ONNX files (an export of those checkpoints by Viet-Anh Nguyen, one zip per size) | Apache-2.0, "license unchanged from upstream" | https://huggingface.co/vietanhdev/segment-anything-2.1-onnx-models; `catalog/sam2.json` |
| ONNX Runtime | MIT | https://github.com/microsoft/onnxruntime |
| NumPy, Pillow | BSD-3-Clause, MIT-CMU | |

Apache-2.0 asks for the licence and a notice with a copy. The model cards do not speak for the training data (SA-V and
SA-1B, whose own terms were not read): not verified here, so the catalog says "Apache-2.0" and links the licence.
The export is a third party's, not Meta's: the weights are the checkpoints' (checked by running them, below), the
graph is the exporter's. Nothing is bundled: the weights are downloaded from the model picker after a question that names
the licence. MobileSAM (Apache-2.0 code, Acly's ONNX files) is not offered: see the spec's decisions.

## The model

Two bundles in `catalog/sam2.json`, each one `.zip` with `*.encoder.onnx` and `*.decoder.onnx` (the zip is the file
Fugue downloads and checks by sha256; the node reads the two parts in memory and never unpacks it): **tiny** (117 MB)
and **base+** (272 MB). `small` and `large` of the same repository work if their zip is put in the `sam2` models folder
(same layout). The encoder takes `image` `[1, 3, 1024, 1024]` (ImageNet mean and deviation, the picture squashed to
the square) and gives `image_embed`, `high_res_feats_0` and `high_res_feats_1`; the decoder takes those and
`point_coords` (the picture's pixels scaled to 1024), `point_labels` (1 include, 0 leave out, 2 and 3 the box corners)
and gives three logit maps and their predicted IoUs; the node takes the best-scored map, scales its logits up to the
picture and cuts at 0. A persistent run keeps the embedding of the last picture, so changing only the prompts costs the
decoder (0.05 s) and not the encoder.

Measured here (12 threads, CPU, a 1200 x 900 picture): tiny 2.6 s for the first run (loading the model, the encoder
1 s), base+ 4.9 s (encoder 2.5 s), then 0.05 s for each change of prompts; peak memory 1.1 GB and 2.0 GB.

## Develop

```sh
uv sync --extra cpu                 # with the dev group: onnx, pytest
uv run --extra cpu pytest           # 26 tests, a 1.7 KB stand-in model, no network
FUGUE_SAM_MODEL=$PWD/tests/fixtures/tiny-sam.zip fugue node test .
# the real model: any bundle, from the node's picker or the repository above
FUGUE_SAM_REAL_MODEL=/path/to/sam2.1_hiera_tiny_20260221.zip uv run --extra cpu pytest -k real
```

A node test cannot give a model (a resource), so the node also reads `FUGUE_SAM_MODEL` when its `model` input is empty.
`tests/make_fixture.py` rebuilds the stand-in (its decoder gives the box of the box corners and Include points).

GPU: `uv sync --no-dev --extra cuda` swaps in `onnxruntime-gpu` (CUDA and cuDNN must be installed); Device `auto` then uses it
(not tried on a GPU here). The node does not declare `needs.gpu`, so Fugue does not schedule its GPU memory against the helper's.

`segment/onnx_node.py` (the command protocol, execution providers, the picture as a tensor) is the same file as in
`io.github.bmaltais.fugue-remove-background`; change both together. A third ONNX pack is the time to make it a library.
