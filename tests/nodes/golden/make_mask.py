#!/usr/bin/env python3
"""Writes mask.json: ComfyUI's own outputs for ImageCompositeMasked, ThresholdMask and MaskComposite
(Fugue's Composite Masked, Threshold Mask and Combine Masks) on small fixed inputs, which
tests/nodes/test_mask_nodes.cpp compares Fugue's against. ComfyUI's nodes are run as an oracle; their
code is not read or copied (GPL-3.0). Offsets stay within ComfyUI's own range (0 and up): its nodes
fail on a negative one, so Fugue's handling of a source before the left or top edge has worked
numbers in the test instead.

Run with ComfyUI's Python, from a ComfyUI checkout:
    cd ~/github/ComfyUI && .venv/bin/python <fugue>/tests/nodes/golden/make_mask.py
The checked-in file came from ComfyUI commit e9afbccc2 (2026-11-20), torch 2.15, scipy 1.18

"""
import json
import os
import subprocess
import sys

import torch

sys.path.insert(0, os.getcwd())
from comfy_extras import nodes_mask  # noqa: E402


def lcg(n, seed):
    s = (seed * 2654435761 + 12345) & 0xFFFFFFFF
    out = []
    for _ in range(n):
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(((s >> 24) & 0xFF) / 255.0)  # exact 8-bit levels
    return out


def image(h, w, c, seed):
    return torch.tensor(lcg(h * w * c, seed), dtype=torch.float32).reshape(1, h, w, c)


def mask(h, w, seed, special=()):
    m = torch.tensor(lcg(h * w, seed), dtype=torch.float32).reshape(1, h, w)
    flat = m.reshape(-1)
    for i, v in special:  # exact values, for the cases a random level seldom hits
        flat[i % flat.numel()] = v
    return flat.reshape(1, h, w)


def flat(t):
    return [float(v) for v in t.reshape(-1).tolist()]


def pic(t):  # an image [1,h,w,c] or a mask [1,h,w]
    if t.dim() == 3:
        return {"w": t.shape[2], "h": t.shape[1], "c": 1, "d": flat(t)}
    return {"w": t.shape[2], "h": t.shape[1], "c": t.shape[3], "d": flat(t)}


def value(r):
    return r.result[0] if hasattr(r, "result") else r[0]


cases = []

# ---- Composite Masked ----
dest = image(10, 12, 3, 1)
src = image(5, 6, 3, 2)
m_src = mask(5, 6, 3)
m_small = mask(3, 4, 4)
for name, x, y, resize, m in (
    ("no mask", 2, 3, False, None),
    ("mask", 2, 3, False, m_src),
    ("mask of another size", 4, 1, False, m_small),
    ("at the origin", 0, 0, False, m_src),
    ("crosses the right and bottom edge", 9, 8, False, m_src),
    ("fully outside", 40, 40, False, m_src),
    ("resize_source, no mask", 0, 0, True, None),
    ("resize_source and mask", 0, 0, True, m_src),
    ("resize_source, mask of another size, offset", 3, 2, True, m_small),
):
    try:
        out = value(nodes_mask.ImageCompositeMasked.execute(dest, src, x, y, resize, m))
    except Exception as e:  # ComfyUI itself fails here: no golden output, so no case
        print("ComfyUI fails on composite", name, "->", type(e).__name__)
        continue
    case = {"node": "composite-masked", "name": name, "inputs": {"x": x, "y": y, "resize_source": resize},
            "destination": pic(dest), "source": pic(src), "out": pic(out)}
    if m is not None:
        case["mask"] = pic(m)
    cases.append(case)

# a source with alpha onto a destination with alpha
dest4 = image(6, 7, 4, 5)
src4 = image(4, 3, 4, 6)
out = value(nodes_mask.ImageCompositeMasked.execute(dest4, src4, 2, 1, False, mask(4, 3, 7)))
cases.append({"node": "composite-masked", "name": "four channels", "inputs": {"x": 2, "y": 1, "resize_source": False},
              "destination": pic(dest4), "source": pic(src4), "mask": pic(mask(4, 3, 7)), "out": pic(out)})

# ---- Threshold Mask ----
for value_, seed in ((0.5, 10), (0.0, 11), (1.0, 12), (0.25, 13), (0.9, 14)):
    m = mask(6, 7, seed, special=[(0, 0.0), (1, 0.5), (2, 1.0), (3, 0.25), (4, 0.9), (5, 0.9000000238418579)])
    out = value(nodes_mask.ThresholdMask.execute(m, value_))
    cases.append({"node": "threshold-mask", "name": f"threshold {value_}", "inputs": {"value": value_},
                  "mask": pic(m), "out": pic(out)})

# ---- Combine Masks ----
d = mask(8, 9, 20, special=[(0, 0.0), (1, 1.0), (2, 0.5), (3, 0.5)])
s = mask(4, 5, 21, special=[(0, 0.5), (1, 0.5), (2, 1.0), (3, 0.0)])
for op in ("multiply", "add", "subtract", "and", "or", "xor"):
    for name, x, y in (("inside", 2, 3), ("origin", 0, 0), ("past the right and bottom", 6, 5),
                       ("outside", 30, 30)):
        try:
            out = value(nodes_mask.MaskComposite.execute(d, s, x, y, op))
        except Exception as e:  # ComfyUI itself fails here: no golden output, so no case
            print("ComfyUI fails on combine", op, name, "->", type(e).__name__)
            continue
        cases.append({"node": "combine-masks", "name": f"{op} {name}", "inputs": {"operation": op, "x": x, "y": y},
                      "destination": pic(d), "source": pic(s), "out": pic(out)})
# a source larger than the destination
big = mask(12, 13, 22)
for op in ("add", "subtract", "xor"):
    out = value(nodes_mask.MaskComposite.execute(d, big, 1, 2, op))
    cases.append({"node": "combine-masks", "name": f"{op} larger source", "inputs": {"operation": op, "x": 1, "y": 2},
                  "destination": pic(d), "source": pic(big), "out": pic(out)})

try:
    commit = subprocess.check_output(["git", "rev-parse", "--short=9", "HEAD"], cwd=os.getcwd()).decode().strip()
except Exception:
    commit = "unknown"
import scipy  # noqa: E402
meta = {"comfyui_commit": commit, "torch": torch.__version__, "scipy": scipy.__version__}
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mask.json")
with open(path, "w") as f:
    json.dump({"meta": meta, "cases": cases}, f, separators=(",", ":"))
print(len(cases), "cases", meta, "->", path)
