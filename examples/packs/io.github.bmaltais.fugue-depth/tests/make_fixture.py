"""Builds tests/fixtures/tiny-depth.onnx: a stand-in with Depth Anything's interface.

Input `pixel_values` [1, 3, h, w] float32 (normalised RGB, any h and w), output `predicted_depth` [1, h, w]:
the mean of the three channels, so a bright region comes out as near.
The real model is 99 MB, so the tests and `fugue node test` use this one.
Run: .venv/bin/python tests/make_fixture.py
"""
import sys
from pathlib import Path

import onnx
from onnx import TensorProto, helper


def build(path: Path) -> None:
    x = helper.make_tensor_value_info("pixel_values", TensorProto.FLOAT, [1, 3, "height", "width"])
    y = helper.make_tensor_value_info("predicted_depth", TensorProto.FLOAT, [1, "height", "width"])
    mean = helper.make_node("ReduceMean", ["pixel_values"], ["predicted_depth"], axes=[1], keepdims=0)
    graph = helper.make_graph([mean], "tiny_depth", [x], [y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, path)


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "fixtures" / "tiny-depth.onnx"
    build(out)
    print(out)
