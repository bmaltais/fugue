import io
import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

import remove_background as rb

FIXTURE = Path(__file__).parent / "fixtures" / "tiny-birefnet.onnx"


def square_image(path: Path, size=(100, 80)) -> Path:
    """Black picture with a white square in the middle."""
    image = Image.new("RGB", size, (0, 0, 0))
    w, h = size
    image.paste((255, 255, 255), (w // 4, h // 4, 3 * w // 4, 3 * h // 4))
    image.save(path)
    return path


def test_preprocess_resizes_normalises_and_orders_channels(tmp_path):
    image = Image.new("RGB", (10, 20), (255, 0, 0))
    tensor = rb.preprocess(image, (64, 64), np.float32)
    assert tensor.shape == (1, 3, 64, 64) and tensor.dtype == np.float32
    assert tensor[0, 0, 0, 0] == pytest.approx((1.0 - 0.485) / 0.229)
    assert tensor[0, 1, 0, 0] == pytest.approx((0.0 - 0.456) / 0.224)
    assert tensor[0, 2, 0, 0] == pytest.approx((0.0 - 0.406) / 0.225)


def test_preprocess_drops_alpha_over_white():
    image = Image.new("RGBA", (4, 4), (0, 0, 0, 0))
    tensor = rb.preprocess(image, (8, 8), np.float32)
    assert tensor[0, 0, 0, 0] == pytest.approx((1.0 - 0.485) / 0.229)


def test_preprocess_half_precision_model():
    tensor = rb.preprocess(Image.new("RGB", (4, 4)), (8, 8), np.float16)
    assert tensor.dtype == np.float16


def test_postprocess_is_sigmoid_scaled_to_the_original_size():
    logits = np.zeros((1, 1, 8, 8), np.float32)
    logits[0, 0, :, :4] = 20.0   # left half subject
    logits[0, 0, :, 4:] = -20.0
    mask = rb.postprocess(logits, (16, 10), invert=False, threshold=0.0)
    assert mask.size == (16, 10) and mask.mode == "L"
    a = np.asarray(mask)
    assert a[5, 0] == 255 and a[5, 15] == 0


def test_postprocess_invert_and_threshold():
    logits = np.full((1, 1, 4, 4), 0.0, np.float32)  # sigmoid = 0.5
    soft = np.asarray(rb.postprocess(logits, (4, 4), invert=False, threshold=0.0))
    assert abs(int(soft[0, 0]) - 128) <= 1
    hard = np.asarray(rb.postprocess(logits, (4, 4), invert=False, threshold=0.4))
    assert set(np.unique(hard)) == {255}
    flipped = np.asarray(rb.postprocess(logits, (4, 4), invert=True, threshold=0.6))
    assert set(np.unique(flipped)) == {255}  # 0.5 < 0.6 is background, inverted is subject


def test_choose_providers():
    both = ["CUDAExecutionProvider", "CPUExecutionProvider"]
    assert rb.choose_providers("auto", both) == ["CUDAExecutionProvider", "CPUExecutionProvider"]
    assert rb.choose_providers("auto", ["CPUExecutionProvider"]) == ["CPUExecutionProvider"]
    assert rb.choose_providers("cpu", both) == ["CPUExecutionProvider"]
    with pytest.raises(rb.NodeError) as error:
        rb.choose_providers("cuda", ["CPUExecutionProvider"])
    assert error.value.code == "gpu_unavailable"


def run_request(tmp_path, model, **inputs):
    source = square_image(tmp_path / "in.png")
    work = tmp_path / "work"
    work.mkdir()
    request = {"protocol": 1, "op": "run", "id": "r1", "work_dir": str(work),
               "inputs": {"image": {"type": "image", "path": str(source), "width": 100, "height": 80},
                          "invert": False, "threshold": 0.0, "device": "cpu", **inputs}}
    if model is not None:
        request["inputs"]["model"] = {"resource": "x/birefnet-model", "path": str(model)}
    return request, work


def test_run_gives_a_gray_png_mask_of_the_input_size(tmp_path):
    request, work = run_request(tmp_path, FIXTURE)
    outputs = rb.Runner().run(request, lambda *a: None)
    path = work / outputs["mask"]["path"]
    mask = Image.open(path)
    assert mask.mode == "L" and mask.size == (100, 80)
    a = np.asarray(mask)
    assert a[40, 50] > 200 and a[2, 2] < 60  # the white square is the subject


def test_invert_flips_the_mask(tmp_path):
    request, work = run_request(tmp_path, FIXTURE, invert=True)
    outputs = rb.Runner().run(request, lambda *a: None)
    a = np.asarray(Image.open(work / outputs["mask"]["path"]))
    assert a[40, 50] < 60 and a[2, 2] > 200


def test_the_session_is_kept_between_runs(tmp_path):
    runner = rb.Runner()
    request, _ = run_request(tmp_path, FIXTURE)
    runner.run(request, lambda *a: None)
    first = runner.session
    runner.run(request, lambda *a: None)
    assert runner.session is first


def test_a_missing_model_is_model_missing(tmp_path, monkeypatch):
    monkeypatch.delenv("FUGUE_BIREFNET_MODEL", raising=False)
    request, _ = run_request(tmp_path, None)
    with pytest.raises(rb.NodeError) as error:
        rb.Runner().run(request, lambda *a: None)
    assert error.value.code == "model_missing"


def test_the_environment_names_a_model_when_the_input_is_unset(tmp_path, monkeypatch):
    monkeypatch.setenv("FUGUE_BIREFNET_MODEL", str(FIXTURE))
    request, work = run_request(tmp_path, None)
    outputs = rb.Runner().run(request, lambda *a: None)
    assert (work / outputs["mask"]["path"]).exists()


def test_serve_one_shot_writes_json_lines(tmp_path):
    request, work = run_request(tmp_path, FIXTURE)
    out = io.StringIO()
    code = rb.serve(io.StringIO(json.dumps(request)), out, persistent=False)
    lines = [json.loads(line) for line in out.getvalue().splitlines()]
    assert code == 0 and lines[-1]["op"] == "result" and "mask" in lines[-1]["outputs"]
    assert any(line["op"] == "progress" for line in lines[:-1])


def test_serve_reports_a_node_error(tmp_path):
    request, _ = run_request(tmp_path, tmp_path / "gone.onnx")
    out = io.StringIO()
    code = rb.serve(io.StringIO(json.dumps(request)), out, persistent=False)
    last = json.loads(out.getvalue().splitlines()[-1])
    assert code == 1 and last["op"] == "error" and last["code"] == "model_missing"


def test_serve_persistent_answers_each_run_with_its_id(tmp_path):
    first, _ = run_request(tmp_path, FIXTURE)
    first["id"] = "a"
    second = dict(first, id="b")
    lines = "\n".join([json.dumps(first), json.dumps(second), json.dumps({"op": "shutdown"})])
    out = io.StringIO()
    code = rb.serve(io.StringIO(lines), out, persistent=True)
    messages = [json.loads(line) for line in out.getvalue().splitlines()]
    assert code == 0 and messages[0] == {"op": "ready"}
    results = [m for m in messages if m["op"] == "result"]
    assert [m["id"] for m in results] == ["a", "b"]
