"""Builds tests/fixtures/tiny-lama.onnx: a stand-in with LaMa's interface.

Inputs `image` [1, 3, 64, 64] float32 (RGB 0..1) and `mask` [1, 1, 64, 64] (1 is the hole); output `output`
[1, 3, 64, 64] in 0..255: the picture inverted. So a test sees which pixels the node took from the model
(the masked ones, inverted) and which it left alone. The real model is 208 MB (512 x 512), so the tests and
`fugue node test` use this one. Run: .venv/bin/python tests/make_fixture.py
"""
import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def build(path: Path) -> None:
    image = helper.make_tensor_value_info("image", TensorProto.FLOAT, [1, 3, 64, 64])
    mask = helper.make_tensor_value_info("mask", TensorProto.FLOAT, [1, 1, 64, 64])
    out = helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 3, 64, 64])
    one = numpy_helper.from_array(np.array(1.0, np.float32), "one")
    scale = numpy_helper.from_array(np.array(255.0, np.float32), "scale")
    nodes = [helper.make_node("Sub", ["one", "image"], ["inverted"]),
             helper.make_node("Mul", ["inverted", "scale"], ["scaled"]),
             # reads the mask, so the model has both inputs: adds 0 * mask
             helper.make_node("Mul", ["mask", "zero"], ["nothing"]),
             helper.make_node("Add", ["scaled", "nothing"], ["output"])]
    zero = numpy_helper.from_array(np.array(0.0, np.float32), "zero")
    graph = helper.make_graph(nodes, "tiny_lama", [image, mask], [out], initializer=[one, scale, zero])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, path)


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "fixtures" / "tiny-lama.onnx"
    build(out)
    print(out)
