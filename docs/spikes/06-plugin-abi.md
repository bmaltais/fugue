# Spike 06: how to version Fugue's native plugin ABI

Issue: #6. Date: 2026-11-19. Verdict: **one exported function, a `struct_size` first field on every
table in both directions, a major and minor number checked twice (manifest, then binary), and
optional features as string-id extensions whose ids never change. Copy CLAP's shape and OBS's size
check; do not copy a hand-bumped version number as the only guard.**

## Question

[Spec 6.9](../spec.md#69-native-tier) promises a plain C ABI that is "a versioned table of function
pointers", frozen only after prior-art research. Which version scheme, growth rule and ownership
rules have survived in real plugin systems, and what should `fugue_plugin.h` look like so we do
not freeze a bad interface?

## Method

Primary sources only: headers and docs from each project's own repo or site, fetched with `curl`
on 2026-11-19. Every claim has a numbered source from the list at the end. Where a source says
nothing, this note says "not found". No plugin was built or run; this is a reading study.

## Systems compared

| System | Entry point | Version scheme | Negotiation | Growth rule | Ownership and threads |
| --- | --- | --- | --- | --- | --- |
| OBS Studio [1][2][3][4] | `obs_module_ver()` and `obs_module_load()` found by name. | One `uint32`: major, minor, patch. | Host rejects a module built against a newer major or minor. Older modules load. | `obs_register_source_s(info, sizeof(...))`. Host copies only `size` bytes; larger than host is an error. | Plugin owns its `void *data`. Not stated in the headers read. |
| VST3 [5][6][7] | `GetPluginFactory()`. | No global number. Each interface has an IID. | `queryInterface(iid)` returns the interface or `kNoInterface`. | A released interface never changes. New features mean a new interface (`IPluginFactory2`, `3`). | Reference counted (`addRef`, `release`). |
| LV2 [8][9][10] | `lv2_descriptor(index)`. | `minorVersion`, `microVersion` in the `.ttl`. The URI is the major version. | Host passes a `features` array. Plugin returns extension data for a URI via `extension_data(uri)`. | Add ports or features only with a minor bump; anything else needs a new URI. | Host never frees `extension_data` results. |
| CLAP [11][12][13][14][15][16][17][18][29] | One exported const struct `clap_entry`. | `clap_version` (major, minor, revision) in entry, host and descriptor. | Plugin asks the host `get_extension("clap.name/N")`. Host asks the plugin the same way. | No size field. Fixed structs; new things are new extensions with a revision in the id. | Descriptors owned by plugin until `deinit`. Extension pointers owned by the other side. Thread tags in every comment. |
| OpenFX [19][20][21][22] | `OfxGetNumberOfPlugins`, `OfxGetPlugin(n)`. | `OfxPlugin.apiVersion`, plus plugin major and minor. | Host calls `fetchSuite(host, name, version)`; NULL means unknown. | New suite version = new struct. Host ignores plugins whose `apiVersion` it lacks. | Handles are opaque. Strict main and render thread rules. |
| Vulkan [23][24] | `vkGetInstanceProcAddr`; drivers export `vk_icdNegotiateLoaderICDInterfaceVersion`. | `apiVersion` for the API; an integer for the loader and driver interface. | Loader offers its version; driver answers with the highest both support. | `sType` plus `pNext` chains. Unknown structs are skipped. | Not studied in depth. |
| SQLite extensions [25][26][27] | `sqlite3_extension_init`, name derived from the file. | None for the table. Library version only. | None. The extension gets `sqlite3_api_routines*`. | "Add new interfaces to the end of this structure only." `sqlite3_vfs` has `iVersion`. | Not studied in depth. |
| Node-API [28] | `NAPI_MODULE_INIT`. | `NAPI_VERSION` baked into the addon. `napi_get_version` at run time. | Host supports every version up to its highest; addon uses what it asked for. | Additive only; defaults to version 8 unless the addon opts in. | All values are opaque handles. Every call returns a status code. |

## Findings per system

### OBS Studio

- Every module exports `obs_module_ver()`, which returns `LIBOBS_API_VER`: major in the top byte,
  minor in the next, patch in the low 16 bits [1][2].
- The host rejects a module whose version is greater than its own. It masks off the patch part
  first [4]. It does not reject an older module; old plugins keep loading.
- Struct growth is the part worth copying. The macro `obs_register_source(info)` expands to
  `obs_register_source_s(info, sizeof(struct obs_source_info))` [3]. The host copies exactly
  `size` bytes into a zeroed struct and refuses a size larger than its own [4]. A required
  field is accepted only if it lies inside `size` (`HAS_VAL` uses `offsetof` plus `sizeof`) [4].
  Old plugins keep working because the missing tail reads as zero.
- A separate `version` field on the struct exists "if needed to specify a new version" [3]. Old
  callbacks stay with a `@deprecated` note and a numbered twin (`get_defaults2`) [3]. That is
  the price of never changing a slot.
- Pain point: the same header holds two ways to grow (size and `version`) plus numbered twins.
  The project does not call this a problem. None found in fetched sources.

### VST3

- The entry point is `GetPluginFactory()`. The factory is a COM-like object [6]. Discovery
  grows by new factory interfaces: `IPluginFactory`, then `IPluginFactory2` (adds `PClassInfo2`
  with `sdkVersion`), then `IPluginFactory3` (adds wide-string `PClassInfoW`) [6].
- The rule, in the project's words: "once an interface has been released, it must never change
  again. Adding new functionality to an interface requires a new version" [7].
- A caller asks for what it knows with `queryInterface(iid, &obj)`. `kNoInterface` is a normal
  answer [5]. That is capability negotiation by identity.
- Cost: reference counting and `queryInterface` on every object is heavy for a node that
  mostly runs once. The names also pile up (`Factory3`). Both are my reading; the sources do not
  call them problems.

### LV2

- The host loads the library, calls `lv2_descriptor(index)`, and finds a plugin by URI [8].
- `instantiate` takes a NULL-terminated `features` array of `{URI, data}` pairs. Plugins may
  refuse if a feature is missing. Hosts "MUST NOT use this as a discovery mechanism"; they read
  the RDF data instead and never instantiate an unsupported plugin [8].
- `extension_data(uri)` returns "a struct containing function pointers to extend the
  `LV2_Descriptor` API", NULL for unknown URIs, never freed by the host [8].
- Versioning: `minorVersion` rises for backward compatible additions, `microVersion` for fixes.
  "There is deliberately no major version": the URI is the major version, and a breaking change
  needs a new URI [10]. A plugin declares `requiredFeature` and `optionalFeature` in its data [9].
- Lesson: the requirements sit in a data file the host can read without loading code. Fugue
  already has that file: `node.json`.
- Pain point: LV2 has two discovery functions (`lv2_descriptor` and `lv2_lib_descriptor`) [8].
  The older one is kept. None else found in fetched sources.

### CLAP

- The entry is one exported const struct, `clap_entry`, holding `clap_version`, `init`, `deinit`
  and `get_factory(id)` [11]. A factory is found by string id (`"clap.plugin-factory"`) [15].
- Both sides carry `clap_version` (major, minor, revision). 0.x was unstable and 1.x is stable;
  `clap_version_is_compatible` only checks `major >= 1` [12]. Compatibility is therefore carried
  by the major number plus the extension ids, not by the minor.
- Features are extensions found by string: `host->get_extension(host, id)` and
  `plugin->get_extension(plugin, id)`. NULL means "not supported". Pointers are owned by the
  provider and valid until `destroy` [13][14].
- Extension id rule: `clap.$NAME/$REV`, revision starts at 1; third parties use
  `$REVERSE_URI.$NAME/$REV` [17]. Draft extensions live in a `draft` folder. When one becomes
  stable "its extension ID must not change" [17]. In the headers, drafts carry ids such as
  `clap.tuning/2` and `clap.triggers/1`; the stable `clap.params` has no revision [18][29].
- Pain points the project admits:
  - Before 1.2.0 some draft ids contained the string `draft`. Removing it needed extra
    `_COMPAT` ids to keep old binaries working [17]. The id is an ABI promise; never put a status
    word in it.
  - `init` and `deinit` were documented as called once. "We realized, though, that this is not a
    requirement hosts can meet", so from 1.2.0 plugins must survive repeated calls [11].
- Structs have no size field. That works because CLAP almost never adds a field: it adds an
  extension. The cost is an extension id for every small change.

### OpenFX

- Hosts rely on two exported symbols, `OfxGetNumberOfPlugins` and `OfxGetPlugin(n)`; all other
  communication starts from those two [21]. A newer header also has `OfxSetHost` [19].
- `OfxPlugin` holds `pluginApi` (a string such as `"OfxImageEffectPluginAPI"`), `apiVersion`,
  a reverse-domain `pluginIdentifier`, plugin major and minor, `setHost` and one `mainEntry` [19][20].
- If the host lacks that API or version it "should simply ignore that plug-in". A binary can
  expose several versions of itself and present an old one to an old host [20].
- The host hands over `fetchSuite(host, suiteName, suiteVersion)`; it returns NULL for an
  unknown name or version. Repeated calls return the same pointer, valid while the binary is
  loaded [19]. Host function tables are named and versioned individually.
- Operations are strings (actions) sent to one `mainEntry` with property sets as arguments [21].
  That is flexible but untyped. The same shape as our command protocol's `op` strings.
- Threads: one main thread and zero or more render threads per instance; only a listed subset of
  actions may run on render threads, and a plugin declares how many it supports [22]. The doc
  admits a workaround: many hosts keep two instances, one for the UI and one for rendering [22].
- OFX is the closest domain match (image effects in a node graph), and it keeps images as
  opaque handles with a property set. No size field.

### Vulkan

- Structures start with `sType` and `pNext`. Anything that does not know a struct in the chain
  "must skip over, without processing" it [23]. New data goes in a new struct on the chain; the
  old struct is untouched.
- `apiVersion` is the highest version the app is designed for. Vulkan 1.0 drivers had to return
  `VK_ERROR_INCOMPATIBLE_DRIVER` if it was above 1.0; 1.1 and later must not, for any value [23].
  Lesson: failing on a newer minor was too strict and had to be reversed.
- Loader and driver negotiate: the loader passes the version it wants in
  `vk_icdNegotiateLoaderICDInterfaceVersion`; the driver lowers it to the highest both support, or
  returns `VK_ERROR_INCOMPATIBLE_DRIVER` if it no longer supports the loader's version [24].
  A driver that lacks the function is assumed to be version 0 or 1 [24]. The negotiation call
  is made first, before any other call [24].

### SQLite loadable extensions

- The entry is `sqlite3_extension_init(db, errmsg, const sqlite3_api_routines *pApi)`; the file
  name decides the symbol name [25].
- `sqlite3ext.h` carries the rule in capitals: "add new interfaces to the end of this structure
  only. If you insert new interfaces in the middle of this structure, then older different
  versions of SQLite will not be able to load each other's shared libraries" [26].
- The table has no size field. An extension built against a newer header can read a slot that an
  older SQLite never filled. This is my reading of the header; the source does not discuss it.
- Where SQLite does use a version number (`sqlite3_vfs.iVersion`) it records a mistake: the
  struct "changed ... and yet the iVersion field was not increased" [27]. A hand-bumped number
  can be forgotten. `sizeof` cannot.

### Node-API

- It is a C API with opaque handles and a status code on every call [28].
- The addon picks a `NAPI_VERSION` at compile time; the runtime supports "all versions between 8
  and the highest" and defaults to 8 unless the addon opts in [28]. `napi_get_version` tells the
  addon the highest version at run time so it can use a newer function if present [28].
- It adds one point the others lack: the version is chosen by the plugin's build, so it equals
  the minimum the plugin needs. This is the idea behind `min_host_minor` below.

## Lessons

| Copy | Source |
| --- | --- |
| A size field passed with every table, and a "slot is readable only if it lies inside the size" check. It is computed by the compiler, so it cannot be forgotten. | OBS [3][4], SQLite's `iVersion` oversight [27] |
| Put requirements in data the host reads without loading code. Fugue's `node.json` already has `runtime.abi`. | LV2 [8][9] |
| One exported symbol; everything else hangs off tables. | OFX [21], CLAP [11], SQLite [25] |
| Optional features by string id with a revision, found by `get_extension`; NULL means no. | CLAP [13][14][17], LV2 [8], OFX [19] |
| An extension id never changes after it ships, even when a draft turns stable. | CLAP [17] |
| Append-only tables. | SQLite [26] |
| Opaque handles and a status result; no layout shared. | Node-API [28], OFX [20] |
| The plugin states the minimum host minor it needs, not the minor it saw. | Node-API [28] |
| Thread rules stated per function. | CLAP [13][14], OFX [22] |

| Avoid | Source |
| --- | --- |
| A version number as the only guard. It can be forgotten. | SQLite [27] |
| Hard-failing on a newer minor version. | Vulkan [23] |
| Status words (`draft`) inside ids. | CLAP [17] |
| "Called once" rules the host cannot keep. Make `shutdown` safe to repeat. | CLAP [11] |
| Numbered twins of the same slot (`get_defaults2`). Prefer a new extension. | OBS [3] |
| Reference counting and `queryInterface` on every object. Too heavy here. | VST3 [5][7] |
| Stringly typed property bags for core data. We have a closed set of value kinds. | OFX [21] |
| Two discovery functions. | LV2 [8] |

## Recommendation for Fugue

### Rules

1. One exported function, `fugue_plugin_entry(host)`. The host passes its table; the plugin returns
   its table, or NULL when it cannot work with this host.
2. ABI numbers: `FUGUE_ABI_MAJOR` changes only for a break. `FUGUE_ABI_MINOR` rises when slots or
   host functions are added. The plugin records `min_host_minor`, the lowest host minor it needs
   (the Node-API idea).
3. `node.json` carries `"abi": "1.2"` for native nodes: major 1, needs host minor 2 or higher.
   Spec 6.9 and 6.1 already reserve an `abi` field; this fixes its meaning. The registry checks it
   at index time with no `dlopen`, so an incompatible plugin is listed with a reason and never
   loaded. This fits [6.12](../spec.md#612-the-registry): manifests at startup, code on first use.
4. Mismatches:

| Case | What happens |
| --- | --- |
| Different major | Rejected at index time. The node shows "built for ABI 2, this Fugue speaks 1". |
| Plugin needs a newer host minor (old host, new plugin) | Rejected at index time from the manifest; again from `min_host_minor` at load. The plugin entry also returns NULL. |
| Older plugin on a newer host (new host, old plugin) | Works. The host reads a plugin slot only if it lies inside the plugin table's `struct_size`. |
| New plugin on an old host, optional feature | Works. The plugin finds it by `host->get_extension(id)`; NULL means it takes the fallback. |

5. One growth mechanism: `uint32_t struct_size` first in every table, set by the side that builds
   it. Tables are append-only. A change in meaning, not just an added slot, is a new extension
   id (`name/2`), never an edit. Do not add CLAP-style `clap_version` fields on top of sizes.
6. Extension ids: `fugue.$NAME/$REV` for ours, `$REVERSE_URI.$NAME/$REV` for others, following
   CLAP [17]. An id is never renamed. No `draft`, `experimental` or `beta` in an id.
7. Values (text, number, boolean, image, resource, data) are opaque `fugue_value *` read through
   host functions. No `VipsImage *` or Qt type crosses the boundary, so plugins are not tied to
   libvips' ABI. Images are views: width, height, channels, a pixel format, a pointer and a
   stride in bytes. The plugin names the format it wants and the host converts. Formats are
   8-bit, 16-bit and float32 in 0..1; float32 is the engine's working format
   ([spike 03](03-libvips.md)), so a pixel node that asks for it gets the image without a copy
   when the host already holds it that way.
8. Errors are strings, matching [9.5](../spec.md#95-errors) and [7.2](../spec.md#72-one-shot-mode-default):
   `bad_input`, `node_failed`, `node_bad_output`, `node_timeout`, `out_of_memory`, `cancelled`.
   The set stays open, as it is in the JSON protocol.
9. Memory: the plugin never frees host memory and the host never frees plugin memory. Every
   `const char *` and value the host returns is valid until the `run` call returns. The host
   copies every string and buffer the plugin gives it. Tables the plugin returns stay valid until
   `shutdown`.
10. Threads: `create`, `destroy` and `shutdown` run on the engine thread. `run` runs on a worker
    thread. `progress`, `log`, `error` and `is_cancelled` may be called from any thread the plugin
    started during `run`, but only until `run` returns. One `run` at a time per instance. Two
    instances of one type may run together only if the type sets `FUGUE_NODE_PARALLEL_OK`
    (the OFX render-thread flag [22]). `shutdown` is safe to call more than once.
11. No C++ exception may cross the boundary. The wrapper catches and reports `node_failed`.
12. No C enum type crosses the boundary: kinds and formats are `uint32_t` with named constants,
    because the size of an enum is up to the compiler.

### Header sketch

```c
#ifndef FUGUE_PLUGIN_H
#define FUGUE_PLUGIN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FUGUE_ABI_MAJOR 1
#define FUGUE_ABI_MINOR 0
#define FUGUE_EXT_PREVIEW "fugue.preview/1" /* host: fugue_host_preview */
#define FUGUE_EXT_STATS "fugue.stats/1"     /* host: fugue_host_stats */
#define FUGUE_NODE_PARALLEL_OK 1u
/* Read a slot only if it lies inside the table's struct_size. */
#define FUGUE_HAS(p, T, f) ((p)->struct_size >= offsetof(T, f) + sizeof((p)->f))
typedef struct fugue_value fugue_value; /* text number bool image file list */
typedef struct fugue_ctx fugue_ctx;     /* one run of one node */
/* Kinds and pixel formats are uint32_t across the boundary: C does not fix an enum's size. */
typedef uint32_t fugue_kind;  /* FUGUE_KIND_* */
typedef uint32_t fugue_pixel; /* FUGUE_PX_*: channels are interleaved */
enum { FUGUE_KIND_TEXT = 1, FUGUE_KIND_NUMBER, FUGUE_KIND_BOOL, FUGUE_KIND_IMAGE, FUGUE_KIND_FILE, FUGUE_KIND_LIST };
enum { FUGUE_PX_U8 = 1, FUGUE_PX_U16 = 2, FUGUE_PX_F32 = 3 }; /* F32: 0..1, the working format */
typedef struct {
  uint32_t width, height, channels;
  fugue_pixel format;
  void *data;       /* inputs: read-only. outputs: the plugin fills it */
  ptrdiff_t stride; /* bytes per row; may be negative */
} fugue_image;
typedef struct fugue_host {
  uint32_t struct_size, abi_major, abi_minor;
  const void *(*get_extension)(const struct fugue_host *h, const char *id);
  const fugue_value *(*input)(fugue_ctx *c, const char *name); /* NULL if unset */
  fugue_kind (*kind)(const fugue_value *v);
  bool (*get_number)(const fugue_value *v, double *out);      /* also bool */
  const char *(*get_text)(const fugue_value *v);              /* UTF-8 */
  const char *(*get_path)(const fugue_value *v);              /* file kinds */
  bool (*get_image)(const fugue_value *v, fugue_pixel want, fugue_image *out);
  uint32_t (*list_count)(const fugue_value *v);
  const fugue_value *(*list_at)(const fugue_value *v, uint32_t i);
  fugue_value *(*new_text)(fugue_ctx *c, const char *utf8);
  fugue_value *(*new_number)(fugue_ctx *c, double x);
  fugue_value *(*new_image)(fugue_ctx *c, uint32_t w, uint32_t h, uint32_t ch,
                            fugue_pixel fmt, fugue_image *out);
  bool (*set_output)(fugue_ctx *c, const char *name, fugue_value *v);
  const char *(*work_dir)(fugue_ctx *c);
  void (*progress)(fugue_ctx *c, double value, const char *message);
  bool (*is_cancelled)(fugue_ctx *c);
  void (*log)(fugue_ctx *c, int level, const char *message);
  void (*error)(fugue_ctx *c, const char *code, const char *message);
} fugue_host;
typedef struct fugue_node_type {
  uint32_t struct_size;
  const char *id;    /* the node type id in node.json */
  uint32_t flags;    /* FUGUE_NODE_* */
  void *(*create)(const fugue_host *h);          /* NULL: no instance state */
  bool (*run)(void *inst, fugue_ctx *c);         /* false: error() was called */
  void (*destroy)(void *inst);
  const void *(*get_extension)(void *inst, const char *id);
} fugue_node_type;
typedef struct fugue_plugin {
  uint32_t struct_size, abi_major, min_host_minor;
  uint32_t (*node_count)(void);
  const fugue_node_type *(*node_type)(uint32_t index);
  void (*shutdown)(void);                        /* safe to call twice */
  const void *(*get_extension)(const char *id);
} fugue_plugin;
#if defined(_WIN32)
#define FUGUE_EXPORT __declspec(dllexport)
#else
#define FUGUE_EXPORT __attribute__((visibility("default")))
#endif
/* The only exported symbol. Returns NULL when this host is not supported. */
FUGUE_EXPORT const fugue_plugin *fugue_plugin_entry(const fugue_host *host);
#ifdef __cplusplus
}
#endif
#endif
```

Notes on the sketch:

- `FUGUE_HAS` is the OBS `HAS_VAL` check [4] in one macro. The host uses it on `fugue_plugin` and
  `fugue_node_type`; a plugin uses it on `fugue_host` before using a slot newer than its
  `min_host_minor`.
- `preview` (7.2 `preview`) and `stats` (7.2 `stats`) are extensions, not core. Core stays small and
  grows without touching the table. A GPU or Vulkan hand-off ([spike 04](04-gpu.md)) would be
  another extension, added when the first native GPU node needs it.
- `list_count` and `list_at` carry the list inputs of 7.2.
- Video, audio, data and resource values are files: `get_path` returns the resolved path, as
  command nodes get a `path` in 7.2 and 2.5. Only images are in memory ([2.4](../spec.md#24-media-handles)).
- `create` takes only the host. Instance state is the plugin's `void *`; the host passes it back.

### Mapping to the command protocol

| Command protocol ([7](../spec.md#7-command-node-protocol)) | Native ABI |
| --- | --- |
| `run` request, `inputs` | `run(inst, ctx)`; each input by name through `host->input` |
| unset optional input is absent | `input` returns NULL |
| `result` outputs | `new_*` then `set_output(name, value)` |
| `work_dir` | `host->work_dir(ctx)` |
| `progress` | `host->progress` |
| `preview` | extension `fugue.preview/1` |
| `stats` | extension `fugue.stats/1` |
| `error` with `code`, `message` | `host->error(code, message)` then return false |
| stderr log | `host->log` |
| `cancel` | `host->is_cancelled(ctx)`, polled |
| persistent mode `ready`, runs, `shutdown` | `create`, `run` repeatedly, `destroy` |
| `idle_timeout_s`, memory pressure | the engine calls `destroy`, same as the `shutdown` message |
| `node_bad_output`, `node_failed` | the engine checks outputs against the manifest after `run`; false without `error` is `node_failed` |

### The thin C++ wrapper

`fugue_plugin.hpp` is a header only and lives in the SDK. It does the following:

1. Defines `fugue_plugin_entry`. It checks `host->abi_major` and `host->abi_minor` against the
   compile-time constants, then returns the static `fugue_plugin` table, or NULL.
2. Sets `min_host_minor` from the highest `FUGUE_ABI_MINOR` the author asked for (the
   Node-API [28] pattern), so authors cannot forget it.
3. Offers `template <class T> register_node("id")`. The class gets `run(Context&)`. The wrapper
   builds the `fugue_node_type` table from it and sets `struct_size` with `sizeof`.
4. Wraps handles: `Context::input<double>("amount")`, `Context::image("image")`,
   `ctx.progress(0.4, "pass 2")`, `ctx.cancelled()`, and `ctx.fail("bad_input", "...")`.
5. Catches every C++ exception in `run`, `create` and `destroy`. It maps them to `node_failed`
   and `out_of_memory` (`std::bad_alloc`).
6. Checks `FUGUE_HAS` before using an optional host slot and exposes it as `std::optional`.

Built-in nodes use the same header, so the first built-in native node tests it before the freeze.

## Decision

1. **Freeze these rules, not the slot list.** The slot list in the sketch is a start; the rules
   (one entry, `struct_size`, append-only, string-id extensions, never rename an id) are what make
   later growth safe.
2. **ABI `1.0` stays unreleased until two built-in nodes use the header.** Spec 6.9 says
   built-ins dogfood it. Suggested: one pixel node and one node that keeps state across runs.
3. **`runtime.abi` in `node.json` is the string `"<major>.<min_host_minor>"`** and is checked before
   `dlopen`.
4. **Core stays tiny; features are extensions.** `preview` and `stats` start as extensions.
5. **Errors are open strings** from the 9.5 set, not an enum.

## Open questions for the maintainer

| Question | Recommendation |
| --- | --- |
| Do we need to stop a native node that ignores cancel? In-process, there is no equivalent of SIGKILL. `node_timeout` ([7.4](../spec.md#74-cancel-and-timeouts)) cannot be enforced. | Say so in the docs. Cancel is cooperative only. For a node that must be killable, use the command tier, which can be killed. Detect a stuck node and show "not responding" instead of promising a kill. |
| Should `abi` be a string `"1.2"` or an object `{"major": 1, "min_minor": 2}`? | A string. It matches `version` fields elsewhere and is easy to read. Reject anything that does not parse. |
| Which pixel formats? | `U8`, `U16` (PNG storage, [2.4](../spec.md#24-media-handles)) and `F32` (the in-memory working format, spike 03) from the start. More are new constants later; `get_image` returns false for a format the host cannot supply. Decided by the reviewer from spike 03. |
| Is a plugin allowed to start its own threads? | Yes, but only during `run`, and only to call `progress`, `log`, `error` and `is_cancelled`. All of them stay thread-safe. |
| Do we keep the plugin ABI behind one major for the life of v0.x? | Yes. Plan for `1.x` only. A `2.0` would mean a new header and a manifest that names both, as OFX allows one binary to offer several API versions [20]. Not needed now. |
| Should resource handles ([2.5](../spec.md#25-resource-handles)) be resolved to a path (like command nodes) or to a loaded model? | A path, as `get_path`. Loading and residency stay with the scheduler ([9.3](../spec.md#93-residency)). Revisit when the first native model node exists. |
| Sandboxing and trust. | Out of scope for the ABI. Spec 6.9 already states: not sandboxed, trust prompt applies. |

## Limits

- Reading study only. No plugin was built against the sketch with a real host. The C was checked
  for syntax only (`gcc -std=c11 -fsyntax-only -Wall -Wextra`), not for behaviour.
- The pages at `lv2plug.in/ns/lv2core/` returned 404. LV2 facts come from the project's GitHub
  mirror (`lv2.h`, `lv2core.ttl`, `lv2core.meta.ttl`).
- The VST3 interface list at `steinbergmedia.github.io/vst3_dev_portal` partly returned 404;
  the "never change" rule is from the Module Architecture page [7] and the factory chain from the
  header [6]. The VST3 "pain points" above are my reading, not a quote.
- The brief mentioned a `_DRAFT` id suffix for CLAP. The current headers and `extension-id.md` show
  a `/N` revision rule and say old ids had the word `draft` and were replaced with `_COMPAT` ids
  [17]. Behaviour before 1.2.0 was not checked beyond that note.
- CLAP and OBS were read on `main`/`master`; version numbers (CLAP 1.2.10, OBS 33.0.1) were current
  at the fetch and will move.
- OFX docs at `openfx.readthedocs.io` were read for the plugin struct, structure and thread
  pages only. Property suites and the full action list were not read.
- Vulkan and SQLite were read for the version rules only. Their ownership and threading rules were
  not studied.
- Python's limited API was not read; Node-API gave the point it would have.
- No benchmark of call overhead. A table of function pointers costs one indirect call per host
  function, which is small beside an image filter or a model run, but it was not measured.

## Sources

All fetched 2026-11-19.

1. OBS `obs-module.h`: https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs/obs-module.h
2. OBS `obs-config.h`: https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs/obs-config.h
3. OBS `obs-source.h`: https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs/obs-source.h
4. OBS `obs-module.c`: https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs/obs-module.c
5. VST3 `funknown.h`: https://raw.githubusercontent.com/steinbergmedia/vst3_pluginterfaces/master/base/funknown.h
6. VST3 `ipluginbase.h`: https://raw.githubusercontent.com/steinbergmedia/vst3_pluginterfaces/master/base/ipluginbase.h
7. VST3 Developer Portal, VST Module Architecture: https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/VST+Module+Architecture/Index.html
8. LV2 `lv2.h`: https://raw.githubusercontent.com/lv2/lv2/main/include/lv2/core/lv2.h
9. LV2 `lv2core.ttl`: https://raw.githubusercontent.com/lv2/lv2/main/lv2/core.lv2/lv2core.ttl
10. LV2 `lv2core.meta.ttl`: https://raw.githubusercontent.com/lv2/lv2/main/lv2/core.lv2/lv2core.meta.ttl
11. CLAP `entry.h`: https://raw.githubusercontent.com/free-audio/clap/main/include/clap/entry.h
12. CLAP `version.h`: https://raw.githubusercontent.com/free-audio/clap/main/include/clap/version.h
13. CLAP `host.h`: https://raw.githubusercontent.com/free-audio/clap/main/include/clap/host.h
14. CLAP `plugin.h`: https://raw.githubusercontent.com/free-audio/clap/main/include/clap/plugin.h
15. CLAP `factory/plugin-factory.h`: https://raw.githubusercontent.com/free-audio/clap/main/include/clap/factory/plugin-factory.h
16. CLAP README: https://raw.githubusercontent.com/free-audio/clap/main/README.md
17. CLAP `conventions/extension-id.md`: https://raw.githubusercontent.com/free-audio/clap/main/conventions/extension-id.md
18. CLAP `ext/draft/tuning.h`: https://raw.githubusercontent.com/free-audio/clap/main/include/clap/ext/draft/tuning.h
19. OpenFX `ofxCore.h`: https://raw.githubusercontent.com/AcademySoftwareFoundation/openfx/main/include/ofxCore.h
20. OpenFX docs, The OfxPlugin Struct: https://openfx.readthedocs.io/en/main/Reference/ofxPluginStruct.html
21. OpenFX docs, Structure of the OFX API: https://openfx.readthedocs.io/en/main/Reference/ofxStructure.html
22. OpenFX docs, Thread and Recursion Safety: https://openfx.readthedocs.io/en/main/Reference/ofxThreadSafety.html
23. Vulkan specification: https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html
24. Vulkan Loader, Loader and Driver Interface: https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/main/docs/LoaderDriverInterface.md
25. SQLite, Run-Time Loadable Extensions: https://www.sqlite.org/loadext.html
26. SQLite `sqlite3ext.h`: https://raw.githubusercontent.com/sqlite/sqlite/master/src/sqlite3ext.h
27. SQLite `sqlite.h.in` (`sqlite3_vfs`, `iVersion`): https://raw.githubusercontent.com/sqlite/sqlite/master/src/sqlite.h.in
28. Node.js Node-API documentation: https://raw.githubusercontent.com/nodejs/node/main/doc/api/n-api.md
29. CLAP `ext/params.h`: https://raw.githubusercontent.com/free-audio/clap/main/include/clap/ext/params.h
