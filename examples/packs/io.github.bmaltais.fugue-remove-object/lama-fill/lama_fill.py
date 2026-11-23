"""LaMa Fill: LaMa (big-lama) through ONNX Runtime, as a Fugue command node.

The model fills the masked part of a picture from what surrounds it. Its pre- and post-processing are
LaMa's own (github.com/advimman/lama, Apache-2.0) as the ONNX export does it (huggingface.co/Carve/LaMa-ONNX,
Apache-2.0): RGB 0..1, a 0/1 mask where 1 is the hole, a fixed 512 x 512 input, RGB 0..255 out. The picture
is fitted into the square without stretching (mirrored margins), and only the masked pixels of the result are
taken, so everything else stays exactly as it was.

Run by run.sh in the pack's own environment (spec 6.8). One-shot or persistent, see main().
"""
import resource
import sys
from pathlib import Path

import numpy as np
from PIL import Image

import onnx_node
from onnx_node import NodeError, choose_providers  # noqa: F401  (the tests use these names)

SIZE = 512  # the export's fixed input; a model with a static other size is read from its shape


def model_path(inputs: dict) -> Path:
    return onnx_node.model_path(inputs, "FUGUE_LAMA_MODEL",
                                "No LaMa model. Pick one in Model (the picker offers the "
                                "download of this pack) or put a .onnx file in the lama models folder.")


def fit_square(array: np.ndarray, side: int) -> tuple[np.ndarray, tuple[int, int, int, int]]:
    """`array` (h, w, ...) mirrored out to a square, and the box (left, top, right, bottom) of the original in it."""
    h, w = array.shape[:2]
    big = max(h, w)
    top, left = (big - h) // 2, (big - w) // 2
    pad = [(top, big - h - top), (left, big - w - left)] + [(0, 0)] * (array.ndim - 2)
    square = np.pad(array, pad, mode="symmetric") if big > min(h, w) else array
    return square, (left, top, left + w, top + h)


def preprocess(image: Image.Image, mask: Image.Image, side: int):
    """Model inputs `image` [1, 3, side, side] and `mask` [1, 1, side, side], and the box of the picture in the square."""
    rgb = np.asarray(image.convert("RGB"), np.uint8)
    hole = np.asarray(mask.convert("L").resize(image.size, Image.BILINEAR), np.uint8) >= 128
    square, box = fit_square(rgb, side)
    hole_square = np.zeros(square.shape[:2], np.uint8)
    hole_square[box[1]:box[3], box[0]:box[2]] = hole  # the mirrored margin is known ground, never a hole
    small = Image.fromarray(square).resize((side, side), Image.BICUBIC)
    small_hole = Image.fromarray(hole_square * 255).resize((side, side), Image.BILINEAR)
    tensor = (np.asarray(small, np.float32).transpose(2, 0, 1) / 255.0)[None]
    flag = (np.asarray(small_hole, np.float32) > 127.0).astype(np.float32)[None, None]
    return tensor, flag, box


def composite(original: Image.Image, mask: Image.Image, output: np.ndarray, box: tuple[int, int, int, int]) -> Image.Image:
    """The model's output taken back to the picture's size and laid over `original` where the mask is."""
    side = output.shape[-1]
    result = np.clip(np.asarray(output, np.float32).reshape(3, side, side).transpose(1, 2, 0), 0.0, 255.0)
    big = max(original.size)  # the square the picture was mirrored out to
    square = Image.fromarray(np.rint(result).astype(np.uint8)).resize((big, big), Image.BICUBIC)
    left, top, right, bottom = box
    filled = np.asarray(square, np.float32)[top:bottom, left:right]
    weight = (np.asarray(mask.convert("L").resize(original.size, Image.BILINEAR), np.float32) / 255.0)[..., None]
    base = np.asarray(original.convert("RGB"), np.float32)
    mixed = Image.fromarray(np.rint(base * (1.0 - weight) + filled * weight).astype(np.uint8), "RGB")
    if "A" in original.getbands():
        mixed.putalpha(original.getchannel("A"))
    return mixed


class Runner:
    """Keeps the ONNX Runtime session between runs, so a persistent program loads the model once."""

    def __init__(self):
        self.session = None
        self.key = None

    def load(self, path: Path, device: str):
        key = (str(path), path.stat().st_mtime_ns, device)
        if self.session is None or key != self.key:
            self.session = None  # free the old model before the new one is read
            self.session = onnx_node.open_session(str(path), device, path.name)
            self.key = key
        return self.session

    def run(self, request: dict, say) -> dict:
        inputs = request["inputs"]
        work = Path(request["work_dir"])
        say(0.05, "loading the model")
        session = self.load(model_path(inputs), inputs.get("device", "auto"))
        names = [meta.name for meta in session.get_inputs()]
        shape = session.get_inputs()[0].shape[2:4]
        side = shape[0] if all(isinstance(n, int) for n in shape) and shape[0] == shape[1] else SIZE
        say(0.3, "reading the picture")
        image = Image.open(inputs["image"]["path"])
        image.load()
        mask = Image.open(inputs["mask"]["path"])
        mask.load()
        if not np.asarray(mask.convert("L")).any():
            raise NodeError("empty_mask", "The mask is empty: paint what to remove in white.")
        tensor, flag, box = preprocess(image, mask, side)
        say(0.4, "filling")
        output = session.run(None, {names[0]: tensor, names[1]: flag})[0]
        say(0.9, "putting the fill in")
        result = composite(image, mask, output, box)
        work.mkdir(parents=True, exist_ok=True)
        result.save(work / "image.png")
        return {"image": {"path": "image.png"},
                "_stats": {"peak_memory_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024}}


def serve(stdin, stdout, persistent: bool) -> int:
    return onnx_node.serve(stdin, stdout, persistent, Runner())


def main() -> int:
    return serve(sys.stdin, sys.stdout, onnx_node.node_is_persistent())


if __name__ == "__main__":
    sys.exit(main())
