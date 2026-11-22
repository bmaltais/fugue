import io
import json
import os
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

NODE = Path(__file__).parent.parent / "find-by-text"
sys.path.insert(0, str(NODE))
import find_by_text as fb

FIXTURE = Path(__file__).parent / "fixtures" / "tiny-dino.onnx"
VOCAB = NODE / "vocab.txt"


@pytest.fixture(scope="module")
def tokenizer():
    return fb.WordPiece(VOCAB.read_text(encoding="utf-8").splitlines())


# the query as the person writes it

def test_phrases_are_split_on_periods_commas_semicolons_and_lines_and_cleaned():
    assert fb.parse_query("red balloon. the clown,  \n glove;;  ") == ["red balloon", "the clown", "glove"]


def test_a_query_with_no_words_is_refused():
    for text in ("", "  . , \n"):
        with pytest.raises(fb.NodeError) as error:
            fb.parse_query(text)
        assert error.value.code == "prompt_missing"


# the tokenizer: BERT uncased wordpiece, checked against the Hugging Face tokenizer.json of the model

def test_words_are_lowercased_stripped_of_accents_and_split_on_punctuation(tokenizer):
    assert tokenizer.encode("Café") == tokenizer.encode("cafe")
    assert tokenizer.encode("hot-dog") == tokenizer.encode("hot") + tokenizer.encode("-") + tokenizer.encode("dog")


def test_a_known_word_is_one_token_and_a_rare_word_is_pieces(tokenizer):
    assert tokenizer.encode("clown") == [tokenizer.vocab["clown"]]
    pieces = tokenizer.encode("unballoonable")
    assert len(pieces) > 1 and pieces[1:] == [tokenizer.vocab[t] for t in tokenizer.tokens(pieces)[1:]]
    assert all(t.startswith("##") for t in tokenizer.tokens(pieces)[1:])


def test_an_unknown_character_is_unk(tokenizer):
    assert tokenizer.encode("☃") == [tokenizer.vocab["[UNK]"]]


def test_the_ids_match_the_real_tokenizer_json_for_sentences(tokenizer):
    # expected ids from the model's own tokenizer.json (Hugging Face `tokenizers`)
    ids, span = fb.build_text("a red balloon", tokenizer)
    assert ids.tolist() == [[101, 1037, 2417, 13212, 1012, 102]] and span == (1, 4)
    assert tokenizer.encode("Caf\u00e9 hot-dog unballoonable \u2603 don't 3.5") == \
        [7668, 2980, 1011, 3899, 4895, 7384, 7828, 3085, 100, 2123, 1005, 1056, 1017, 1012, 1019]


def test_the_span_covers_exactly_the_phrase_and_skips_the_separators(tokenizer):
    ids, span = fb.build_text("fox", tokenizer)
    flat = ids[0].tolist()
    assert flat[0] == tokenizer.vocab["[CLS]"] and flat[-1] == tokenizer.vocab["[SEP]"]
    assert flat[span[0]:span[1]] == [tokenizer.vocab["fox"]] and flat[span[1]] == tokenizer.vocab["."]


def test_a_phrase_longer_than_the_model_reads_is_refused(tokenizer):
    with pytest.raises(fb.NodeError) as error:
        fb.build_text(" ".join(["word"] * 300), tokenizer)
    assert error.value.code == "prompt_too_long"


# the picture as the model's input

def test_the_picture_is_scaled_to_fit_and_padded_with_the_valid_part_marked():
    image = Image.new("RGB", (200, 100), (255, 255, 255))
    pixels, mask, valid = fb.letterbox(image, 64, np.float32)
    assert pixels.shape == (1, 3, 64, 64) and mask.shape == (1, 64, 64)
    assert valid == (64, 32)
    assert mask[0, :32, :].all() and not mask[0, 32:, :].any()
    assert (pixels[0, :, 32:, :] == 0).all() and pixels[0, 0, 0, 0] > 1  # white, normalised, is above 1


def test_a_picture_with_alpha_is_put_on_white():
    image = Image.new("RGBA", (10, 10), (0, 0, 0, 0))
    pixels, _, _ = fb.letterbox(image, 8, np.float32)
    assert pixels[0, 0, 0, 0] > 2


# the model's answer as boxes

def sig_inv(p):
    return float(np.log(p / (1 - p)))


def answer(rows, tokens=256):
    """rows: (cx, cy, w, h, {token: probability}); logits are -10 where nothing is said."""
    logits = np.full((1, len(rows), tokens), -10.0, np.float32)
    boxes = np.zeros((1, len(rows), 4), np.float32)
    for i, (cx, cy, w, h, says) in enumerate(rows):
        boxes[0, i] = (cx, cy, w, h)
        for token, p in says.items():
            logits[0, i, token] = sig_inv(p)
    return logits, boxes


def test_a_box_is_scaled_to_the_picture_and_scored_by_the_best_token_of_its_phrase():
    logits, boxes = answer([(0.5, 0.5, 0.5, 0.5, {1: 0.4, 2: 0.9})])
    [found] = fb.decode(logits, boxes, (1, 3), "clown", (200, 100), threshold=0.3)
    assert found["box"] == pytest.approx((50, 25, 150, 75)) and found["score"] == pytest.approx(0.9, abs=1e-4)
    assert found["phrase"] == "clown"


def test_a_token_outside_the_phrase_does_not_score():
    logits, boxes = answer([(0.5, 0.5, 0.5, 0.5, {0: 0.99, 3: 0.99})])  # [CLS] and the period
    assert fb.decode(logits, boxes, (1, 3), "x", (100, 100), 0.3) == []


def test_the_threshold_cuts_and_results_come_best_first():
    logits, boxes = answer([(0.2, 0.2, 0.1, 0.1, {1: 0.5}), (0.8, 0.8, 0.1, 0.1, {1: 0.9}), (0.5, 0.5, 0.1, 0.1, {1: 0.7}),
                            (0.5, 0.9, 0.1, 0.1, {1: 0.29})])
    found = fb.decode(logits, boxes, (1, 2), "x", (100, 100), 0.3)
    assert [round(f["score"], 2) for f in found] == [0.9, 0.7, 0.5]


def test_a_box_that_covers_another_is_dropped_but_a_neighbour_is_kept():
    logits, boxes = answer([(0.50, 0.5, 0.4, 0.4, {1: 0.9}), (0.51, 0.5, 0.4, 0.4, {1: 0.8}), (0.9, 0.5, 0.1, 0.4, {1: 0.6})])
    found = fb.decode(logits, boxes, (1, 2), "x", (100, 100), 0.3)
    assert [round(f["score"], 2) for f in found] == [0.9, 0.6]


def test_a_box_is_kept_inside_the_picture():
    logits, boxes = answer([(0.95, 0.5, 0.4, 0.4, {1: 0.9})])
    [found] = fb.decode(logits, boxes, (1, 2), "x", (100, 100), 0.3)
    assert found["box"][2] == 100


def match(score, box, phrase):
    return {"score": score, "box": box, "phrase": phrase}


def test_phrases_are_merged_best_first_and_max_results_keeps_the_best():
    cat = [match(0.5, (0, 0, 10, 10), "cat"), match(0.9, (50, 50, 60, 60), "cat")]
    dog = [match(0.7, (20, 20, 30, 30), "dog")]
    assert [m["phrase"] for m in fb.merge([cat, dog], 0)] == ["cat", "dog", "cat"]
    assert [m["score"] for m in fb.merge([cat, dog], 2)] == [0.9, 0.7]


def test_the_same_box_found_for_two_phrases_is_kept_once_under_the_better_one():
    person = [match(0.6, (10, 10, 50, 90), "person")]
    man = [match(0.8, (11, 10, 50, 91), "man")]
    assert [m["phrase"] for m in fb.merge([person, man], 0)] == ["man"]


def test_boxes_are_written_one_per_line_in_whole_pixels():
    text = fb.format_boxes([{"box": (10.4, 20.6, 30.5, 40.0), "score": 1, "phrase": "x"},
                            {"box": (0, 0, 5, 5), "score": 1, "phrase": "x"}])
    assert text == "10 21 30 40\n0 0 5 5"
    assert fb.format_boxes([]) == ""


# the node, with a stand-in model that has the interface of GroundingDINO

def request(tmp_path, model, **inputs):
    source = tmp_path / "in.png"
    Image.new("RGB", (100, 80), (10, 10, 10)).save(source)
    work = tmp_path / "work"
    work.mkdir(exist_ok=True)
    req = {"protocol": 1, "op": "run", "id": "r1", "work_dir": str(work),
           "inputs": {"image": {"type": "image", "path": str(source)}, "query": "thing", "threshold": 0.3,
                      "max_results": 0, "device": "cpu", **inputs}}
    if model is not None:
        req["inputs"]["model"] = {"resource": "x/grounding-dino-model", "path": str(model)}
    return req, work


@pytest.fixture(scope="module")
def stand_in(tmp_path_factory):
    sys.path.insert(0, str(Path(__file__).parent))
    import make_dino_fixture

    path = tmp_path_factory.mktemp("dino") / "tiny-dino.onnx"
    make_dino_fixture.build(path)
    return path


def test_the_stand_in_model_gives_the_box_it_was_built_to_give(tmp_path, stand_in):
    req, work = request(tmp_path, stand_in)
    outputs = fb.Runner().run(req, lambda *a: None)
    # the stand-in says: box 1 (centre .25 .25, size .5 .5) 0.9 on the first query token, box 2 0.2 (below the threshold)
    assert outputs["boxes"] == "0 0 50 40"
    assert Image.open(work / outputs["preview"]["path"]).size == (100, 80)


def test_a_lower_threshold_lets_the_second_box_in_and_max_results_cuts_it_again(tmp_path, stand_in):
    req, _ = request(tmp_path, stand_in, threshold=0.1)
    assert fb.Runner().run(req, lambda *a: None)["boxes"].splitlines() == ["0 0 50 40", "50 40 100 80"]
    req, _ = request(tmp_path, stand_in, threshold=0.1, max_results=1)
    assert fb.Runner().run(req, lambda *a: None)["boxes"] == "0 0 50 40"


def test_each_phrase_is_run_alone_and_the_same_box_for_two_phrases_is_kept_once(tmp_path, stand_in):
    req, _ = request(tmp_path, stand_in, query="thing. other thing\nnothing")
    messages = []
    outputs = fb.Runner().run(req, lambda value, text: messages.append(text))
    assert outputs["boxes"] == "0 0 50 40"
    assert [m for m in messages if m.startswith("looking for")] == ["looking for thing", "looking for other thing", "looking for nothing"]


def test_nothing_found_is_an_empty_box_list_not_an_error(tmp_path, stand_in):
    req, _ = request(tmp_path, stand_in, threshold=0.95)
    assert fb.Runner().run(req, lambda *a: None)["boxes"] == ""


def test_an_empty_query_is_refused_before_the_model_is_read(tmp_path):
    req, _ = request(tmp_path, tmp_path / "gone.onnx", query=" ")
    with pytest.raises(fb.NodeError) as error:
        fb.Runner().run(req, lambda *a: None)
    assert error.value.code == "prompt_missing"


def test_a_missing_model_is_model_missing(tmp_path, monkeypatch):
    monkeypatch.delenv("FUGUE_DINO_MODEL", raising=False)
    req, _ = request(tmp_path, None)
    with pytest.raises(fb.NodeError) as error:
        fb.Runner().run(req, lambda *a: None)
    assert error.value.code == "model_missing"


def test_the_environment_names_a_model_when_the_input_is_unset(tmp_path, stand_in, monkeypatch):
    monkeypatch.setenv("FUGUE_DINO_MODEL", str(stand_in))
    req, _ = request(tmp_path, None)
    assert fb.Runner().run(req, lambda *a: None)["boxes"] == "0 0 50 40"


def test_serve_one_shot_writes_json_lines(tmp_path, stand_in):
    req, _ = request(tmp_path, stand_in)
    out = io.StringIO()
    code = fb.serve(io.StringIO(json.dumps(req)), out, persistent=False)
    lines = [json.loads(line) for line in out.getvalue().splitlines()]
    assert code == 0 and lines[-1]["op"] == "result" and lines[-1]["outputs"]["boxes"] == "0 0 50 40"


@pytest.mark.skipif(not os.environ.get("FUGUE_DINO_REAL_MODEL"), reason="set FUGUE_DINO_REAL_MODEL to a real GroundingDINO ONNX file")
def test_the_real_model_finds_the_red_square_by_its_name(tmp_path):
    source = tmp_path / "in.png"
    image = Image.new("RGB", (320, 240), (200, 200, 200))
    from PIL import ImageDraw

    ImageDraw.Draw(image).rectangle((200, 100, 280, 180), fill=(220, 20, 20))
    image.save(source)
    req = {"work_dir": str(tmp_path / "w"), "inputs": {"image": {"path": str(source)}, "query": "red square",
                                                         "model": {"path": os.environ["FUGUE_DINO_REAL_MODEL"]},
                                                         "threshold": 0.3, "max_results": 1, "device": "cpu"}}
    x1, y1, x2, y2 = (int(v) for v in fb.Runner().run(req, lambda *a: None)["boxes"].split())
    assert abs(x1 - 200) < 25 and abs(y1 - 100) < 25 and abs(x2 - 280) < 25 and abs(y2 - 180) < 25
