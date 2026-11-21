# Types and handles

Every input and output has a port type. An output connects to an input only when both have the
same type id and version: there is no conversion in v0.1, and a node cannot declare `any`.
Any type can be a **list** (`"list": true`).

## Built-in types

| Type | In the request | You return |
| --- | --- | --- |
| `text` | string | string |
| `number` | number (integers within ±2^53) | number |
| `boolean` | true or false | true or false |
| `image`, `video`, `audio` | a media handle | `{"path": …}` to a file in `work_dir` |

A choice is a `text` input with `choices`. A mask is an `image` with one channel.

## Media handles

```json
{"type": "image", "path": "/…/blobs/3f9a….png", "hash": "sha256:…",
 "width": 1024, "height": 1024, "channels": 3, "format": "png"}
```

`path` is a file you can read; `hash` is its content. Videos add `fps`, `frames`, `duration`,
`has_audio`; audio has `sample_rate`, `duration`, `channels`. A video's pending trim and crop are
applied to a file before you get it, so you only ever read `path`. Images you return are stored
as PNG unless you write another format.

## Resource types (models)

A pack declares a resource type in `pack.json`:

```json
"types": [{"id": "io.github.alice.tools/palette-model", "version": 1, "kind": "resource",
           "name": "Palette model", "extensions": [".safetensors"], "folders": ["palette"]}]
```

A resource value names something loadable and never holds weights:

```json
{"resource": "fugue/lora", "hash": "sha256:…", "name": "detail-tweaker.safetensors",
 "path": "/…/models/loras/detail-tweaker.safetensors"}
```

Fugue finds the file by its hash in the model folders and adds the `path` to the request. A model
that is not on this machine fails the node with `model_missing` before your program starts. An
input of a resource type shows a model picker. The built-in types are `fugue/diffusion-model`,
`fugue/text-encoder`, `fugue/vae`, `fugue/lora`, `fugue/model`, plus `fugue/controlnet` and
`fugue/upscale-model`.

## Data types

`"kind": "data"` types are files a node produced, such as a latent. The value is a handle with a
hash, and you return `{"data": "<type id>", "path": "<file in work_dir>", "meta": {…}}`; Fugue
adds the hash. Data values travel on links only: they have no literal and no widget. Because the
hash is part of the cache key, changing only a later node does not recompute this one.

Declared types are nominal: id and version are the whole type.
