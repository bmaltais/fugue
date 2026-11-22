"""Builds tests/fixtures/tiny-sam.zip: a bundle with SAM 2.1's interface and a stand-in inside.

Encoder: input `image` [1, 3, 64, 64], outputs `image_embed`, `high_res_feats_0`, `high_res_feats_1` (the
picture's own values, in the real model's order and names). Decoder: the inputs of the real one; it ignores
the embedding and gives three 64 x 64 logit maps: 1 and 3 are all background, 2 is +10 inside the box
that holds the box corners and Include points (label 1, 2 or 3; Exclude points, label 0, are ignored),
plus half a cell, and -10 outside it. The scores are 0.1, 0.9 and 0.2, so the node must pick the second.
The real model is 116 MB to 272 MB, so the tests and `fugue node test` use this one.
Run: .venv/bin/python tests/make_fixture.py
"""
import sys
import zipfile
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

SIDE = 64  # the stand-in decoder's mask side; a cell is 1024 / 64 = 16 model pixels
OPSET = [helper.make_opsetid("", 17)]


def finish(graph) -> bytes:
    model = helper.make_model(graph, opset_imports=OPSET)
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model.SerializeToString()


def encoder() -> bytes:
    image = helper.make_tensor_value_info("image", TensorProto.FLOAT, [1, 3, SIDE, SIDE])
    outputs = [helper.make_tensor_value_info("high_res_feats_0", TensorProto.FLOAT, [1, 3, SIDE, SIDE]),
               helper.make_tensor_value_info("high_res_feats_1", TensorProto.FLOAT, [1, 3, SIDE, SIDE]),
               helper.make_tensor_value_info("image_embed", TensorProto.FLOAT, [1, 1, SIDE, SIDE])]
    nodes = [helper.make_node("Identity", ["image"], ["high_res_feats_0"]),
             helper.make_node("Neg", ["image"], ["high_res_feats_1"]),
             helper.make_node("ReduceMean", ["image"], ["image_embed"], axes=[1], keepdims=1)]
    return finish(helper.make_graph(nodes, "tiny_sam_encoder", [image], outputs))


def decoder() -> bytes:
    f = TensorProto.FLOAT
    inputs = [helper.make_tensor_value_info("image_embed", f, [1, 1, SIDE, SIDE]),
              helper.make_tensor_value_info("high_res_feats_0", f, [1, 3, SIDE, SIDE]),
              helper.make_tensor_value_info("high_res_feats_1", f, [1, 3, SIDE, SIDE]),
              helper.make_tensor_value_info("point_coords", f, ["num_labels", "num_points", 2]),
              helper.make_tensor_value_info("point_labels", f, ["num_labels", "num_points"]),
              helper.make_tensor_value_info("mask_input", f, ["num_labels", 1, 256, 256]),
              helper.make_tensor_value_info("has_mask_input", f, ["num_labels"])]
    outputs = [helper.make_tensor_value_info("masks", f, [1, 3, SIDE, SIDE]),
               helper.make_tensor_value_info("iou_predictions", f, [1, 3])]
    centres = (np.arange(SIDE, dtype=np.float32) + 0.5) * (1024 / SIDE)
    consts = {"gx": np.tile(centres, (SIDE, 1))[None, None], "gy": np.tile(centres[:, None], (1, SIDE))[None, None],
              "big": np.array(1e9, np.float32), "nbig": np.array(-1e9, np.float32), "half": np.array(0.5, np.float32),
              "margin": np.array(1024 / SIDE / 2, np.float32), "plus": np.full((1, 1, SIDE, SIDE), 10, np.float32),
              "minus": np.full((1, 1, SIDE, SIDE), -10, np.float32), "scores": np.array([[0.1, 0.9, 0.2]], np.float32),
              "shape4": np.array([1, 1, 1, 2], np.int64), "axis_last": np.array([-1], np.int64),
              "zero": np.array([0], np.int64), "one": np.array([1], np.int64)}
    nodes = [
        helper.make_node("Unsqueeze", ["point_labels", "axis_last"], ["labels3"]),
        helper.make_node("Greater", ["labels3", "half"], ["is_included"]),  # [1, n, 1]
        helper.make_node("Where", ["is_included", "point_coords", "big"], ["for_min"]),
        helper.make_node("Where", ["is_included", "point_coords", "nbig"], ["for_max"]),
        helper.make_node("ReduceMin", ["for_min"], ["lo3"], axes=[1], keepdims=1),
        helper.make_node("ReduceMax", ["for_max"], ["hi3"], axes=[1], keepdims=1),
        helper.make_node("Reshape", ["lo3", "shape4"], ["lo"]),
        helper.make_node("Reshape", ["hi3", "shape4"], ["hi"]),
        helper.make_node("Gather", ["lo", "zero"], ["lox"], axis=3), helper.make_node("Gather", ["lo", "one"], ["loy"], axis=3),
        helper.make_node("Gather", ["hi", "zero"], ["hix"], axis=3), helper.make_node("Gather", ["hi", "one"], ["hiy"], axis=3),
        helper.make_node("Sub", ["lox", "margin"], ["x0"]), helper.make_node("Add", ["hix", "margin"], ["x1"]),
        helper.make_node("Sub", ["loy", "margin"], ["y0"]), helper.make_node("Add", ["hiy", "margin"], ["y1"]),
        helper.make_node("GreaterOrEqual", ["gx", "x0"], ["in_x0"]), helper.make_node("LessOrEqual", ["gx", "x1"], ["in_x1"]),
        helper.make_node("GreaterOrEqual", ["gy", "y0"], ["in_y0"]), helper.make_node("LessOrEqual", ["gy", "y1"], ["in_y1"]),
        helper.make_node("And", ["in_x0", "in_x1"], ["in_x"]), helper.make_node("And", ["in_y0", "in_y1"], ["in_y"]),
        helper.make_node("And", ["in_x", "in_y"], ["inside"]),
        helper.make_node("Where", ["inside", "plus", "minus"], ["box_mask"]),
        helper.make_node("Concat", ["minus", "box_mask", "minus"], ["masks"], axis=1),
        helper.make_node("Identity", ["scores"], ["iou_predictions"]),
    ]
    inits = [numpy_helper.from_array(v, k) for k, v in consts.items()]
    return finish(helper.make_graph(nodes, "tiny_sam_decoder", inputs, outputs, inits))


def build(path: Path) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as bundle:
        for name, data in (("tiny.encoder.onnx", encoder()), ("tiny.decoder.onnx", decoder())):
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))  # a fixed date: the same bytes every time
            info.compress_type = zipfile.ZIP_DEFLATED
            bundle.writestr(info, data)


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "fixtures" / "tiny-sam.zip"
    build(out)
    print(out)
