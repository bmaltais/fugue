import io
import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

import lama_fill as lf

FIXTURE = Path(__file__).parent / "fixtures" / "tiny-lama.onnx"
GRAY = (100, 100, 100)


def pictures(tmp_path: Path, size=(100, 80)):
    """A gray picture and a mask whose white square sits in its middle."""
    w, h = size
    Image.new("RGB", size, GRAY).save(tmp_path / "in.png")
    mask = Image.new("L", size, 0)
    mask.paste(255, (w // 3, h // 3, 2 * w // 3, 2 * h // 3))
    mask.save(tmp_path / "mask.png")
    return tmp_path / "in.png", tmp_path / "mask.png"


def request(tmp_path, model=FIXTURE, size=(100, 80), **inputs):
    source, mask = pictures(tmp_path, size)
    work = tmp_path / "work"
    work.mkdir()
    req = {"protocol": 1, "op": "run", "id": "r1", "work_dir": str(work),
           "inputs": {"image": {"type": "image", "path": str(source)}, "mask": {"type": "image", "path": str(mask)},
                      "device": "cpu", **inputs}}
    if model is not None:
        req["inputs"]["model"] = {"resource": "x/lama-model", "path": str(model)}
    return req, work


def test_fit_square_mirrors_the_short_side_and_says_where_the_picture_is():
    array = np.arange(2 * 4).reshape(2, 4)
    square, box = lf.fit_square(array, 4)
    assert square.shape == (4, 4) and box == (0, 1, 4, 3)
    assert (square[1:3] == array).all()
    assert (square[0] == array[0]).all() and (square[3] == array[1]).all()  # symmetric margins


def test_fit_square_far_beyond_the_short_side():
    square, box = lf.fit_square(np.zeros((2, 40), np.uint8), 40)
    assert square.shape == (40, 40) and box == (0, 19, 40, 21)


def test_preprocess_gives_the_models_inputs_and_keeps_the_margin_out_of_the_hole(tmp_path):
    source, mask = pictures(tmp_path, (100, 40))
    tensor, flag, box = lf.preprocess(Image.open(source), Image.open(mask), 64)
    assert tensor.shape == (1, 3, 64, 64) and flag.shape == (1, 1, 64, 64)
    assert tensor.dtype == np.float32 and flag.dtype == np.float32
    assert tensor.max() == pytest.approx(100 / 255, abs=0.01)  # 0..1, not normalised
    assert set(np.unique(flag)) <= {0.0, 1.0} and flag.max() == 1.0
    assert flag[0, 0, :16].max() == 0.0 and flag[0, 0, 48:].max() == 0.0  # the top and bottom margins are known ground
    assert box == (0, 30, 100, 70)


def test_run_changes_only_the_masked_pixels(tmp_path):
    req, work = request(tmp_path)
    outputs = lf.Runner().run(req, lambda *a: None)
    a = np.asarray(Image.open(work / outputs["image"]["path"]))
    assert a.shape == (80, 100, 3)
    assert tuple(a[40, 50]) == (155, 155, 155)  # the model's inverted gray, inside the mask
    assert tuple(a[2, 2]) == GRAY and tuple(a[75, 95]) == GRAY  # outside it, as it was
    assert (a[:20] == GRAY).all()


def test_a_soft_mask_edge_mixes_the_fill_in_by_its_weight(tmp_path):
    req, work = request(tmp_path)
    mask = Image.new("L", (100, 80), 0)
    mask.paste(255, (30, 20, 70, 60))
    mask.paste(128, (70, 20, 80, 60))  # half a hole: not sent to the model, mixed by half
    mask.save(tmp_path / "mask.png")
    outputs = lf.Runner().run(req, lambda *a: None)
    a = np.asarray(Image.open(work / outputs["image"]["path"])).astype(int)
    assert a[40, 50, 0] == 155
    assert a[40, 75, 0] in range(125, 131)  # (100 + 155) / 2 where the model gave 155


def test_alpha_is_kept(tmp_path):
    req, work = request(tmp_path)
    Image.new("RGBA", (100, 80), (100, 100, 100, 77)).save(tmp_path / "in.png")
    outputs = lf.Runner().run(req, lambda *a: None)
    result = Image.open(work / outputs["image"]["path"])
    assert result.mode == "RGBA" and result.getchannel("A").getextrema() == (77, 77)


def test_a_picture_of_another_shape_comes_back_at_its_own_size(tmp_path):
    req, work = request(tmp_path, size=(30, 200))
    outputs = lf.Runner().run(req, lambda *a: None)
    assert Image.open(work / outputs["image"]["path"]).size == (30, 200)


def test_an_empty_mask_is_an_error(tmp_path):
    req, _ = request(tmp_path)
    Image.new("L", (100, 80), 0).save(tmp_path / "mask.png")
    with pytest.raises(lf.NodeError) as error:
        lf.Runner().run(req, lambda *a: None)
    assert error.value.code == "empty_mask"


def test_choose_providers():
    both = ["CUDAExecutionProvider", "CPUExecutionProvider"]
    assert lf.choose_providers("auto", both) == ["CUDAExecutionProvider", "CPUExecutionProvider"]
    assert lf.choose_providers("cpu", both) == ["CPUExecutionProvider"]
    with pytest.raises(lf.NodeError) as error:
        lf.choose_providers("cuda", ["CPUExecutionProvider"])
    assert error.value.code == "gpu_unavailable"


def test_the_session_is_kept_between_runs(tmp_path):
    runner = lf.Runner()
    req, _ = request(tmp_path)
    runner.run(req, lambda *a: None)
    first = runner.session
    runner.run(req, lambda *a: None)
    assert runner.session is first


def test_a_missing_model_is_model_missing(tmp_path, monkeypatch):
    monkeypatch.delenv("FUGUE_LAMA_MODEL", raising=False)
    req, _ = request(tmp_path, model=None)
    with pytest.raises(lf.NodeError) as error:
        lf.Runner().run(req, lambda *a: None)
    assert error.value.code == "model_missing"


def test_the_environment_names_a_model_when_the_input_is_unset(tmp_path, monkeypatch):
    monkeypatch.setenv("FUGUE_LAMA_MODEL", str(FIXTURE))
    req, work = request(tmp_path, model=None)
    outputs = lf.Runner().run(req, lambda *a: None)
    assert (work / outputs["image"]["path"]).exists()


def test_serve_one_shot_writes_json_lines(tmp_path):
    req, _ = request(tmp_path)
    out = io.StringIO()
    code = lf.serve(io.StringIO(json.dumps(req)), out, persistent=False)
    lines = [json.loads(line) for line in out.getvalue().splitlines()]
    assert code == 0 and lines[-1]["op"] == "result" and "image" in lines[-1]["outputs"]
    assert any(line["op"] == "progress" for line in lines[:-1])


def test_serve_reports_a_node_error(tmp_path):
    req, _ = request(tmp_path, model=tmp_path / "gone.onnx")
    out = io.StringIO()
    code = lf.serve(io.StringIO(json.dumps(req)), out, persistent=False)
    last = json.loads(out.getvalue().splitlines()[-1])
    assert code == 1 and last["op"] == "error" and last["code"] == "model_missing"


def test_serve_persistent_answers_each_run_with_its_id(tmp_path):
    first, _ = request(tmp_path)
    first["id"] = "a"
    lines = "\n".join([json.dumps(first), json.dumps(dict(first, id="b")), json.dumps({"op": "shutdown"})])
    out = io.StringIO()
    code = lf.serve(io.StringIO(lines), out, persistent=True)
    messages = [json.loads(line) for line in out.getvalue().splitlines()]
    assert code == 0 and messages[0] == {"op": "ready"}
    assert [m["id"] for m in messages if m["op"] == "result"] == ["a", "b"]
