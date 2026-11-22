"""Remove Background: BiRefNet through ONNX Runtime, as a Fugue command node.

The model finds the salient subject of a picture and gives a soft mask of it (white is the
subject). The pre- and post-processing are BiRefNet's own (github.com/ZhengPeng7/BiRefNet,
MIT): resize to the model's input size, ImageNet mean and standard deviation, sigmoid on the
output, resize back to the picture's size.

Run by run.sh in the pack's own environment (spec 6.8). One-shot or persistent, see main().
"""
import json
import os
import resource
import sys
from pathlib import Path

import numpy as np
from PIL import Image

MEAN = np.array([0.485, 0.456, 0.406], np.float32).reshape(1, 3, 1, 1)
STD = np.array([0.229, 0.224, 0.225], np.float32).reshape(1, 3, 1, 1)
DEFAULT_SIZE = (1024, 1024)  # BiRefNet's training size, used when the model's input is dynamic


class NodeError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def preprocess(image: Image.Image, size: tuple[int, int], dtype) -> np.ndarray:
    """A picture as the model's input: [1, 3, h, w], normalised. `size` is (height, width)."""
    if image.mode in ("RGBA", "LA", "PA") or "transparency" in image.info:
        rgba = image.convert("RGBA")
        base = Image.new("RGBA", rgba.size, (255, 255, 255, 255))  # a transparent part is white
        image = Image.alpha_composite(base, rgba)
    rgb = image.convert("RGB").resize((size[1], size[0]), Image.BILINEAR)
    array = np.asarray(rgb, np.float32).transpose(2, 0, 1)[None] / 255.0
    return ((array - MEAN) / STD).astype(dtype)


def postprocess(output: np.ndarray, size: tuple[int, int], invert: bool, threshold: float) -> Image.Image:
    """The model's logits as an 8-bit gray mask of `size` (width, height). 0 threshold keeps it soft."""
    logits = np.asarray(output, np.float32).reshape(output.shape[-2:])
    probability = 1.0 / (1.0 + np.exp(-np.clip(logits, -50.0, 50.0)))
    resized = np.asarray(Image.fromarray(probability, "F").resize(size, Image.BILINEAR))
    if threshold > 0:
        resized = (resized >= threshold).astype(np.float32)
    if invert:
        resized = 1.0 - resized
    return Image.fromarray(np.rint(np.clip(resized, 0.0, 1.0) * 255.0).astype(np.uint8), "L")


def choose_providers(device: str, available: list[str]) -> list[str]:
    cuda = "CUDAExecutionProvider"
    if device == "cpu":
        return ["CPUExecutionProvider"]
    if device == "cuda" and cuda not in available:
        raise NodeError("gpu_unavailable", "ONNX Runtime has no CUDA provider here. Install the GPU build "
                        "(uv sync --no-dev --extra cuda in the pack folder) with CUDA and cuDNN, or set Device to CPU or Auto.")
    return [cuda, "CPUExecutionProvider"] if cuda in available else ["CPUExecutionProvider"]


def model_path(inputs: dict) -> Path:
    handle = inputs.get("model")
    path = (handle or {}).get("path") if isinstance(handle, dict) else None
    path = path or os.environ.get("FUGUE_BIREFNET_MODEL")
    if not path or not Path(path).is_file():
        raise NodeError("model_missing", "No BiRefNet model. Pick one in Model (the picker offers the "
                        "downloads of this pack) or put a .onnx file in the birefnet models folder.")
    return Path(path)


class Runner:
    """Keeps the ONNX Runtime session between runs, so a persistent program loads the model once."""

    def __init__(self):
        self.session = None
        self.key = None

    def load(self, path: Path, device: str):
        import onnxruntime as ort

        key = (str(path), path.stat().st_mtime_ns, device)
        if self.session is None or key != self.key:
            self.session = None  # free the old model before the new one is read
            options = ort.SessionOptions()
            options.log_severity_level = 3
            options.enable_cpu_mem_arena = False  # the arena keeps every size it ever grew to: GBs on a 1024 px swin
            options.enable_mem_pattern = False
            providers = choose_providers(device, ort.get_available_providers())
            try:
                self.session = ort.InferenceSession(str(path), options, providers=providers)
            except Exception as error:  # a file that is not an ONNX model, or a provider that cannot start
                raise NodeError("model_invalid", f"{path.name} could not be loaded by ONNX Runtime: {error}") from error
            self.key = key
        return self.session

    def run(self, request: dict, say) -> dict:
        inputs = request["inputs"]
        work = Path(request["work_dir"])
        say(0.05, "loading the model")
        session = self.load(model_path(inputs), inputs.get("device", "auto"))
        meta = session.get_inputs()[0]
        shape = meta.shape[2:4]
        size = tuple(shape) if all(isinstance(n, int) for n in shape) else DEFAULT_SIZE
        dtype = np.float16 if meta.type == "tensor(float16)" else np.float32
        say(0.3, "reading the picture")
        image = Image.open(inputs["image"]["path"])
        image.load()
        tensor = preprocess(image, size, dtype)
        say(0.4, "finding the subject")
        output = session.run(None, {meta.name: tensor})[0]
        say(0.9, "making the mask")
        mask = postprocess(output, image.size, bool(inputs.get("invert")), float(inputs.get("threshold", 0.0)))
        work.mkdir(parents=True, exist_ok=True)
        mask.save(work / "mask.png")
        return {"mask": {"path": "mask.png"},
                "_stats": {"peak_memory_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024}}


def serve(stdin, stdout, persistent: bool) -> int:
    """The command protocol (spec 7). Returns the exit code."""
    runner = Runner()

    def send(message: dict):
        stdout.write(json.dumps(message) + "\n")
        stdout.flush()

    def handle(request: dict) -> bool:
        ident = {"id": request["id"]} if persistent and "id" in request else {}
        try:
            outputs = runner.run(request, lambda v, m: send({"op": "progress", "value": v, "message": m, **ident}))
            stats = outputs.pop("_stats")
            send({"op": "result", "outputs": outputs, "stats": stats, **ident})
            return True
        except NodeError as error:
            send({"op": "error", "code": error.code, "message": str(error), **ident})
        except MemoryError:
            send({"op": "error", "code": "out_of_memory", "message": "Out of memory.", **ident})
        except Exception as error:  # an unreadable picture, a model of another shape: say what it was
            send({"op": "error", "code": "node_failed", "message": f"{type(error).__name__}: {error}", **ident})
        return False

    if not persistent:
        return 0 if handle(json.loads(stdin.read())) else 1
    send({"op": "ready"})
    for line in stdin:
        if not line.strip():
            continue
        message = json.loads(line)
        if message.get("op") == "shutdown":
            return 0
        if message.get("op") == "run":
            handle(message)
        # "cancel": a run is not interruptible, and it answers when it ends or Fugue kills it
    return 0


def main() -> int:
    node = json.load(open(Path(os.environ["FUGUE_NODE_DIR"]) / "node.json"))
    persistent = node["runtime"].get("mode") == "persistent"
    return serve(sys.stdin, sys.stdout, persistent)


if __name__ == "__main__":
    sys.exit(main())
