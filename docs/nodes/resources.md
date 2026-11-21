# Resources and the GPU

The optional `needs` block says what a node uses. Fugue's scheduler uses it to decide what can run
at once and what to unload.

```json
"needs": {
  "gpu": {"api": "any", "vram_mb": 6000, "exclusive": false, "unified_ok": true},
  "memory_mb": 2000,
  "network": false,
  "files": false,
  "resources": ["fugue/upscale-model"]
}
```

| Field | Meaning |
| --- | --- |
| `gpu.api` | `any`, `cuda` or `vulkan`. Having `gpu` at all makes it a GPU node. |
| `gpu.vram_mb` | Your estimate of GPU memory. |
| `gpu.exclusive` | The node must have the GPU to itself. |
| `gpu.unified_ok` | Works on unified-memory machines. |
| `gpu.parallel` | Opt in to running beside other GPU work. Default false. |
| `memory_mb` | Your estimate of system memory. |
| `network` | The node uses the network. |
| `files` | It reads or writes outside its run folder. |
| `resources` | The [resource types](types.md) it reads. |

- **The estimates are not trusted.** The scheduler measures free memory itself. Report a result's
  `stats.peak_memory_mb` and `peak_vram_mb` so Fugue can refine them.
- A GPU node runs alone on the GPU unless it set `parallel`. GPU nodes wait for each other.
- `network`, `files` and `gpu` are the permissions the trust prompt shows. They are **declared, not
  enforced**.
- Say `out_of_memory` as the error code when memory really ran out: Fugue frees a resident model and
  tries once more.
- A persistent program that declares `needs.gpu` is a *resident model* while it runs. Fugue stops
  the least recently used one to make room, never one in the middle of a run.
