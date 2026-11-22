"""Builds a stand-in for GroundingDINO's ONNX interface, for the tests and `fugue node test`.

Inputs as the real model (`pixel_values` [1, 3, 32, 32], `input_ids`, `token_type_ids`, `attention_mask` [1, n],
`pixel_mask` [1, 32, 32]); outputs `logits` [1, 2, 256] and `pred_boxes` [1, 2, 4], constants: box 1 (centre .25 .25,
size .5 .5) says 0.9 on the first query token; box 2 (centre .75 .75, size .5 .5) says 0.2. The real model is 718 MB.
Run: .venv/bin/python tests/make_dino_fixture.py
"""
import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

SIDE = 32


def build(path: Path) -> None:
    i64, f = TensorProto.INT64, TensorProto.FLOAT
    inputs = [helper.make_tensor_value_info("pixel_values", f, [1, 3, SIDE, SIDE]),
              helper.make_tensor_value_info("input_ids", i64, [1, "sequence_length"]),
              helper.make_tensor_value_info("token_type_ids", i64, [1, "sequence_length"]),
              helper.make_tensor_value_info("attention_mask", i64, [1, "sequence_length"]),
              helper.make_tensor_value_info("pixel_mask", i64, [1, SIDE, SIDE])]
    outputs = [helper.make_tensor_value_info("logits", f, [1, 2, 256]),
               helper.make_tensor_value_info("pred_boxes", f, [1, 2, 4])]
    logits = np.full((1, 2, 256), -10.0, np.float32)
    logits[0, 0, 1] = np.log(0.9 / 0.1)
    logits[0, 1, 1] = np.log(0.2 / 0.8)
    boxes = np.array([[[0.25, 0.25, 0.5, 0.5], [0.75, 0.75, 0.5, 0.5]]], np.float32)
    nodes = [helper.make_node("Identity", ["c_logits"], ["logits"]), helper.make_node("Identity", ["c_boxes"], ["pred_boxes"])]
    inits = [numpy_helper.from_array(logits, "c_logits"), numpy_helper.from_array(boxes, "c_boxes")]
    model = helper.make_model(helper.make_graph(nodes, "tiny_dino", inputs, outputs, inits),
                              opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    path.write_bytes(model.SerializeToString())


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "fixtures" / "tiny-dino.onnx"
    build(out)
    print(out)
