import io
import json
import os
import zipfile
from pathlib import Path

import numpy as np
import pytest
import segment as sg
from PIL import Image

FIXTURE = Path(__file__).parent / "fixtures" / "tiny-sam.zip"
SIZE = (100, 80)  # width, height of the test picture


def square_image(path: Path, size=SIZE) -> Path:
    """Black picture with a white square in the middle."""
    image = Image.new("RGB", size, (0, 0, 0))
    w, h = size
    image.paste((255, 255, 255), (w // 4, h // 4, 3 * w // 4, 3 * h // 4))
    image.save(path)
    return path


# the prompts as the person writes them

def test_points_are_numbers_one_pair_per_line_with_any_separator():
    assert sg.parse_coordinates("10 20\n30,40\n\n 50 ; 60.5 \n# a note\n", 2, "Include points") == \
        [[10, 20], [30, 40], [50, 60.5]]


@pytest.mark.parametrize("text, count", [("10", 2), ("1 2 3", 2), ("1 2 3", 4)])
def test_a_line_with_the_wrong_number_of_numbers_names_its_line(text, count):
    with pytest.raises(sg.NodeError) as error:
        sg.parse_coordinates("1 2\n" + text if count == 2 else text, count, "Points")
    assert error.value.code == "prompt_invalid" and "expected" in str(error.value)


def test_text_that_is_not_numbers_is_refused():
    with pytest.raises(sg.NodeError) as error:
        sg.parse_coordinates("10 left", 2, "Include points")
    assert error.value.code == "prompt_invalid" and "line 1" in str(error.value)


def test_a_box_is_put_in_order_whatever_corner_comes_first():
    prompts = sg.parse_prompts("", "", "90 70, 10 20", SIZE)
    assert prompts["box"] == (10, 20, 90, 70) and prompts["include"] == []


def test_something_to_segment_is_needed():
    for args in (("", "", ""), ("", "5 5", "")):  # nothing, or only a point to leave out
        with pytest.raises(sg.NodeError) as error:
            sg.parse_prompts(*args, SIZE)
        assert error.value.code == "prompt_missing"


def test_a_point_outside_the_picture_says_the_size():
    with pytest.raises(sg.NodeError) as error:
        sg.parse_prompts("101 5", "", "", SIZE)
    assert error.value.code == "prompt_outside" and "100 x 80" in str(error.value)


def test_a_box_with_no_area_or_two_lines_is_refused():
    for box in ("10 10 10 50", "1 1 5 5\n2 2 6 6"):
        with pytest.raises(sg.NodeError) as error:
            sg.parse_prompts("", "", box, SIZE)
        assert error.value.code == "prompt_invalid"


# the decoder's input

def test_decoder_prompts_put_the_box_first_then_include_then_exclude_in_model_space():
    prompts = sg.parse_prompts("50 40", "10 20", "0 0 100 80", SIZE)
    coords, labels = sg.decoder_prompts(prompts, SIZE)
    assert coords.shape == (1, 4, 2) and labels.shape == (1, 4)
    assert labels[0].tolist() == [2, 3, 1, 0]
    np.testing.assert_allclose(coords[0], [[0, 0], [1024, 1024], [512, 512], [102.4, 256]], rtol=1e-5)


def test_decoder_prompts_with_one_point_have_no_padding_point():
    coords, labels = sg.decoder_prompts(sg.parse_prompts("50 40", "", "", SIZE), SIZE)
    assert coords.shape == (1, 1, 2) and labels.tolist() == [[1.0]]


# the mask

def test_to_mask_cuts_the_scaled_up_logits_and_inverts():
    logits = np.full((8, 8), -5.0, np.float32)
    logits[:, :4] = 5.0
    mask = sg.to_mask(logits, (32, 16), invert=False, soft=False)
    a = np.asarray(mask)
    assert mask.size == (32, 16) and mask.mode == "L" and set(np.unique(a)) == {0, 255}
    assert a[8, 2] == 255 and a[8, 30] == 0
    assert np.asarray(sg.to_mask(logits, (32, 16), invert=True, soft=False))[8, 2] == 0


def test_soft_edge_gives_the_sigmoid():
    mask = np.asarray(sg.to_mask(np.zeros((4, 4), np.float32), (4, 4), invert=False, soft=True))
    assert abs(int(mask[0, 0]) - 128) <= 1


# the model bundle

def test_read_bundle_gives_the_two_parts(tmp_path):
    encoder, decoder = sg.read_bundle(FIXTURE)
    assert encoder[:2] and decoder[:2] and encoder != decoder


def test_a_bundle_without_both_parts_or_not_a_zip_is_model_invalid(tmp_path):
    only = tmp_path / "only.zip"
    with zipfile.ZipFile(only, "w") as z:
        z.writestr("a.encoder.onnx", b"x")
    plain = tmp_path / "plain.zip"
    plain.write_bytes(b"not a zip")
    for path in (only, plain):
        with pytest.raises(sg.NodeError) as error:
            sg.read_bundle(path)
        assert error.value.code == "model_invalid"


def run_request(tmp_path, model, **inputs):
    source = square_image(tmp_path / "in.png")
    work = tmp_path / "work"
    work.mkdir(exist_ok=True)
    request = {"protocol": 1, "op": "run", "id": "r1", "work_dir": str(work),
               "inputs": {"image": {"type": "image", "path": str(source), "width": 100, "height": 80},
                          "positive": "", "negative": "", "box": "", "invert": False, "soft": False, "device": "cpu", **inputs}}
    if model is not None:
        request["inputs"]["model"] = {"resource": "x/sam2-model", "path": str(model)}
    return request, work


def mask_of(runner, request, work):
    outputs = runner.run(request, lambda *a: None)
    mask = Image.open(work / outputs["mask"]["path"])
    assert mask.mode == "L" and mask.size == SIZE
    return np.asarray(mask)


def test_a_point_gives_a_mask_around_it_and_the_best_scored_map_is_taken(tmp_path):
    request, work = run_request(tmp_path, FIXTURE, positive="50 40")
    a = mask_of(sg.Runner(), request, work)
    assert a[40, 50] == 255 and a[5, 5] == 0 and a[75, 95] == 0  # maps 1 and 3 are empty: a mask means map 2 was taken


def test_a_box_gives_that_box(tmp_path):
    request, work = run_request(tmp_path, FIXTURE, box="20 10 60 50")
    a = mask_of(sg.Runner(), request, work)
    assert a[30, 40] == 255 and a[30, 90] == 0 and a[70, 40] == 0
    assert 0.9 < (a[10:50, 20:60] == 255).mean()


def test_an_exclude_point_is_not_taken_for_an_include_point(tmp_path):
    request, work = run_request(tmp_path, FIXTURE, positive="50 40", negative="5 5")
    a = mask_of(sg.Runner(), request, work)
    assert a[40, 50] == 255 and a[5, 5] == 0 and a[75, 95] == 0


def test_invert_flips_the_mask(tmp_path):
    request, work = run_request(tmp_path, FIXTURE, positive="50 40", invert=True)
    a = mask_of(sg.Runner(), request, work)
    assert a[40, 50] == 0 and a[5, 5] == 255


def test_the_picture_is_embedded_once_while_only_the_prompts_change(tmp_path):
    runner = sg.Runner()
    request, work = run_request(tmp_path, FIXTURE, positive="50 40")
    runner.run(request, lambda *a: None)
    sessions = runner.encoder, runner.decoder
    request["inputs"]["positive"] = "20 20"
    runner.run(request, lambda *a: None)
    assert runner.encoder_runs == 1 and (runner.encoder, runner.decoder) == sessions
    square_image(tmp_path / "in.png", (100, 81))  # another picture, same file name
    request["inputs"]["image"]["height"] = 81
    runner.run(request, lambda *a: None)
    assert runner.encoder_runs == 2


def test_a_missing_model_is_model_missing(tmp_path, monkeypatch):
    monkeypatch.delenv("FUGUE_SAM_MODEL", raising=False)
    request, _ = run_request(tmp_path, None, positive="5 5")
    with pytest.raises(sg.NodeError) as error:
        sg.Runner().run(request, lambda *a: None)
    assert error.value.code == "model_missing"


def test_the_environment_names_a_model_when_the_input_is_unset(tmp_path, monkeypatch):
    monkeypatch.setenv("FUGUE_SAM_MODEL", str(FIXTURE))
    request, work = run_request(tmp_path, None, positive="50 40")
    outputs = sg.Runner().run(request, lambda *a: None)
    assert (work / outputs["mask"]["path"]).exists()


def test_a_prompt_error_comes_before_the_model_is_read(tmp_path):
    request, _ = run_request(tmp_path, tmp_path / "gone.zip")
    with pytest.raises(sg.NodeError) as error:
        sg.Runner().run(request, lambda *a: None)
    assert error.value.code == "prompt_missing"


def test_serve_one_shot_writes_json_lines(tmp_path):
    request, _ = run_request(tmp_path, FIXTURE, positive="50 40")
    out = io.StringIO()
    code = sg.serve(io.StringIO(json.dumps(request)), out, persistent=False)
    lines = [json.loads(line) for line in out.getvalue().splitlines()]
    assert code == 0 and lines[-1]["op"] == "result" and "mask" in lines[-1]["outputs"]
    assert any(line["op"] == "progress" for line in lines[:-1])


def test_serve_reports_a_node_error(tmp_path):
    request, _ = run_request(tmp_path, FIXTURE)
    out = io.StringIO()
    code = sg.serve(io.StringIO(json.dumps(request)), out, persistent=False)
    last = json.loads(out.getvalue().splitlines()[-1])
    assert code == 1 and last["op"] == "error" and last["code"] == "prompt_missing"


def test_serve_persistent_answers_each_run_with_its_id(tmp_path):
    first, _ = run_request(tmp_path, FIXTURE, positive="50 40")
    first["id"] = "a"
    second = dict(first, id="b")
    lines = "\n".join([json.dumps(first), json.dumps(second), json.dumps({"op": "shutdown"})])
    out = io.StringIO()
    code = sg.serve(io.StringIO(lines), out, persistent=True)
    messages = [json.loads(line) for line in out.getvalue().splitlines()]
    assert code == 0 and messages[0] == {"op": "ready"}
    assert [m["id"] for m in messages if m["op"] == "result"] == ["a", "b"]


@pytest.mark.skipif(not os.environ.get("FUGUE_SAM_REAL_MODEL"), reason="set FUGUE_SAM_REAL_MODEL to a real SAM 2.1 bundle (.zip)")
def test_the_real_model_finds_the_square_from_a_point_and_from_a_box(tmp_path):
    source = tmp_path / "in.png"
    image = Image.new("RGB", (200, 150), (25, 25, 25))
    image.paste((230, 60, 60), (60, 40, 140, 110))
    image.save(source)
    truth = np.zeros((150, 200), bool)
    truth[40:110, 60:140] = True
    runner = sg.Runner()
    for prompt in ({"positive": "100 75"}, {"box": "55 35 145 115"}, {"positive": "100 75", "negative": "20 20"}):
        request = {"work_dir": str(tmp_path / "w"), "inputs": {
            "image": {"path": str(source)}, "model": {"path": os.environ["FUGUE_SAM_REAL_MODEL"]},
            "positive": "", "negative": "", "box": "", "device": "cpu", **prompt}}
        outputs = runner.run(request, lambda *a: None)
        got = np.asarray(Image.open(tmp_path / "w" / outputs["mask"]["path"])) > 127
        iou = (got & truth).sum() / (got | truth).sum()
        assert iou > 0.9, (prompt, iou)
