#!/usr/bin/env python3
"""Writes pixel_more.json: ComfyUI's own outputs for the Pad, Rotate, Flip, Blur, Sharpen, Invert and
Grow Mask nodes on small fixed inputs, which tests/nodes/test_pixel_more.cpp compares Fugue's against.

Run with ComfyUI's Python, from a ComfyUI checkout:
    cd ~/github/ComfyUI && .venv/bin/python ~/github/fugue/tests/nodes/golden/make_pixel_more.py
The checked-in file came from ComfyUI commit e9afbccc2 (2026-11-20), torch 2.15, scipy 1.18.
"""
import json
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.getcwd())
import nodes  # noqa: E402
from comfy_extras import nodes_images, nodes_mask, nodes_post_processing  # noqa: E402


def lcg(n, seed):
    s = (seed * 2654435761 + 12345) & 0xFFFFFFFF
    out = []
    for _ in range(n):
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(((s >> 24) & 0xFF) / 255.0)  # exact 8-bit levels
    return out


def image(h, w, c, seed):
    return torch.tensor(lcg(h * w * c, seed), dtype=torch.float32).reshape(1, h, w, c)


def flat(t):
    return [float(v) for v in t.reshape(-1).tolist()]


def img_case(inp, out):
    return {"in": {"w": inp.shape[2], "h": inp.shape[1], "c": inp.shape[3], "d": flat(inp)},
            "out": {"w": out.shape[2], "h": out.shape[1], "c": out.shape[3], "d": flat(out)}}


def value(r):
    return r.result[0] if hasattr(r, "result") else r[0]


cases = []

for name, ((l, t, r, b, f), (h, w)) in {
    "pad feathered": ((3, 0, 2, 4, 3), (10, 12)),
    "pad all sides": ((4, 5, 6, 7, 2), (9, 8)),
    "pad feathering too big": ((2, 2, 2, 2, 6), (8, 8)),
    "pad no feathering": ((8, 0, 0, 8, 0), (6, 6)),
}.items():
    inp = image(h, w, 3, len(name))
    out_img, out_mask = nodes.ImagePadForOutpaint().expand_image(inp, l, t, r, b, f)
    cases.append({"node": "pad", "name": name, "inputs": {"left": l, "top": t, "right": r, "bottom": b, "feathering": f},
                  **img_case(inp, out_img), "mask": {"w": out_mask.shape[2], "h": out_mask.shape[1], "d": flat(out_mask)}})

for rotation in ("none", "90 degrees", "180 degrees", "270 degrees"):
    inp = image(4, 6, 3, 7)
    cases.append({"node": "rotate", "name": "rotate " + rotation, "inputs": {"rotation": rotation},
                  **img_case(inp, value(nodes_images.ImageRotate.execute(inp, rotation)))})

for method in ("x-axis: vertically", "y-axis: horizontally"):
    inp = image(4, 6, 3, 8)
    direction = "vertically" if method.startswith("x") else "horizontally"
    cases.append({"node": "flip", "name": "flip " + direction, "inputs": {"direction": direction},
                  **img_case(inp, value(nodes_images.ImageFlip.execute(inp, method)))})

for c, radius, sigma in ((3, 1, 1.0), (4, 2, 1.5), (3, 5, 0.3), (3, 3, 9.0)):
    inp = image(9, 11, c, radius)
    cases.append({"node": "blur", "name": f"blur c{c} r{radius} s{sigma}", "inputs": {"radius": radius, "sigma": sigma},
                  **img_case(inp, value(nodes_post_processing.Blur.execute(inp, radius, sigma)))})

for c, radius, sigma, alpha in ((3, 1, 1.0, 1.0), (3, 2, 1.2, 0.8), (3, 4, 2.5, 5.0), (3, 2, 1.0, 0.0)):
    inp = image(9, 11, c, radius + 20)
    cases.append({"node": "sharpen", "name": f"sharpen r{radius} s{sigma} a{alpha}",
                  "inputs": {"radius": radius, "sigma": sigma, "amount": alpha},
                  **img_case(inp, value(nodes_post_processing.Sharpen.execute(inp, radius, sigma, alpha)))})

for c in (3, 4):
    inp = image(3, 5, c, 40 + c)
    cases.append({"node": "invert", "name": f"invert c{c}", "inputs": {},
                  **img_case(inp, nodes.ImageInvert().invert(inp.clone())[0])})

for expand, tapered in ((3, True), (3, False), (-2, True), (-2, False), (0, True), (12, True), (-12, False)):
    inp = image(9, 8, 1, 60)
    # mostly low, a few set pixels, so growth and erosion both show
    inp = torch.where(inp > 0.85, torch.ones_like(inp), inp * 0.4)
    if expand < 0:
        inp = 1.0 - inp
    mask = inp.reshape(1, 9, 8)
    out = value(nodes_mask.GrowMask.execute(mask, expand, tapered))
    cases.append({"node": "grow-mask", "name": f"grow {expand} tapered {tapered}", "inputs": {"expand": expand, "tapered_corners": tapered},
                  **img_case(mask.reshape(1, 9, 8, 1), out.reshape(1, 9, 8, 1))})

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pixel_more.json")
with open(path, "w") as f:
    json.dump(cases, f, separators=(",", ":"))
print(len(cases), "cases ->", path)
