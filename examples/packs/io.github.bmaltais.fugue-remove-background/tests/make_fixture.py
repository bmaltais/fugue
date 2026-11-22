"""Builds tests/fixtures/tiny-birefnet.onnx: a stand-in with BiRefNet's interface.

Input `input_image` [1, 3, 64, 64] float32 (normalised RGB), output `output_image` [1, 1, 64, 64]
logits that are the mean of the three channels, so a bright region comes out as the subject.
The real model is 224 MB to 1 GB, so the tests and `fugue node test` use this one.
Run: .venv/bin/python tests/make_fixture.py
"""
import sys
from pathlib import Path

import onnx
from onnx import TensorProto, helper


def build(path: Path) -> None:
    x = helper.make_tensor_value_info("input_image", TensorProto.FLOAT, [1, 3, 64, 64])
    y = helper.make_tensor_value_info("output_image", TensorProto.FLOAT, [1, 1, 64, 64])
    mean = helper.make_node("ReduceMean", ["input_image"], ["output_image"], axes=[1], keepdims=1)
    graph = helper.make_graph([mean], "tiny_birefnet", [x], [y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, path)


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "fixtures" / "tiny-birefnet.onnx"
    build(out)
    print(out)
