import io
import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

import depth_estimate as de

FIXTURE = Path(__file__).parent / "fixtures" / "tiny-depth.onnx"


def square_image(path: Path, size=(100, 80)) -> Path:
    """Black picture with a white square in the middle."""
    image = Image.new("RGB", size, (0, 0, 0))
    w, h = size
    image.paste((255, 255, 255), (w // 4, h // 4, 3 * w // 4, 3 * h // 4))
    image.save(path)
    return path


def test_model_size_keeps_the_aspect_ratio_on_a_multiple_of_14():
    assert de.model_size(100, 100, 28) == (28, 28)
    height, width = de.model_size(1000, 500, 832)
    assert height % 14 == 0 and width % 14 == 0
    assert (height, width) == (840, 1666)  # 832 is not a multiple of 14: 826 is under it, so up to 840
    assert de.model_size(500, 1000, 832) == (width, height)


def test_model_size_never_goes_under_the_resolution():
    for w, h in [(101, 77), (640, 480), (999, 1000)]:
        height, width = de.model_size(w, h, 518)
        assert min(height, width) >= 518 and height % 14 == 0 and width % 14 == 0


def test_postprocess_is_stretched_gray_rgb_near_light_at_the_picture_size():
    depth = np.zeros((1, 4, 4), np.float32)
    depth[0, :, :2] = 7.0  # left half near (the model's large values), right half far
    depth[0, :, 2:] = 3.0
    picture = de.postprocess(depth, (8, 6))
    assert picture.size == (8, 6) and picture.mode == "RGB"
    a = np.asarray(picture)
    assert a[3, 0, 0] == 255 and a[3, 7, 0] == 0
    assert (a[..., 0] == a[..., 1]).all() and (a[..., 0] == a[..., 2]).all()


def test_postprocess_of_a_flat_scene_is_black_not_an_error():
    picture = de.postprocess(np.full((1, 4, 4), 5.0, np.float32), (4, 4))
    assert np.asarray(picture).max() == 0


def test_preprocess_half_precision_model():
    assert de.preprocess(Image.new("RGB", (4, 4)), (14, 14), np.float16).dtype == np.float16


def test_choose_providers():
    both = ["CUDAExecutionProvider", "CPUExecutionProvider"]
    assert de.choose_providers("auto", both) == ["CUDAExecutionProvider", "CPUExecutionProvider"]
    assert de.choose_providers("cpu", both) == ["CPUExecutionProvider"]
    with pytest.raises(de.NodeError) as error:
        de.choose_providers("cuda", ["CPUExecutionProvider"])
    assert error.value.code == "gpu_unavailable"


def run_request(tmp_path, model, **inputs):
    source = square_image(tmp_path / "in.png")
    work = tmp_path / "work"
    work.mkdir()
    request = {"protocol": 1, "op": "run", "id": "r1", "work_dir": str(work),
               "inputs": {"image": {"type": "image", "path": str(source), "width": 100, "height": 80},
                          "resolution": 28, "device": "cpu", **inputs}}
    if model is not None:
        request["inputs"]["model"] = {"resource": "x/depth-anything-model", "path": str(model)}
    return request, work


def test_run_gives_an_rgb_depth_picture_of_the_input_size(tmp_path):
    request, work = run_request(tmp_path, FIXTURE)
    outputs = de.Runner().run(request, lambda *a: None)
    picture = Image.open(work / outputs["depth"]["path"])
    assert picture.mode == "RGB" and picture.size == (100, 80)
    a = np.asarray(picture)
    assert a[40, 50, 0] > 200 and a[2, 2, 0] < 60  # the white square is near


def test_the_session_is_kept_between_runs(tmp_path):
    runner = de.Runner()
    request, _ = run_request(tmp_path, FIXTURE)
    runner.run(request, lambda *a: None)
    first = runner.session
    runner.run(request, lambda *a: None)
    assert runner.session is first


def test_a_missing_model_is_model_missing(tmp_path, monkeypatch):
    monkeypatch.delenv("FUGUE_DEPTH_MODEL", raising=False)
    request, _ = run_request(tmp_path, None)
    with pytest.raises(de.NodeError) as error:
        de.Runner().run(request, lambda *a: None)
    assert error.value.code == "model_missing"


def test_the_environment_names_a_model_when_the_input_is_unset(tmp_path, monkeypatch):
    monkeypatch.setenv("FUGUE_DEPTH_MODEL", str(FIXTURE))
    request, work = run_request(tmp_path, None)
    outputs = de.Runner().run(request, lambda *a: None)
    assert (work / outputs["depth"]["path"]).exists()


def test_serve_one_shot_writes_json_lines(tmp_path):
    request, _ = run_request(tmp_path, FIXTURE)
    out = io.StringIO()
    code = de.serve(io.StringIO(json.dumps(request)), out, persistent=False)
    lines = [json.loads(line) for line in out.getvalue().splitlines()]
    assert code == 0 and lines[-1]["op"] == "result" and "depth" in lines[-1]["outputs"]


def test_serve_reports_a_node_error(tmp_path):
    request, _ = run_request(tmp_path, tmp_path / "gone.onnx")
    out = io.StringIO()
    code = de.serve(io.StringIO(json.dumps(request)), out, persistent=False)
    last = json.loads(out.getvalue().splitlines()[-1])
    assert code == 1 and last["op"] == "error" and last["code"] == "model_missing"
