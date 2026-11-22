# Mesh research (epic #203)

Research notes for the Fugue mesh: peers that advertise models and nodes, dispatch parts of a workflow to each other, and account for usage. Written 2026-11-22 by research agents from primary sources and a read of this repo; claims marked **[U]** or "unverified" could not be checked against a source and must be re-checked before they drive a decision. The decisions themselves are in issue #203, not here.

1. [Code today: where a subgraph can run elsewhere](#fugue-mesh-epic-203-what-the-code-does-today)
2. [Membership and discovery](#fugue-mesh-epic-203-peer-discovery-and-membership-without-a-coordinator)
3. [Dispatch and placement prior art](#prior-art-distributed-inference-and-job-dispatch-for-the-fugue-mesh-epic-203)
4. [Trust, identity, security and accounting](#fugue-mesh-epic-203-trust-identity-security-accounting)

# Fugue mesh epic (#203): what the code does today

Repo ~/github/fugue at 8a9b6088 (main), read-only. Paths are under src/engine unless stated. "UNVERIFIED" marks what I did not confirm by reading or running.

## 1. One run, step by step, and the seams

There is no separate RunService/residency-planner/run-recorder class. All three live inside one `Scheduler` class in `scheduler.cpp` (1444 lines, `m_dry` checked ~14 times), wrapped by `Run` (scheduler.h:127) and `runGraph` (scheduler.h:155). The window side is `src/run/run_service.h` (run thread, queue, check()).

Step by step (window run):
1. `compileForRun(document, registry, request, setup)` (compile_for_run.cpp:14-36): registerProviders, embedGroupTypes, `compile()` with `target` ("run to here"), `provider`, trust callback. Returns `CompiledGraph` (compiler.h:~85): flat `QMap<path, CompiledNode{type, version, inputs, sink, provider, frozen}>`, JSON round-trip `toJson/fromJson` (`fugue run compiled.json` exists).
2. RunService makes a `RunFolder` (run_folder.h:44: run.json, compiled.json, workflow.json, roots.json, events.jsonl, nodes/<path>/), enqueues in the cross-process queue (queue.h), waits for turn + engine lock (run_service.h header).
3. `Scheduler::start()` (scheduler.cpp:278): `graph.order()`, builds a `NodeRun` per node, binds registry type by major version and kernel (NodeKernel or ProviderKernel), writes compiled.json, emits run.started/node.queued, `describeProviders()` (asks every provider instance to `describe`).
4. `advance()` loop (431): `settle()` -> `prepare()` per node whose deps finished; `startReady()`.
5. `prepare()` (493-603): freeze lookup; skip on upstream failure; `resolveInputs()` (735): defaults, link values from upstream `outputs`, `auto` seed made concrete via `env.randomSeed`/QRandomGenerator (665-671), path inputs located, read-roots checked, file inputs hashed (`FileHashes`) and keyed as `{"file": hash}`; provider node: `selectProvider()` (providers.h) picks instance+model; builds `KeyParts` and `cacheKey()` (result_cache.h:44-64); `ResultCache::lookup` -> `hit()` (node.cached) else Phase::Ready.
6. `startReady()` (838): ready nodes stable-sorted so ones whose model is resident go first; lanes: CPU slots, one GPU lane (`usesGpuLane` 827: provider nodes only when instance is local), one job per provider instance (`m_busy`); GPU nodes call `makeRoom()` (878).
7. `startNode()` (1009): `nodeDir`, `NodeContext` (contextFor 962: inputs, workDir, hashes, models, registry, document, usedUpstream, progress/preview/cancelled callbacks). Kernel node: `kernel->run(ctx)` on thread pool, `filed()` writes in-memory images. Provider node: `kernel->request()` -> `provider->submit(JobRequest)` -> `statusChanged`/`jobStatus` (1095, progress mapped into node.progress, preview PNG) -> `provider->result()` -> `kernel->next()` (more jobs) or `kernel->outputs()`.
8. `returned()` (1170): `checkOutputs`, `ResultCache::store(key, outputs, nodeDir, ...)` (files moved into blobs/<hash>, hard link left), models recorded in `ResidentModels`, `release()` lane, state Cached, event node.done with `seconds`, `advance()`.
9. `finish()` (1307): run.json, run.done; `RunOutcome{status, errors, states, keys, outputs (QMap<path,Values>), ran}` (scheduler.h:113).

Seams for "subgraph runs elsewhere and returns named outputs":
- **Entry already exists as a function over JSON**: `runGraph(CompiledGraph, RunFolder, RunEnvironment, RunOptions)` returns `RunOutcome.outputs` keyed by path id. A peer could call exactly this on a received compiled graph. This is the cleanest seam.
- **Cut-out**: `CompileOptions::target` (compiler.h) already keeps "only the target and what it needs". There is no "boundary inputs" concept: a cut subgraph needs its boundary links replaced by literals or by data/media handles (hash-addressed files). Nothing does that today (new code, a pure function on CompiledGraph).
- **Per-node remote execution** is the existing Provider seam (provider.h:~130 `Provider` with describe/submit/status/cancel/result/shutdown, all async). A "peer provider" that takes a whole `JobRequest{capability:"graph.run", params:{compiled graph}, inputs:{files}}` fits the interface with no scheduler change; the scheduler would treat the whole subgraph as one node. Tradeoff: ProviderKernel is per node type, so a synthetic node type `fugue/remote-subgraph` would be needed, and the cut must be made before compile (or as a transform of CompiledGraph).
- **Alternative seam inside the Scheduler**: in `startNode()` (1009) choose a "remote" executor per node. More invasive: Scheduler is one 1400-line class with `m_dry` branches; the epic's own "Scheduler split" (provider job, residency planner, run recorder) is not done. UNVERIFIED: the workbench handoff mentions it as a candidate; I did not find a written design.
- `m_busy` is per provider instance, so a remote peer instance is already off the GPU lane (scheduler.cpp:827-830, spec 9.2).

## 2. Can a half-finished graph resume elsewhere? Is a node pure?

State kept per node:
- Result cache (result_cache.h): `keys/<hex>.json` (type, outputs, sizes, last-used, used_by), `blobs/<hash>.<ext>`, `frozen/` markers. Key = SHA-256 of canonical JSON of type, version, pack, packVersion, every input's `Value::identity()`, engine + model identity (provider nodes), `keyExtra`, run id if non-deterministic (spec 10.1, scheduler.cpp:580-593).
- Run folder: per-node workDir, events.jsonl, run.json with `nodes[path].{state,key,outputs}`, `auto` seeds, `providers` used.
- Resume on the same machine already works: a re-run with unchanged inputs hits the cache for finished nodes. So "half-finished resume" = the cache, not a checkpoint of the run.
- To resume on ANOTHER machine, the blobs for the boundary values must exist there. Cache is per machine (`~/.cache/fugue/results`); no sync/transfer; `ResultCache` is "not thread-safe, one thread" and has no "import blob by hash" call (UNVERIFIED but I saw none in the header). Needed: `has(hash)`, `put(hash, bytes)`.

Purity (node = f(inputs, node version, seed)):
- Good: seed is resolved before keying, so a peer given the concrete seed is deterministic (scheduler.cpp:665). File inputs are keyed by content hash, resource handles by identity hash, data handles drop top-level `path` (value.cpp:~40 `object.remove("path")`), media identity drops `path` (value.cpp:91-95).
- Breaks purity or portability:
  - Engine and device: cache key includes engine id `sdcpp@<host>` (provider.cpp:82; spec 8.10 "results are deterministic per machine and build"). So a re-run on another peer is a different key and may give different pixels. The epic text "same result" holds for CPU nodes and same-build helper only. Re-running a lost job elsewhere gives an equivalent, not identical, image. Decide this explicitly.
  - `fugue/stitch` (crop_stitch.cpp:100-112,138-165): descriptor holds `meta.image`/`meta.mask` media handles with absolute `path`, and the Stitch node re-hashes those paths and fails with file_not_found if gone. The top-level path is stripped from identity but nested `meta.image.path` is not (value.cpp identity only strips the top-level key; UNVERIFIED that jsonIdentity strips nothing deeper, I read only lines 40-42). Crossing a machine needs the stitch's two images shipped by hash and the paths rewritten. Treat as a non-cuttable edge or ship the three files.
  - Absolute paths: media/data handles carry `path` in values and events; `JobRequest.inputs` are `{"path": ...}` (components.cpp:707-945). Peers must rewrite to their own store paths (cache store paths are by hash, so this is mechanical).
  - Save/Preview/Compare sinks: write to user's output folder, `needs.files`; "Final outputs written by Save nodes are never cache entries" (spec 10.2). Sinks must stay on the originator (cut before them). `sink` is a flag on CompiledNode, easy to detect.
  - `deterministic:false` nodes key on run id (scheduler.cpp:588); not portable. Reject in a cut or treat as barrier. Load Image/Video/Audio read local files by path (keyed by content): the file must be shipped, or the node stays on the originator.
  - Wall clock or random in kernels: grep of src/nodes for QRandomGenerator/currentDateTime found none (my grep returned nothing); command nodes (command_node.cpp) and packs are opaque: UNVERIFIED.
  - Node text-generation / Enhance Prompt `keyExtra` reads template files on the executing machine (spec 10.1): the template fingerprint must match.
  - Frozen nodes (`frozen` key) are served from the local cache by key; meaningless on a peer.
  - `NodeContext.document` and `roots` (RunOptions) are local-machine context: a peer gets a different document/roots.

## 3. Residency planner: fit and time today

- Not a planner: reactive and LRU. `ResidentModels` (resident_models.h) is a keyed LRU list with no sizes. Order preference: `prefersResident` (scheduler.cpp:827). Fit: `makeRoom` (878) compares `env.freeGpuMemoryMb()` with the node's declared `needs.gpu.vram_mb` and evicts LRU while short; on `out_of_memory` evict and retry; error names GPU holders (`withMemoryHolders` 925).
- `needs.gpu.vram_mb` is read only in scheduler.cpp; grep over src/nodes/manifests found no manifest that declares it (UNVERIFIED for packs and templates). In practice the estimate is blank and fit is decided by the OOM retry.
- Time: nothing estimates time. The only timing is `node.done.seconds` in events.jsonl (scheduler.cpp:1211) and progress events. No per-stage speed table exists.
- Data available for a remote planner: free GPU memory per machine (`GpuMemoryProbe` gpu_memory.h:48: Vulkan budget, or MemAvailable on unified memory; device kind Discrete/Unified); the helper's `describe` returns `free_memory_bytes` (provider.h `Description::freeMemoryBytes`); model file sizes (`ModelFile.size` model_library.h:46, handle `size`), safetensors headers (`model_shape.h` reads shapes/families without loading), content hashes remembered in `~/.local/state/fugue/hashes.json` (file_hashes.h); resource handle `needs` hints (spec 2.5); cache record sizes after a run (`ResultCache::entries()`). Missing: load time, steps/sec per device, link speed.

## 4. Existing remote mechanisms

- Paired-host helper: `RemoteHelperProvider` (remote_helper_provider.h/.cpp, 577 lines) <-> `fugue-helper --serve-http --listen --port --key-file --models` (helper/src/main.cpp:~1200). Endpoints `POST /fugue/v1/{describe,submit,status,cancel,result,shutdown}`, `GET /fugue/v1/result/<job>/<i>`. One shared bearer key (`Authorization: Bearer`, constant-time compare, main.cpp:1206-1211), plain HTTP over the tailnet (spec: `http://<tailnet address>:<port>`). Key lives in providers.json mode 0600 (spec 8.2). Status polled every 500 ms; three failed polls = lost (kLostPolls).
- Data transfer: inputs go inline as `{"data": base64, "format"}` per job, every time (remote_helper_provider.cpp:~195-215; spec 8.7). Outputs fetched by `GET .../result/<job>/<i>` into the job's outputDir. **No content-hash addressing on the wire**: a latent used by two jobs is sent twice. Models are handles `{name, sha256, size}` resolved on the host from `--models` roots, hashed once and remembered in `model-hashes.json` (spec 8.7). Handles never contain paths (spec 2.5).
- Provider interface: spec 8.11 / provider.h. `describe -> Description{engine, capabilities[CapabilityOffer{capability, models[OfferedModel{handle, title, status, properties, limits, defaults}]}], sharesFilesystem, freeMemoryBytes}`.
- Omarchy hosts: `OmarchyProvider` reads `~/.local/state/omarchy/agent-local/status.json` (local) and `network.json` (paired hosts), key files `~/.config/omarchy/agent-local/remotes/<host>.key`; pairing is `omarchy-agent-local pair <host>` (omarchy-local-ai's, not Fugue's). `defaultProviderInstances` builds one instance per paired host `https://<dns>` (providers.cpp:260-300). Each model row is a served model named by recipe with no hash; capabilities per `kind` (image/text/video/music). Capability advertisement is therefore Omarchy's, not Fugue's: Fugue peers need their own.
- Trust: packs trusted per id+version in `~/.local/state/fugue/trust.json` (`TrustStore`, trust_store.h; compiler refuses untrusted, compiler.h `CompileOptions::trusted`). `fugue` and `local` packs are always trusted. No sandbox (spec 6.7). There is **no node content hash** today: only `NodeType::packVersion` and `version`; registry.h `packVersion(packId)`. "id+version+hash" needs a new pack/node-type hash (e.g. hash of manifest + files at load, in pack_loader).
- No peer discovery: grep for tailscale/mdns/avahi in src and spec found only spec text about hosts via omarchy `network.json`; nothing in code (grep returned nothing). Fugue has no Fugue-to-Fugue peer protocol, no Qt server (QtNetwork is client only here; UNVERIFIED that QtHttpServer is available in the build/packaging).

## 5. Cheap cut points

Port types (spec 2.1, 2.5, 2.6): text, number, boolean (bytes); `image` media handle {width,height,channels,format,hash}; `video` (always file, size by duration); `audio`; data `fugue/latent` (`meta` width, height, channels, family; video adds frames, fps; "tens of MB for a few seconds"), `fugue/conditioning` (helper stage file; size unknown, UNVERIFIED), `fugue/stitch` (descriptor, but references two images), resource handles (tiny; peer needs the file or loads by hash). Masks are `image` with role mask (spec 2.1; no mask type), so a mask is a one-channel PNG.
- The graph already knows types: `CompiledNode.type` -> `Registry::nodeTypeFor` -> `InputSpec.type/OutputSpec` (registry.h, node_type.h, port_type.h), compile checks types per link. Sizes are known only for media and latents from handle fields (width x height x channels; latent meta) and, after a run, from `CacheEntry.size`. Data handles carry no byte size. Estimates: latent bytes ~ w/8*h/8*c*2..4; computable from meta; conditioning not.
- Natural cheap cuts: after Text Encode (conditioning, small-ish), after Sampler (latent ~1 MB per image per spec epic), before VAE Decode; embeddings and masks small; images and clips large but hash-addressable.

## 6. What must change

- **Lease/admission layer**: new, peer-side. Today admission is a local lane and `m_busy` in Scheduler plus the cross-process file queue (queue.h, `engine.lock`) which is per machine. Needed: a `Peer` server object holding capacity (free VRAM from `GpuMemoryProbe`, declared job needs), grant/expire/release; the originator side as a Provider (`RemotePeerProvider`) or a pre-step. The helper has no admission at all (one job at a time in order, spec 8.7).
- **Job protocol with heartbeats and cancel**: the helper's 8.8 HTTP protocol already has submit/status(progress, stage)/cancel/result (provider.h `JobStatus{state, progress, stage, preview, error}`); heartbeat = existing poll (500 ms), cancel exists (cooperative, 9.6). Missing: lease id, per-node events for the subgraph (progress per inner node; today one job = one progress number), incremental output transfer, resumption by job id after reconnect, a protocol/Fugue version handshake (describe has `protocol` int only, provider.h Description).
- **Usage records**: no accounting anywhere (UNVERIFIED by grep). Closest data: `node.done.seconds`, run.json, `Description.freeMemoryBytes`. Peer must measure GPU-seconds, peak VRAM (not measured today: only free memory before start), bytes moved (transfer counted in RemoteHelperProvider? UNVERIFIED), model hash.
- **NodeRunner trust check (id+version+hash)** on the receiving side: `compile()` already checks id, major version (`node_version_mismatch`) and pack trust at installed version, on the sender's registry. The receiver must re-run `compile`-equivalent validation (registry lookup + trust + new hash) on the received `CompiledGraph`, not trust the sender's compile. `CompiledGraph::fromJson` does shape check only ("The registry is not consulted", compiler.h). Add `verifyForPeer(graph, registry, trust, peerPolicy)`.
- **Trust/auth for peers**: replace the single shared bearer key and plain HTTP with per-peer pairing and TLS (or tailnet identity); "my devices" mode needs some notion of owner identity: new.

## Smallest code changes: step 1 (advertise models + nodes, read-only)

Goal: a read-only view of what each peer offers.
1. `engine/peer_info.h/.cpp` (new): `PeerAdvert{fugueVersion, protocol, nodes:[{id, version, packVersion, hash}], models:[{type, name, sha256, size, loaded}], freeVramMb, deviceKind, load, timestamp, ttl}`, `toJson/fromJson`, and `buildAdvert(Registry, ModelLibrary, FileHashes, GpuMemoryProbe, ResidentModels)` using existing parts (registry node types, `listDiskModels`, remembered hashes with `RememberedOnly` hashing so it is quick, `GpuMemoryProbe::device/freeMb`, `ResidentModels::keys`).
2. `engine/node_type.*` / `pack_loader.cpp`: compute and store a content hash per node type (hash of manifest plus runtime files for pack nodes; for built-ins use fugue version + manifest hash). Touches `NodeType` (node_type.h:89 area), pack loader. Risk: hash must be stable across distros/AppImage (line endings, file order, build-time embedded manifests): use canonical JSON + sorted file hashes.
3. A tiny server: add `fugue serve-peer` CLI command (src/cli/*) exposing `GET /fugue/v1/peer/advert` (read-only) with the same bearer/pairing as the helper; or put the endpoint in `fugue-helper --serve-http` (helper/src/main.cpp:1224) which already has auth and the models roots, but then it only knows models, not nodes. Choose the former. Risk: no QtHttpServer dependency known (UNVERIFIED); the helper uses cpp-httplib. Cheapest: a minimal `QTcpServer` GET, or reuse httplib in a small separate process.
4. Discovery seed, minimal: a peers file (`~/.config/fugue/peers.json`, mirrored on `providers.cpp`'s reading of `providers.json`: address+key+name) plus the paired omarchy hosts from `network.json` as candidates. Defer gossip/mDNS/Tailscale.
5. Read-only UI: a panel listing peers and their nodes/models (QML under src/panels, like the Providers page, settings Providers page spec 8.2). Cache adverts with expiry.
6. Tests: advert JSON golden; server+client in-process with fake clock for expiry.
Risks: model hash cost (large files; use RememberedOnly and mark unknown), privacy of listing every model file, key/auth for a new endpoint, protocol version field from the start.

## Smallest code changes: step 2 (offload one subgraph to a peer that has everything)

1. `engine/subgraph.h/.cpp` (new, pure): `cutSubgraph(CompiledGraph, QStringList nodes) -> {CompiledGraph sub, boundaryInputs:[{input node/port, value}], outputs:[path/port]}`. Replace each cross-boundary link into the set with an input literal/handle (hash-addressed), list the ports leaving the set. Reject: sinks, non-deterministic nodes, file-reading nodes (Load*), frozen nodes, Stitch with its images unless shipped. Reuses `CompiledGraph::dependencies/consumers/order`, `CompiledNode.sink`, `NodeType::deterministic`.
2. `engine/result_cache.*`: `bool hasBlob(hash)`, `Result<QString> importBlob(hash, QIODevice&)` (verify hash, atomic write, read-only). Not thread-safe class: wrap on the run thread.
3. `engine/peer_run.h/.cpp` receiving side, runs on the peer: accept `{compiled graph, inputs by hash, protocol}`; `verifyForPeer` (registry lookup by id+version+hash, pack trust, built-ins are fine, report missing nodes, never install); fetch missing blobs (`GET`/`PUT /fugue/v1/blob/<hash>`); `runGraph` in a temp `RunFolder` with own `RunEnvironment`; stream per-node events; return outputs as `{port: value-with-hashes}` plus blobs fetched by hash. Needs the engine lock/queue so it does not fight the local owner's runs (queue.h).
4. `engine/remote_subgraph_provider.h/.cpp` originator side: implements `Provider` (describe from the peer advert, submit = lease+send, status polled with node events mapped to progress, cancel, result downloads blobs into outputDir). Plus a synthetic node type or a scheduler hook so the subgraph occupies one `NodeRun` and its outputs are written to the cache under the inner nodes' keys (so a local re-run is a hit). Touches scheduler.cpp (`prepare`, `startNode`, `returned`) and `registry` for the synthetic type; or build it as a pre-pass that rewrites the CompiledGraph (no scheduler change, cleaner): replace the cut set by one node of type `fugue/remote-run` whose outputs feed the original consumers.
5. UI: a node/group "run on peer" pin (reuse the existing `provider` pin on CompiledNode: `provider: "<peer name>"`; it is already carried and validated by `unknown_provider` in the compiler). Cheapest first version: pin a Group instance to a peer; `embedGroupTypes` and flattening already produce the inner nodes with a path prefix, so "subgraph = group instance" needs no cut algorithm. Show "ran on <peer>" from events.
6. Tests: loopback peer in-process (two Registries and ResultCaches), result equals local run for CPU-only nodes (pure); missing node reported not installed; killed peer mid-run falls back to local with a notice.
Risks: (a) engine/GPU determinism per host (cache key has `sdcpp@host`), so remote results do not satisfy local keys; (b) paths inside data handles and `fugue/stitch`; (c) shared single bearer key and HTTP; (d) base64-inline transfer is inefficient, add by-hash blob transfer first; (e) the Scheduler is a monolith with `m_dry`, so keep the change as a pre-pass or a Provider to avoid touching it; (f) the run queue/engine lock is per machine: a peer job must take that lock or it can oversubscribe the owner's GPU; (g) `RunEnvironment` is one per process, `ResultCache` single-thread.

---

# Fugue mesh (epic #203): peer discovery and membership without a coordinator

Researched 2026-11-22. Primary sources only (papers, official docs, READMEs, repo files, GitHub API metadata). Claims marked **[unverified]** could not be confirmed from a source I could open. Nothing in any repo was modified.

## Summary and recommendation

Fugue's realistic pool is 2 to 20 trusted machines, not thousands. At that scale SWIM's main benefit (constant per-node message load as the group grows) does not matter, and its cost (indirect probes, suspicion timers, piggyback queues, a 512-byte metadata cap in memberlist) buys little. **Recommendation:** write a small in-process C++/Qt membership layer rather than embedding a SWIM library: seeds from `tailscale status --json` (peers online and, optionally, tagged), mDNS/DNS-SD on a LAN, and a peers file; a direct authenticated connection (TLS or Noise, device-key pinned like Syncthing) to each known peer carrying heartbeats and a versioned, per-node key-value state in the Scuttlebutt/anti-entropy style (delta exchange, per-node versioned keys, only the owner writes its keys), with a phi-accrual (or simple adaptive-timeout) failure detector per peer, an explicit "leaving" message for clean drain, and a stall guard so a suspended laptop does not mark everyone dead (exo does this). Advertise compact digests in the gossip (hash of the model set, free VRAM, load, node-set hash) and fetch the full lists (model hashes, node ids/versions/hashes) on demand over the same connection, so payload size stops being a constraint. If the group ever needs to be much larger, run HashiCorp memberlist/Serf as a Go sidecar (MPL-2.0, file-level copyleft, fine for a separate binary). **Main risk:** owning a failure detector and anti-entropy protocol ourselves, in particular false "dead" verdicts on sleep/suspend, NAT/DERP-relayed latency spikes and partitions, which is why the fake-clock, kill-a-peer tests in the epic are mandatory and the state machine must read no clock (exo's `membership.rs` is a good model).

## Options compared

| Option | No coordinator | Join / leave / failure | WAN / tailnet | Advert payload | Licence | Embeddable in C++/Qt |
|---|---|---|---|---|---|---|
| SWIM (paper) | Yes, needs one seed to join | Probe, indirect probe, suspect, then confirm; join/leave/fail piggybacked on ping/ack | Designed for LAN-ish clusters; nothing special for WAN | Piggyback only; message size bounded by a constant | Paper | Algorithm only |
| HashiCorp memberlist (Go) | Yes, needs one seed | SWIM plus Lifeguard (local health), suspicion timeout `SuspicionMult*log(N+1)*ProbeInterval`, graceful leave, 30 s push/pull full-state sync over TCP | UDP probes plus TCP; WAN works with tuned timers (Serf has a separate WAN profile **[unverified in sources I read]**) | Node meta max **512 bytes**; user data limited by UDP buffer (default 1400) | MPL-2.0 | Go; not linkable from C++ sanely; sidecar only |
| Serf (Go agent) | Yes | memberlist plus user events and tags; CLI agent with RPC | As memberlist | Tags share the 512-byte meta cap; user event name+payload default 512, hard max 9 KB | MPL-2.0 | Sidecar program; talks RPC (msgpack, not JSON **[unverified]**) |
| Foca (Rust) | Yes | SWIM + infection + suspicion; transport-agnostic (you move bytes) | Depends on your transport | Custom broadcast data; no stated limit | MPL-2.0 | Rust crate; C ABI wrapper would be ours to write |
| chitchat (Rust, Quickwit) | Yes, seeds | Scuttlebutt anti-entropy with deltas that fit a UDP packet; phi-accrual detector; heartbeat key per round; dead-node GC | Works over UDP; no WAN claims in README | Arbitrary per-node key-value state, versioned; no hard limit stated | MIT | Rust crate; library, not a ready sidecar; design is the best model for us |
| Phi-accrual detector (Hayashibara) | Local judgement per node | Continuous suspicion level from heartbeat history, threshold chosen by the application | Adapts to jitter, good for variable links | n/a | Paper; Akka/Pekko impl Apache-2.0 | Easy to port (mean and stddev of inter-arrival times, normal CDF) |
| mDNS/DNS-SD (Avahi, QMdnsEngine, mjansson/mdns) | Yes | Announce, goodbye (TTL 0), re-query | **LAN only**; multicast does not cross a tailnet (Tailscale staff post, undated) | TXT records, small (single packet realistic ~1.3 KB; RFC limit 9 KB **[unverified here]**); use for seeds only | Avahi LGPL-2.1; QMdnsEngine MIT; mjansson/mdns Unlicense | QMdnsEngine fits Qt; mjansson/mdns is plain C |
| Tailscale peer list | Yes (control plane is Tailscale's, but not ours) | `Online`, `LastSeen`, key expiry per peer; no membership events beyond that, but `WatchIPNBus` pushes netmap changes | Native; direct UDP or DERP relay fallback | None (not an advert channel) | BSD-3-Clause | Run `tailscale status --json` or hit the local API unix socket; no library needed |
| libp2p (cpp-libp2p) | Yes (Kademlia, mDNS, gossipsub) | Pubsub meshes; membership is connection-based | Good, NAT traversal in Go/Rust impls; C++ one less so | Pubsub messages, flexible | Apache-2.0 / MIT | cpp-libp2p: CMake + Hunter, README lists gossipsub WIP and no mDNS; last push 2025-10 per GitHub API; heavy |
| Zenoh (zenoh-c, zenoh-cpp) | Yes (multicast scouting + gossip scouting, peer mode) | Liveliness tokens, sessions | Works over TCP/QUIC; routers optional | Pub/sub and queries, flexible | EPL-2.0 or Apache-2.0 (dual) | C and header-only C++ bindings over a Rust core; large dependency |
| Exo | Yes | See details; full mesh of connections, Alive/Suspect/Gone, leave announce | LAN multicast; not documented for tailnet **[unverified]** | Zenoh pub/sub; shape not read in full | Apache-2.0 | Reference design only (Python + Rust) |
| Syncthing discovery | Local: yes. Global: needs discovery servers | Local: periodic announce, `instance_id` change = restart | Global discovery is a central server (configurable) | Device id + address list only | MPL-2.0 | Protocol is trivial (protobuf over UDP broadcast); copyable design |

## Details per option

### SWIM and Lifeguard
- SWIM paper (Das, Gupta, Motivala): failure detection by random probing, join/leave/failure news spread by piggybacking on ping and ack messages, message load constant per member, expected constant detection time, and a suspicion sub-protocol to cut false positives. Verified by text extracted from the PDF: https://www.cs.cornell.edu/projects/Quicksilver/public_pdfs/SWIM.pdf
- Lifeguard (Dadgar, Phillips, Currey): extensions to SWIM using "local health" so a slow or starved prober does not blame healthy peers; reports fewer false positives and, simultaneously, faster true-failure detection. https://arxiv.org/abs/1707.00788
- Fit for Fugue: good principles to borrow (suspect before dead; local health, i.e. a node that is itself stalled should not accuse others), overkill as a full protocol at our scale. I did not extract the paper's numeric claims.

### HashiCorp memberlist and Serf
- memberlist README: SWIM plus (1) faster propagation/convergence and (2) Lifeguard. Go. https://github.com/hashicorp/memberlist
- Config defaults (config.go): `UDPBufferSize` 1400 bytes, `PushPullInterval` 30 s (full state sync), `ProbeInterval` 1 s, `SuspicionMult` 4. https://raw.githubusercontent.com/hashicorp/memberlist/master/config.go
- `MetaMaxSize = 512` bytes of node metadata (memberlist source, `MetaMaxSize = 512 // Maximum size for node meta data`). This alone rules out putting model-hash lists in node meta. Source file: https://github.com/hashicorp/memberlist (the constant sits near line 84 of a package file; I found it via grep, not a doc page).
- Serf: tags must fit in the 512-byte meta; user events default limit 512 bytes, hard cap 9 KB (serf/config.go and serf.go). https://github.com/hashicorp/serf
- Licence: MPL-2.0 for both (LICENSE files, "Copyright IBM Corp. 2013, 2026"). Both actively maintained (GitHub API: memberlist pushed 2026-11-22, serf 2026-10-15; not archived).
- Sidecar fit: a Go binary using memberlist with a delegate that gossips a small digest (version numbers and hashes) and serves detail over local JSON would work, but it adds a second runtime to package (AppImage, #202) and a second source of truth. MPL-2.0 is file-level copyleft: modifying MPL files requires publishing those files; linking/running a separate binary does not infect Fugue's MIT code. Not legal advice.
- Serf's agent RPC protocol: README does not describe it; I did not verify whether JSON is supported **[unverified]**.

### Foca (Rust, SWIM)
- No_std + alloc, transport agnostic, "SWIM+Inf.+Susp.", pluggable codec and broadcast data, MPL-2.0, pushed 2026-08. Lifeguard is not named on the docs page. https://docs.rs/foca/latest/foca/ , https://github.com/caio/foca
- Interesting as a design: the protocol as a pure state machine that is fed bytes and a clock (fits our fake-clock testing) but writing a Rust-to-C++ bridge is not attractive.

### chitchat (Quickwit)
- Scuttlebutt anti-entropy gossip with delta exchange, phi-accrual failure detection, one key-value namespace per node that only its owner edits, versioned tombstones with GC (recommended 15 min), dead nodes scheduled for deletion after half of `dead_node_grace_period` (default 24 h). MIT. https://github.com/quickwit-oss/chitchat
- Best conceptual match for "advertise a growing structured state without a coordinator". Rust, library only. Port the ideas (digest + delta, per-node version counters), not the code.

### Phi-accrual failure detector
- Hayashibara et al. 2004 ("The phi accrual failure detector"); I could not open the paper (403/404), so the description comes from Akka/Pekko documentation: phi is derived from the history of heartbeat inter-arrival times (normal distribution mean and stddev), with a configurable threshold, `max-sample-size`, `min-std-deviation`, `acceptable-heartbeat-pause`, heartbeats every second by default. A low threshold detects fast with more false alarms; a high one is slower and more accurate. https://pekko.apache.org/docs/pekko/1.0/typed/failure-detector.html , https://doc.akka.io/api/akka-core/2.9/akka/remote/PhiAccrualFailureDetector.html
- Fit: ~50 lines of C++; handles WAN jitter better than a fixed timeout. Needs a floor on stddev and an `acceptable-pause` for DERP-relayed links and laptop sleep.

### mDNS / DNS-SD
- libp2p's mDNS spec shows a clean pattern: PTR query `_p2p._udp.local`, TXT records carry addresses, peers answer their own query for passive discovery, optional SRV/A/AAAA. https://raw.githubusercontent.com/libp2p/specs/master/discovery/mdns.md
- Qt has no built-in mDNS/DNS-SD in QtNetwork (the QNetworkDatagram page does not mention it; I did not find an official Qt module, **[unverified]** that none exists in Qt 6.x add-ons).
- QMdnsEngine (nitroshare): MIT, C++/Qt, RFC 6762, CMake, pushed 2025-09 (GitHub API), 92 stars; small. https://github.com/nitroshare/qmdnsengine
- QtZeroConf (jbagg): LGPLv3, per its README as quoted in search results; wraps Avahi/Bonjour. Not suitable if we want MIT without LGPL obligations. https://github.com/jbagg/QtZeroConf
- mjansson/mdns: single C header, Unlicense (public domain), no internal allocations, DNS-SD discovery and responder; pushed 2025-10. https://github.com/mjansson/mdns
- Avahi: LGPL-2.1, needs the avahi-daemon over D-Bus; present on most desktops, not guaranteed everywhere (AppImage on other distros). https://github.com/avahi/avahi
- Limit: multicast does not cross the tailnet. Tailscale staff said mDNS over Tailscale "wasn't supported yet" in an undated forum thread (could be outdated): https://forum.tailscale.com/t/mdns-over-tailscale-network/221. Use mDNS for LAN seeds only.

### Tailscale as the seed list
- `tailscale status --json` is machine readable; the docs warn the format is subject to change and list no field names. https://tailscale.com/kb/1080/cli
- Field names verified in source (`ipn/ipnstate/ipnstate.go`, BSD-3-Clause) and against a real `tailscale status --json` on this machine: per peer `HostName`, `DNSName`, `TailscaleIPs`, `Online`, `LastSeen`, `Active`, `Relay`, `CurAddr`, `ExitNode`, `OS`, `KeyExpiry`, `Expired`; `Tags` exists in the struct (omitted when empty; this machine's sample peer had none). https://github.com/tailscale/tailscale/blob/main/ipn/ipnstate/ipnstate.go
- Local API: Go package `tailscale.com/client/local` exposes `Status`, `WatchIPNBus`, `WhoIs` over the daemon's unix socket (default platform path, overridable). `WhoIs(remoteAddr)` is useful to authenticate an incoming peer connection by its tailnet identity and tags. Methods without an explicit maturity note are "unstable" per the docs. https://pkg.go.dev/tailscale.com/client/local . From C++ the socket speaks HTTP, so a minimal client works; exact URL paths (`/localapi/v0/status`) are from memory **[unverified here]**.
- Tags: names `tag:xyz`, must be declared in `tagOwners`; ACL `src`/`dst` can use tags with port specs; ACLs are one-directional; grants are the newer recommended system and deny by default. A new tailnet's default policy allows all, but a custom policy can block peers from each other, so Fugue must treat "peer visible in status" as not meaning "peer reachable on our port"; probe the TCP port and surface "blocked by ACL?" to the user. Docs do not say whether a blocked peer still appears in status **[unverified]**. https://tailscale.com/kb/1337/policy-syntax , https://tailscale.com/kb/1018/acls
- MagicDNS gives each device a name (`machine.tailnet.ts.net`); the page documents no SRV or multicast. https://tailscale.com/kb/1081/magicdns
- Connections start via DERP relay and upgrade to direct UDP when NAT traversal works; relayed paths are slower. https://tailscale.com/kb/1257/connection-types . Heartbeat timeouts must tolerate relay jitter.
- Suggested ACL for users: a `tag:fugue` tag allowing `tag:fugue` to `tag:fugue:<port>` both ways; document it. A user's own untagged devices work under the default allow-all.

### libp2p
- gossipsub is a pubsub mesh protocol (v1.0 to v1.2), not a membership protocol; implementations in Go, JS, Rust, Python, Java, Nim; C++ is not in the list on the spec page. https://raw.githubusercontent.com/libp2p/specs/master/pubsub/gossipsub/README.md
- cpp-libp2p: Apache-2.0 / MIT, CMake with Hunter, TCP, Yamux/Mplex, Kademlia, gossipsub marked WIP, no mDNS listed, README targets GCC 7.4 and says C++17/C++20 inconsistently. https://github.com/libp2p/cpp-libp2p . Verdict: too heavy and too incomplete for what we need; skip.

### Zenoh
- Exo uses Zenoh (Rust). zenoh-c and zenoh-cpp (header-only over zenoh-c) are dual-licensed EPL-2.0 / Apache-2.0 per their READMEs, both pushed within the week. https://github.com/eclipse-zenoh/zenoh-c , https://github.com/eclipse-zenoh/zenoh-cpp
- Would give discovery, pub/sub and queries for free, but pulls a Rust core (packaging, binary size **[unverified]**) and a transport stack we would have to trust and track. Worth a prototype only if our own layer proves painful.

### Exo
- README: "Devices running exo automatically discover each other"; works on the same local network; needs macOS Local Network permission; `EXO_ZENOH_NAMESPACE` isolates clusters; peers must match cluster name, wire version and build; planning uses a "realtime view of your device topology" with latency and bandwidth per link. Apache-2.0. https://github.com/exo-explore/exo
- Source (rust/networking): `discovery.rs` sends UDP "Hello" packets (magic `EXO`, cluster-key hash, a nonce) to an IPv6 link-local multicast group and accepts replies; `membership.rs` is a pure state machine with Alive / Suspect / Gone, an explicit "leaving" announce that goes straight to Gone(Left) and is forgotten, a grace period before Gone, probes when a connection closes, and a stall guard so a node that was frozen or asleep holds off marking peers dead. Notably, "the state machine reads no clock: every input carries the time", which is exactly the testing approach our epic wants. https://github.com/exo-explore/exo/tree/main/rust/networking/src
- Observed: discovery is LAN multicast; I found no tailnet path **[unverified]**.

### Syncthing
- Local discovery v4: UDP, IPv4 broadcast, IPv6 multicast `ff12::8384`, port 21027, one packet type, every 30-60 s, content is device id (SHA-256 of the cert), address list and a random `instance_id` that changes on restart. https://docs.syncthing.net/specs/localdisco-v4.html
- Global discovery v3: HTTPS announce and query to a configurable discovery server, client cert auth, server cert pinned. This is a central server, so out for us. https://docs.syncthing.net/specs/globaldisco-v3.html
- Take: the device-id-from-certificate identity and the tiny LAN announce are good, cheap models for pairing and trust.

### C/C++/Qt permissive implementations of SWIM-style membership
Short list, none attractive:
- abbyssoul/libtribe: C++, Apache-2.0, SWIM-inspired, no networking included, last push 2019-08, 7 stars. https://github.com/abbyssoul/libtribe
- massenz/gossip: C++ SWIM failure detector, protobuf, last push 2021-03, GitHub API reports no licence (so unusable as-is). https://github.com/massenz/gossip
- Nothing maintained and permissive turned up in C++; that is the main argument for writing a small purpose-built layer, or using Go/Rust as a sidecar.

## Open questions

1. Scale target: is 20 peers a safe ceiling? If hundreds were ever realistic, revisit memberlist as a sidecar.
2. Does a Tailscale ACL-blocked peer appear in `tailscale status --json` with `Online: true`? Test on a real tailnet with a restrictive policy.
3. Does `tailscaled` expose peer tags in `Status` for non-tag-owner viewers? (Field exists; visibility rules not read.)
4. Transport and identity: reuse tailnet identity (`WhoIs`) as proof of who a peer is, or Fugue-own device keys (Syncthing-style) so LAN-only use works without Tailscale? The "my devices trusted / strangers need pairing" rule from the epic needs a Fugue key either way for non-tailnet LANs.
5. mDNS vs a Syncthing-style UDP multicast hello: the latter is far less code and dependency-free (QUdpSocket), and exo does it; mDNS interoperates with Avahi tooling and shows up in `avahi-browse`. Not decided.
6. Detector tuning on DERP-relayed links and sleeping laptops (suspend/resume): needs measurement.
7. Zenoh C++ size and dependency weight for AppImage: not measured.
8. Whether Serf's RPC supports a JSON mode if we ever use it as sidecar: not verified.
9. Licence questions above are my reading, not legal advice.

## Prototype first

1. **Membership state machine as a pure C++ class** (peer states Alive/Suspect/Gone/Left, inputs carry the time, phi-accrual or adaptive timeout, stall guard) with table tests on a fake clock: join, clean leave, kill, flap, laptop sleep. No sockets yet.
2. **Seed sources**: parse `tailscale status --json` (online peers, optional `Tags`) plus a peers file; probe the Fugue TCP port on each; show "visible but unreachable" distinctly. Add LAN discovery as either QMdnsEngine or a Syncthing-style UDP hello and compare effort.
3. **Two-process loopback test**: two real Fugue processes exchange heartbeats and a versioned key-value state (per-node owner-only writes, digest then delta), kill -9 one, observe Suspect then Gone within the deadline; graceful leave goes straight to Left.
4. **Advert shape**: a compact digest (model-set hash, node-set hash, free VRAM, load, Fugue/protocol version, timestamp with expiry) in the gossip, and full lists fetched on demand over the peer connection; measure typical sizes with a real model library.
5. **Time-boxed spike (1 day) of memberlist as a Go sidecar** with a JSON-over-stdio delegate, only to compare behaviour and packaging cost against the in-process layer; discard if not clearly better.

---

# Prior art: distributed inference and job dispatch for the Fugue mesh (epic #203)

Date: 2026-11-22. Method: WebFetch of READMEs/docs/papers. Marks: **[V]** verified from the cited source this session; **[U]** unverified (from memory or secondary source only).

## Summary and recommendation

Nothing in the AI field does what #203 asks (originator-dispatched, coordinator-free, lease-based, hedged dispatch of pure graph parts over a few heterogeneous consumer GPUs). The AI projects split into three groups:

1. **Model-splitting** (Exo, Petals, llama.cpp RPC, vLLM/SGLang disaggregation, Hivemind): they split one model across machines. This is what #203 marks "Not planned". They are useful only for discovery and for what they cost over slow links.
2. **Job farms** (AI Horde, ComfyUI-Distributed): whole jobs go to workers. Horde has a central server; ComfyUI-Distributed has a master. Both are weak on scheduling (no leases, no estimate-based placement, little failure handling documented).
3. **Classic scheduling theory** (Sparrow, power-of-two-choices, Tail at Scale, Nomad/K8s, BOINC, Condor, leases) supplies the real design material and is mostly license-free as ideas.

Recommendation: build the dispatcher from classic parts: Condor-style two-sided matching (peer admission = machine Requirements; originator = Rank), Nomad/K8s-style filter-then-score, Sparrow-style late binding via leases, and Tail-at-Scale deferred hedging. Reuse ideas only; no code needs to be taken from any project. Every code-bearing source below is either permissive (Exo Apache-2.0, Petals/Hivemind MIT, ComfyUI-Distributed Apache-2.0) or AGPL/GPL (Horde, ComfyUI core), and none is needed.

## Prior art table

| Project | Kind | Reuse as idea | Avoid | Licence |
|---|---|---|---|---|
| Exo | Model split, LAN auto-discovery | Zero-config discovery; placement previews (list valid placements, pick one) | Needs identical version/build on all nodes; Linux is CPU-only; mixed OS do not cluster; tensor parallelism needs fast links | Apache-2.0 [V] |
| Petals | Volunteer pipeline split | Public/private swarm distinction; per-block servers | 4-6 tok/s single batch; data visible to hosts; routing/fault tolerance not described on README [V] | MIT [V] |
| AI Horde | Central queue + volunteer workers | Kudos as priority; uptime kudos; escrow for untrusted new workers; pull model (workers poll, never listen); worker marks job "faulted" so server requeues early; max_power per worker | Central server (DB, Redis); slow-worker handling not documented; global currency | AGPL-3.0 [V] (docs only) |
| ComfyUI-Distributed | Master/worker batch fan-out | Seed-per-worker fan-out; tile fan-out for upscale; master falls back to local when all workers gone | No VRAM pooling; no single-image speedup; no documented retry; large GPU gaps cause bottlenecks | Apache-2.0 (README: yes [V]; only the README was read) |
| llama.cpp RPC | Layer offload over TCP | Tensor cache on the server (`-c`) so weights cross the link once | Self-described "proof-of-concept, fragile and insecure"; weights and KV split by memory ratio; no scheduling | MIT [U] (licence file not fetched) |
| Ray Serve | Replica router | Power-of-two-choices routing; `max_ongoing_requests` as per-replica admission (backpressure) | Central controller/actor | Licence not stated on page; Apache-2.0 [U] |
| vLLM / SGLang disaggregation | Prefill/decode split over KV transfer | Only the principle: ship the small intermediate, not the big state | Experimental; vLLM says it does NOT improve throughput [V]; needs fast RDMA-class links | vLLM Apache-2.0 [U] |
| Hivemind | Decentralized training library | DHT, tolerates unresponsive nodes | Training-oriented; heavy | MIT [V] |
| Sparrow | Decentralized low-latency scheduler | Probe d=2 samples; late binding (reservation placed, task bound when worker is ready) | Assumes homogeneous short tasks; no handling of 30 s-minutes GPU jobs with load cost | Paper (no code reuse) |
| Tail at Scale | Hedging | Deferred hedge at p95; tied requests; cancel loser | Naive eager hedging doubles load | Paper |
| Nomad / K8s | Cluster schedulers | Filter then score; bin-pack vs spread; image locality = warm model; optimistic concurrency with a rejecting authority (Nomad plan queue) = our peer-side admission | Central leader/plan queue; K8s default LeastAllocated spreads, wrong when a model is warm | MPL-2.0 (Nomad) [U], Apache-2.0 (K8s) [U]; ideas only |
| BOINC | Volunteer batch | Per-job deadline (`delay_bound`), reissue on miss, replication for validation; `enforce_delay_bound` refuses work the host cannot finish in time | Replication for trust (we have pure jobs and trusted peers); central server | LGPL-3 [U] |
| Condor ClassAds | Matchmaking | Two-sided Requirements + Rank; the machine's own policy (START/RANK) decides what it accepts | Central negotiator | Apache-2.0 [U] |
| Leases (Gray and Cheriton 1989) | Time-bounded grants | Grant expires unless renewed; dead holder frees capacity without detection | none | Paper [U] |
| Work stealing (Blumofe-Leiserson) | Idle workers pull | Idle peer pulls a part from a slow/queued peer | Needs shared deque state; use only as a late-stage tail optimisation | Paper [U] |

## Details

### (a) AI-specific

**Exo** (https://github.com/exo-explore/exo) [V]. Apache-2.0. Devices on the same network, same cluster name, same wire version and same build join automatically; guest Wi-Fi can block discovery. Placement is API-driven: `/instance/previews` lists valid placements, the user or caller picks one. Splits use topology awareness (device resources, link latency and bandwidth), tensor and pipeline parallelism; tensor parallelism claims 1.8x on 2 devices and 3.2x on 4. RDMA over Thunderbolt 5 requires matching OS versions exactly. Linux runs CPU only. Takeaways: strict version agreement is what #203's handshake already assumes; "previews of valid placements" is a good UI/test surface for the planner; split-model parallelism depends on special links, which supports "Not planned".

**Petals** (https://github.com/bigscience-workshop/petals) [V]. MIT. Each volunteer hosts blocks of a model; ~6 tok/s Llama-2 70B, ~4 tok/s Falcon 180B single batch. Public swarm exposes data to hosts; private swarm for trusted people. README does not describe routing or fault tolerance. Papers cited there hold the detail [not read]. Takeaway: per-step network hops make interactive speed poor; same conclusion as #203.

**AI Horde** (https://github.com/Haidra-Org/AI-Horde, https://github.com/Haidra-Org/haidra-assets/blob/main/docs/workers.md, .../kudos.md) [V]. AGPL-3.0; read docs only. Workers poll the API for jobs when their local queue has room and never listen on a port (works behind NAT). The worker pops the next job with about 30 s left on a long job (pre-fetch to hide latency). On unrecoverable failure the worker submits a "faulted" payload so the API reassigns sooner. Abandoned jobs are timed out and reassigned "if possible" (no durations documented). Worker guide: `max_power` caps job size per VRAM class (64 for 24 GB+, 50 for 12-16 GB, 32 for 8-10 GB); `max_threads` 1-2; `preload_timeout`, `extra_slow_worker` give slow hosts longer. Server can put a too-slow or failing worker in maintenance mode. Kudos: job payout equals job value; uptime kudos in 10-minute increments; new workers' rewards escrowed until trusted (about a week); queue priority by balance; ledger event-sourced with reservations. Takeaways for Fugue: declare a per-peer max job size by VRAM class; early "faulted" signal; escrow-style cap for new/unknown partners (maps to the reciprocal policy's deficit cap); do not copy the central ledger. Kudos amount formula and requeue timings: not verified.

**ComfyUI-Distributed** (https://github.com/robertvoy/ComfyUI-Distributed) [V from README]. Master dispatches to local, remote or cloud workers; parallel generation with different seeds, Ultimate-SD-Upscale tile fan-out, image-list fan-out to next free worker, optional load balancing (one queued run to one free participant). If all workers vanish, master re-enables local render. No VRAM pooling, no single-image speedup except tiles; mixed GPUs work but a big gap bottlenecks. Failure and retry beyond the fallback: not documented. Licence per README Apache-2.0 [V]; one secondary source (CivitAI) agreed [U]. Other extensions seen in search only (not read): ComfyUI_NetDist, ComfyUI Cluster (nomcycle), Raylight (xDiT/FSDP model split), ComfyUI-MultiGPU (placement on one host). ComfyUI core is GPL-3.0 [U, Wikipedia]; do not read its source.
Takeaway: tile and batch fan-out are the proven near-linear wins; "bottleneck from a slow peer in a fixed split" is exactly what late binding avoids (hand out tiles one at a time, pull-based).

**llama.cpp RPC** (https://github.com/ggml-org/llama.cpp/blob/master/tools/rpc/README.md) [V]. `ggml-rpc-server` exposes devices over TCP; client takes `--rpc host:port,...`; weights and KV cache split by memory proportion unless `--tensor-split`. README: proof-of-concept, fragile, insecure, never on an open network. `-c` caches large tensors on the server. RDMA optional (RoCEv2 or Thunderbolt 5). No benchmarks, limits not documented. Limit in practice: every step crosses the network, so latency bound; plus no auth. Reuse: server-side content-addressed cache (matches Fugue's content-hashed files).

**Ray Serve** (https://docs.ray.io/en/latest/serve/architecture.html) [V]. Requests queue at the handle and go to a replica chosen by power of two choices; `max_ongoing_requests` per replica gives backpressure. Autoscaler lives in a central Controller. Multiplexing not covered on page.

**vLLM disaggregated prefill** (https://docs.vllm.ai/en/latest/features/disagg_prefill.html) [V]. Experimental; separate prefill and decode instances joined by a KV-transfer connector (NIXL, Mooncake, LMCache...); explicitly does not improve throughput; helps tail inter-token latency. **SGLang**: only secondary descriptions (NVIDIA Dynamo docs mirror) read: RDMA writes KV from prefill into decode GPU memory, bootstrap server coordinates [U]; SGLang's own docs not read. Takeaway: needs RDMA-class links; irrelevant to a tailnet.

**Hivemind** (https://github.com/learning-at-home/hivemind) [V]. MIT. DHT-based decentralized training, decentralized mixture-of-experts, tolerant of unresponsive nodes. Not a dispatcher; Kademlia use not confirmed on README.

### (b) Scheduling theory

**Power of two choices / Sparrow** (https://amplab.cs.berkeley.edu/wp-content/uploads/2013/11/p69-ousterhout.pdf; the fetch of the PDF itself failed on a certificate error, so only search-result summaries were read [U]). Per the summaries: batch sampling places a job's m tasks on the least loaded of d*m sampled workers; late binding places reservations on probed workers and binds a task only when a worker is ready, fixing "queue length is a poor wait predictor" and parallel-scheduler races. Reported: within 14% of ideal [U], late binding 0.55x response time vs batch sampling at 80% load [U]. Tail at Scale (below, [V]) states the Mitzenmacher result: choosing between two servers by queue length "exponentially improves load-balancing performance over a uniform random scheme", and warns that all clients picking the same least-loaded server creates hot spots, so probe two random ones. Fit: with a pool of 2-10 peers, just probe all (cheap, advertisement already carries queue depth); keep late binding because queue length lies for GPU jobs of different sizes. Avoid assuming homogeneous tasks.

**Tail at Scale** (Dean and Barroso 2013, https://www.barroso.org/publications/TheTailAtScale.pdf, read locally) [V]. Hedged request: send to the best replica, then a second after a short delay; cancel the rest on first result. Deferring the hedge until the first has been outstanding longer than the 95th-percentile latency for that request class limits extra load to about 5%. Their example: 1000 BigTable reads over 100 servers, hedge after 10 ms cut 99.9th percentile from 1800 ms to 74 ms for 2% more requests. Tied requests: enqueue on two servers, each tagged with the other; when one starts it cancels the other; window of one network delay where both may start. Hedges can be marked lower priority. Source of variability is often queueing, and after start variability falls. Caution: these are millisecond reads; hedging a 60 s diffusion job costs a whole GPU for the duration, so the hedge budget must be much tighter than 5% and the "p95" must come from per-class measured history, not a fixed number. Tied-request form fits us well: lease on two peers, the first to start cancels the other lease (no GPU work wasted).

**Work stealing** [U, from memory: Blumofe and Leiserson 1999]. Idle workers take work from busy ones. Pull-based job popping (Horde workers, Sparrow late binding) gives the same effect without shared deques. Use for fan-out parts (tiles, batch items): an idle peer asks the originator for the next tile.

**Nomad** (https://developer.hashicorp.com/nomad/docs/concepts/scheduling/scheduling) [V]. Filter infeasible nodes (constraints, drivers, health), rank feasible nodes mostly by bin packing plus affinity and anti-affinity (spread across nodes for failure independence); parallel schedulers run optimistically and the leader's plan queue rejects partially or fully when plans collide, and the scheduler retries. Direct analogue: originators plan optimistically, the peer's admission control is the plan queue, a refused lease means re-plan. **Kubernetes** (https://kubernetes.io/docs/concepts/scheduling-eviction/kube-scheduler/) [V]: filter then score, random tie-break. Scoring strategies (LeastAllocated default, MostAllocated bin-pack, RequestedToCapacityRatio) and ImageLocality (prefer nodes that already have the image) and `percentageOfNodesToScore` were not on that page and are from general knowledge [U]. ImageLocality is the analogue of warm-model affinity.

**BOINC** [partly V]. Evidence is from search summaries and forum/wiki pages, not official reference pages (the official pages I tried returned 404 or certificate errors): the result deadline is `delay_bound` set at job creation, results past the deadline are reissued, replication via `min_quorum`/`target_nresults` with extra instances created on error or disagreement, and `enforce_delay_bound` refuses sending work that the host cannot finish before the deadline [U beyond that]. Reuse: per-part deadline computed at plan time, admission refuses a part it estimates cannot finish by the deadline, reissue on miss. Avoid replication-for-trust (our jobs are pure and peers trusted; only hedge for speed). Licence not verified here (believed LGPL-3, [U]); ideas only.

**Condor ClassAds** (https://htcondor.readthedocs.io/en/latest/classads/classad-mechanism.html) [V]. Job ad and machine ad each have Requirements (constraints that reference the other via TARGET) and Rank (preference). The negotiator finds the best match by Rank among mutually acceptable pairs. Machine policy (START/RANK) lets the owner decide whom to serve and prefer. This is the cleanest model for "my devices only", "refuse a model", "yield to owner": express each peer's policy as a predicate over the job description, evaluated on the peer at lease time. Licence: docs page did not say [U]; do not adopt the ClassAd language, a small struct of fields is enough.

**Leases** [U]. A lease is a grant that expires unless renewed (Gray and Cheriton 1989, from memory). It turns failure detection into timeout: if the originator stops renewing (it died), the peer reclaims the slot; if the peer stops acking, the originator re-dispatches after expiry. Fits "jobs are safe to lose".

### (c) Estimating completion time

Not covered by a single primary source I could fetch; this section is design reasoning plus the sources above. [U unless noted]
- **Measured beats declared.** Maintain per (peer, stage kind, model) an EWMA of seconds per unit (step at a given resolution/batch, per token, per tile) with variance; seed new entries from a declared device class and from the originator's own local speed ratio; AI Horde's `max_power` classes are a usable declared prior [V that they exist].
- **Warm-model affinity.** time(part, peer) = queue_wait + load_cost(model not resident ? size / disk-or-link bandwidth + init : 0) + input_transfer(bytes not already held by hash / link bandwidth) + compute(measured rate x work) + output_transfer. K8s ImageLocality [U on details] is the analogue; llama.cpp RPC's server cache [V] shows content-addressed caching pays off on repeat.
- **Model load costs** must be measured too (first load vs reload from page cache differ a lot); store as separate observation.
- **Uncertainty.** Plan on a pessimistic quantile (say p80 of the measured distribution), hedge on the p95; Tail at Scale [V] ties hedge delay to a percentile of the same class.
- Observed speed also drifts with thermal throttling and owner activity; decay observations.

## Recommended dispatcher design sketch (about 300 words)

**Inputs.** Compiled graph cut into parts at cheap boundaries; per peer advertisement (timestamped, TTL): free VRAM/RAM, queue depth and running leases, warm model hashes, on-disk hashes, held content hashes, EWMA rate per (stage kind, model), link RTT/bandwidth to originator, owner policy (cap, refused models, yield flag), protocol version.

**Filter (peer-side too).** Drop peers failing version, node-installed (id, version, hash), VRAM fit (spec 8.10), policy predicate (Condor style), stale advertisement.

**Score.** For each part, est = queue_wait + load_cold + transfer_missing + compute_p80 + return_transfer; pick min est over feasible peers plus local; add penalty for peers recently slow/failed; prefer warm even if raw speed is lower (falls out of the formula). Independent parts (tiles, batch items): late binding, i.e. peers pull next item when free, bounded by lease. Chains stay where their data is.

**Lease protocol.** `RESERVE(part, vram, est_duration, deadline)` to the top 1-2 candidates; peer admission checks all originators' live leases against capacity and policy, replies GRANT(lease_id, ttl) or REFUSE(reason, retry_after). Originator sends job under the lease; renews at ttl/3 with progress; peer releases on completion, cancel or missed renewals. Refused or expired: next candidate, count against retry budget.

**Hedging rule.** Tied-request style: obtain a second lease on the next-best peer when the part is short (est below a threshold) or when progress after 1.0x est is below the measured p95 rate; start the duplicate only when the second peer is idle; first result wins, the loser's lease is cancelled; at most one hedge per part; hedges never take a slot an unhedged part wants.

**Redispatch rule.** Trigger on lease expiry, peer dead (membership), explicit fault, or progress rate below 0.3x estimate for two heartbeats. Redispatch remaining work from the last kept intermediate to next-best peer; exponential back-off; budget of 2 retries per part, then local; the run never loops and reports a notice each time.

## What to test with a simulator first

1. Placement quality vs a brute-force optimum on random pools (2-8 peers, speeds spread 1:6, mixed warm/cold).
2. Estimate error sensitivity: how bad are plans if declared speed is wrong by 2x, and how fast does the EWMA correct.
3. Warm-vs-fast tradeoff: model load size vs speed ratio crossover.
4. Several originators at once: oversubscription impossible (property test), and throughput/fairness versus no leases; livelock of refuse-retry; stale-advertisement herding (all pick the same peer).
5. Hedge threshold: extra GPU-seconds vs p99 improvement for job durations 5 s - 5 min with heavy-tail slowdowns; tied vs deferred hedge.
6. Slow peer, killed peer, partitioned peer (can talk to peer but not originator); time to redispatch vs deadline; duplicate completion races.
7. Lease TTL and renewal interval vs detection time and false expiry under jitter.
8. Pull-based tile fan-out vs static partition when one peer is 4x slower.
9. Owner preemption: local run arrives mid-job; drain vs kill cost.
10. Retry budget exhaustion and fallback-to-local path.

## Open questions

- Granularity of "progress": do stable-diffusion.cpp steps give a heartbeat cheaply enough for rate-based slowness detection? (Fugue-side question, not researched here.)
- How much do model load times vary (cold disk vs page cache) on the target machines? Needs measuring.
- AI Horde's actual requeue timeouts and kudos formula; fetch the Horde docs site and ADRs (https://haidra-org.github.io/AI-Horde/decisions/) before finalising the reciprocal policy.
- SGLang's own disaggregation docs, Sparrow PDF, BOINC official docs, llama.cpp / Ray / Nomad licence files: not read directly; marked [U].
- Petals routing and fault tolerance details live in its papers, not read.
- Exact licences of ComfyUI_Cluster, NetDist, Raylight not checked; all are irrelevant unless code is reused, which this design does not need.

---

# Fugue mesh (epic #203): trust, identity, security, accounting

Research date 2026-11-22. Claims tagged **[V]** were read from a primary source this session (URL in Details). **[U]** = unverified (from memory or inference; check before relying). Fugue facts come from `~/github/fugue/docs/spec.md` and epic #203.

## Summary and recommendation

Fugue already has most of v1's trust story in pieces: a paired host is an `fugue-helper --serve-http` endpoint behind a bearer key in `providers.json` (spec 8.2, 8.7), models travel as `{name, sha256, size}` handles that the host resolves only in its own `--models` folders (8.7), and packs need an explicit per-id+version trust prompt with no sandbox in v0.1 (6.7). The epic's rule "ship the graph, never the code" fits that. What is missing is a real peer identity (a bearer key says "someone with the key", not "which machine"), a size/rate budget on what a peer returns, and a signed usage record.

Recommended v1 minimum:

1. **Identity: a per-install Ed25519 keypair; peer ID = SHA-256 of the public key** (Syncthing model [V]). Pair explicitly by comparing/pasting the ID (trust on first use with a human-confirmed fingerprint, not silent TOFU). Run the job channel over mutual TLS or a Noise-style handshake keyed by that key. Treat the tailnet (WireGuard + ACLs) as a network gate only, a second layer, never the sole identity: "on my tailnet" can mean a shared or tagged device the owner did not intend.
2. **Authorization per peer, stored locally** (like `trust.json`): peer ID -> {mode: `mine` | `friend` | `none`, caps}. `mine` = the owner's own devices (may skip prompts); `friend` = strict.
3. **Peer executes only**: built-in nodes, plus packs already installed and trusted locally with identical id+version+content hash; model handles resolved only from its own folders by hash; no paths, no URLs, no code in a job. Anything else is refused with a reason, never installed.
4. **Hard budgets on the executor and the originator**: max job input bytes, max output bytes and files, max pixels/frames/duration, wall-clock and VRAM lease, concurrency per peer, a rate limit per peer. Originator treats every returned file as hostile (see below).
5. **Accounting: record always, enforce lightly.** Signed bilateral usage records, a local append-only ledger, policies `free` / `none` / `reciprocal` with a deficit cap. No global currency. Among friends a bounded deficit cap is the whole double-spend defence.
6. **Privacy: say plainly that the executing peer sees the prompt, the inputs and the outputs.** Strict mode: only `mine` or explicitly trusted peers get a job; the peer keeps no job data after settle (a promise, not enforced).
7. **Isolation of packs is later**, but design the pack runner so bubblewrap/Landlock can wrap it (run folder only, no network unless declared).

## Threat model

| # | Threat | Who | Mitigation | Cost |
|---|---|---|---|---|
| T1 | Impersonation of a peer (spoofed name/IP, rogue device on tailnet) | Anyone on LAN/tailnet | Ed25519 peer ID checked in handshake; pairing by fingerprint; tailnet ACL/tags as outer layer; mDNS/gossip only *suggests* peers, never authorizes | Low; one handshake lib |
| T2 | Stolen bearer key / pairing key reuse | Attacker with a copied key file | Per-peer keys bound to the peer ID, revocable per peer; key file 0600 (already) | Low |
| T3 | Sender ships malicious node code | Malicious originator | Never ship code; run only installed id+version+hash node, locally trusted | None (already a rule) |
| T4 | Sender graph abuses a trusted node (path traversal, arbitrary file read, `network` permission, command args) | Malicious originator | Executor re-validates the graph itself (`compileForRun` on the peer), rejects absolute paths, resolves only handles in its folders (spec 8.7 already does), strips/denies nodes with `network` or `files` permission for non-`mine` peers | Low-medium |
| T5 | Resource abuse: huge image/latent, huge step count, endless job, VRAM hog, flood of jobs | Malicious or buggy originator | Caps on input bytes, pixels, frames, steps, wall-clock; leases that expire; per-peer concurrency and rate limit; owner-local job preempts pool jobs | Low |
| T6 | Malicious outputs: decompression bomb, giant file, crafted PNG/WebP/JPEG/video exploiting a decoder in the originator | Malicious or compromised executor | Cap output size and dimensions before decoding; only accept allow-listed formats; decode in a separate low-privilege process or Landlock/bwrap sandbox; keep decoders patched; prefer raw latents/tensors with known size over images; verify declared hashes/dims. Precedent: libwebp CVE-2023-4863 heap overflow exploited in the wild [V] | Medium (decoder sandbox) |
| T7 | Wrong or poisoned results (garbage image, backdoored model output) | Dishonest executor | Pure jobs + seed: spot re-run locally or on a second peer and compare (diffusion is not bit-identical across GPUs, so compare loosely [U]); trust only friends | Medium |
| T8 | Privacy: executor reads prompts/inputs/outputs, logs them | Curious/malicious executor | Only trusted pools; cut the graph so sensitive inputs stay local; no-log is an honour promise, not enforceable | Low, but a limit to document |
| T9 | Traffic observation/tamper on the wire | Network | Encrypted channel (TLS 1.3 or Noise, WireGuard underneath) | Low |
| T10 | Accounting fraud: executor inflates usage; originator denies a record; replay of an old record | Either party | Both sign the same canonical record; mismatch = dispute, not payment; unique job id + nonce; deficit cap bounds loss | Low |
| T11 | Double spend / balance forked between two partners | Dishonest peer talking to many | Unavoidable without a global ledger [V for the epic's statement; reasoning is mine]; bound by per-partner deficit cap; acceptable among friends | None |
| T12 | Sybil: attacker makes many identities to farm free capacity | Stranger | Unknown peers get zero or tiny credit; new IDs start at the lowest cap; pairing needs a human | Low |
| T13 | Membership/gossip poisoning (fake peer lists, fake advertisements: lying about VRAM/models) | Rogue member | Advertisements signed by the peer ID, expiring; measured speed beats declared; originator verifies by lease and observation | Low-medium |
| T14 | Peer's installed pack is itself malicious | Executor's owner | The executor's owner trusts packs on their own machine (6.7); the originator cannot see it. Advertise pack hash so the originator can require a known-good hash | Low |

## Accounting options

| Option | How it works | Pros | Cons / weaknesses | Fit |
|---|---|---|---|---|
| none (record only) | Log signed records, no limits | Trivial; protocol carries data | No protection | Default for `mine` |
| free | Own devices | Nothing to do | Only own machines | `mine` |
| reciprocal, per partner (tit-for-tat) | Balance per partner = given minus taken, deficit cap, optimistic grace for newcomers | No third party; BitTorrent precedent | BitTorrent is *rate-based and short-memory*, not a ledger; known free-riding and Sybil issues [U for the specific literature]; a partner with nothing to give never earns | Recommended v1 enforce mode |
| mutual credit (zero-sum, credit limits) | Balances start at zero, sum to zero, each member capped [V] | Fits bilateral ledgers; cap is the risk limit | Needs agreed unit (GPU-seconds by device class) | Same as reciprocal with a named unit |
| Kudos (AI Horde) | Central server ledger; earned by serving, spent for queue priority, not buyable, event-sourced with reservations [V] | Simple UX: priority, not blocking | Needs a central server; Fugue has none | Plugin only |
| Deal and escrow (Akash, Golem, Filecoin) | Signed agreement, pre-funded escrow, payment per time/usage, provider issues debit notes/invoice, requestor accepts [V Golem, Akash]; Filecoin deals signed by both with collateral and proofs [U, source not reachable] | Good lessons on keep-alive and incremental settlement | Needs a chain/token | Learn the shapes, adopt none |

Lessons worth copying without tokens:
- Golem: the *provider* meters and issues; the *requestor* accepts; short keep-alive debit notes show liveness and let the requestor compare cost to its own estimate; one final invoice on end for any reason [V]. Maps to: periodic progress heartbeat with accumulated usage, final signed record.
- Akash: a deposit is drawn down per block and the lease closes when it runs out [V (search summary; docs pinned to an old version)]. Maps to: a lease with a budget; stop when the deficit cap is reached.
- Kudos: reward = serving; spending buys queue priority, not access [V]. Maps to: `admit()` returns `queue behind others` rather than refuse.
- Rate limits and deficit caps are the only enforceable controls without a global ledger.

## Recommended record and plugin interface

### Signed usage record (canonical JSON, RFC 8785 JCS [U], or CBOR deterministic)

```json
{
  "v": 1,
  "job": "uuid",
  "part": "subgraph path id or part number",
  "nonce": "random 16 bytes hex",
  "requester": "peerid:<sha256 of pubkey>",
  "executor": "peerid:<...>",
  "graph_hash": "sha256 of the canonical subgraph + input hashes + seed",
  "nodes": [{"id": "fugue/ksampler", "version": "1.2", "hash": "sha256:..."}],
  "model_hashes": ["sha256:..."],
  "device_class": "gb10|rtx3090|...",
  "gpu_seconds": 41.2,
  "peak_vram_mb": 18200,
  "bytes_in": 1048576,
  "bytes_out": 25165824,
  "start": "2026-11-22T10:00:00Z",
  "end": "2026-11-22T10:00:41Z",
  "status": "ok|failed|cancelled|timeout",
  "output_hashes": ["sha256:..."],
  "lease": "lease id",
  "prev": "sha256 of the executor's previous record for this partner"
}
```

- Executor signs (`sig_exec`, Ed25519 over the canonical bytes; the 64-byte signature, deterministic and non-malleable per RFC 8032 [V]); the requester verifies `output_hashes` and its own observed bytes and time, then counter-signs the *same bytes* (`sig_req`). Disagreement beyond a tolerance (say 5% on seconds, exact on hashes and bytes) is stored as `disputed`, and does not settle.
- `prev` chains each side's per-partner log so a missing or altered entry is visible to the partner (tamper-evident, not tamper-proof) [design suggestion, not from a source].
- Use a domain-separation prefix inside the signed bytes (`"fugue-usage-v1\n"`), since RFC 8032 advises a protocol-defined constant for contexts [V].
- Device class matters: a unit is `gpu_seconds * weight(device_class)`; weights are a local policy table, not in the record.

### Plugin interface (quote / admit / settle), refined

Command protocol like a node (JSON in, JSON out, timeout, fallback to the configured default policy).

```
quote(job_estimate, peer)  -> { cost: number, unit: "gs-eq", expires: ts, note }
   job_estimate: {gpu_seconds_est, vram_mb, bytes_in_est, bytes_out_est, device_class, model_hashes, nodes}
   called by the dispatcher (originator side) to rank peers; pure, no side effects

admit(job_estimate, requester, state) -> { decision: "accept"|"queue"|"refuse", priority: int, reason, max_cost, max_seconds }
   called by the executor's admission control, with state = {balance_with_requester, deficit_cap, open_leases, recent_rate}
   a refuse makes the dispatcher try another peer; queue lowers priority

settle(record_signed_by_both) -> { balance_with_peer: number, ok: bool }
   called by both sides after the job; idempotent on (job, part, nonce); a disputed record is passed with status "disputed"
```

Additions to the epic's three calls:
- `max_cost` / `max_seconds` returned by `admit` become lease limits (the executor stops at them).
- `settle` must be idempotent, and a crash or timeout of the plugin falls back to the default (`none` or `reciprocal`) as the epic's test says.
- Built-in `reciprocal`: balance_peer += units served minus units consumed; refuse when `balance - cost < -deficit_cap`; newcomers get a small grant (BitTorrent's optimistic unchoke is the analogue [V: the paper describes one optimistic unchoke regardless of rate]); decay or forgive old debt on a schedule if wanted.

## Details

### 1. Identity and pairing

- **Fugue today.** Paired host = `fugue-helper --serve-http` run by the host owner, `providers.json` entry `{name, type, address, key}`, a refused key (401/403) is `provider_unreachable` (spec 8.7). Omarchy pairing is `omarchy-agent-local pair <host>` writing `remotes/<host>.key`, a bearer key (spec 8.7). Packs: trust per pack id + version in `trust.json`, "no sandbox in v0.1", optional bubblewrap/Landlock later (spec 6.7). There is no peer identity beyond the key and the address.
- **Tailscale.** ACLs: every rule has a source and destination, directional, enforced on each device from a distributed policy; deny by default, except that a policy file with no `acls` section applies allow-all, which is what a new tailnet starts with [V, https://tailscale.com/kb/1018/acls]. Tags: defined in `tagOwners`; a device has either a user identity or a tag identity, never both; tagged devices have no user [V, https://tailscale.com/kb/1068/tags]. Serve passes `Tailscale-User-Login` headers, but not for tagged devices, only if the backend listens on localhost [V, https://tailscale.com/kb/1312/serve]. Consequence: a tag-based mesh cannot get a user identity from Tailscale, so Fugue needs its own peer key. The WhoIs local API (to map a connection IP to a node/user) is **[U]**, the page I fetched did not cover it.
- **WireGuard.** Identity is the Curve25519 static public key, authenticated by static-key DH; the server stays silent to unknown keys; replay is blocked with a TAI64N timestamp [V, https://www.wireguard.com/protocol/]. Good as a network layer; the app cannot easily read which key a connection used, so do not rely on it for application identity [U on tooling].
- **Syncthing device IDs.** Ed25519 keypair at first start; ID = SHA-256 of the DER certificate, base32, grouped with check digits; after TLS each side computes the remote ID and drops the connection if it is not in its configuration. Local discovery is unauthenticated, so any device can announce any ID, which is harmless because the TLS check follows [V, https://docs.syncthing.net/dev/device-ids.html]. This is the closest model to copy.
- **SSH TOFU.** `StrictHostKeyChecking accept-new` records a new host key and refuses a changed one; `ask` (the default) prompts; `yes` never adds [V, https://man.openbsd.org/ssh_config.5]. For Fugue: ask on first contact and show the fingerprint (and let the user confirm out-of-band); refuse a changed key with a clear "peer key changed" error.
- **Ed25519 signed messages.** 32-byte keys, 64-byte deterministic signatures, malleability prevented by the S < L check [V, https://www.rfc-editor.org/rfc/rfc8032]. Qt: a library with Ed25519 (libsodium, OpenSSL 3, or Qt's own TLS with Ed25519 certs) is needed; which Fugue already links is **[U]**.

### 2. Remote execution risks

- Executor risks: arbitrary graph input to nodes that read files, run commands, or touch the network. A trusted command node is "a program running as the user" (spec 6.7). So the executor must re-validate the graph and refuse non-handle paths (already done for models, spec 8.7), and for non-owner peers refuse nodes declaring `network` or `files` (the permissions of spec 6.3).
- Originator risks: whatever a peer returns is parsed by the originator. Decoder bugs are real: libwebp CVE-2023-4863 was a heap overflow in image decoding exploited in the wild, fixed Sep 2023 [V via search results: https://www.tenable.com/cve/CVE-2023-4863, https://www.csa.gov.sg/alerts-and-advisories/alerts/al-2023-118/; I did not read the NVD record itself, sources disagree on whether it names Chrome or libwebp]. Mitigate: cap size/dimensions/frames *before* decoding, allow a short list of formats, decode in a child with Landlock/bwrap, keep libs current. Spec 8.7 says helper results come back as base64/`GET /fugue/v1/result`: put a hard cap on that stream and verify the hash the executor claims.
- Sandboxes. bubblewrap: unprivileged, builds a mount namespace on an invisible tmpfs, sets no_new_privs; it is not a policy by itself, protection depends on the arguments; mounted sockets (D-Bus) can be escape routes; `--new-session` needed for TIOCSTI [V, https://github.com/containers/bubblewrap]. Landlock: unprivileged self-restriction since Linux 5.13, TCP bind/connect allow-lists from ABI 4, UDP from ABI 10, rules only tighten and are inherited by children [V, https://docs.kernel.org/userspace-api/landlock.html]. Fit: Landlock for "this pack may read the run folder and nothing else, no network unless declared" without a helper binary; bwrap when a private root is wanted. GPU access inside a sandbox (device nodes, Vulkan drivers) may complicate both **[U]**.
- Hash-addressed everything: inputs by SHA-256 (a peer that has them is not sent them again, per the epic), nodes by id+version+hash, models by hash. This also gives free integrity.

### 3. Accounting

- **BitTorrent.** Choking is a variant of tit-for-tat: a peer uploads to the peers that upload to it fastest, with periodic optimistic unchokes of one random peer regardless of rate, so newcomers can start; four unchoked by default; a peer that gives nothing for a while is "snubbed" [V, Cohen, https://www.bittorrent.org/bittorrentecon.pdf, read via pdftotext]. Weaknesses: the paper's mechanism is rate-based with short memory, so it is not a ledger and gives no long-term accounting; known exploits (large-view, Sybil, seeding-for-credit) are in later literature **[U, not fetched]**. Lesson: tit-for-tat works with small, symmetric, frequent exchanges; GPU jobs are large and asymmetric (one machine may only ever consume), so a balance plus a cap fits better.
- **Mutual credit.** Zero start, balance equals net dealings, each member has a limit [V, https://en.wikipedia.org/wiki/Mutual_credit; secondary source]. A bilateral ledger between two Fugue peers is this with only two members.
- **AI Horde kudos.** Central, event-sourced, reservation-based, not buyable, priority not guarantee, trading prohibited by ToS [V, https://github.com/Haidra-Org/AI-Horde/blob/main/README.md]. A central operator is the price of that model.
- **Golem / Akash / Filecoin.** See table. Golem payments: provider issues keep-alive and payable debit notes plus a final invoice, requestor accepts and pays [V, https://docs.golem.network/docs/golem/payments]; agreements are signed after demand/offer match [V, https://docs.golem.network/docs/golem/overview]. Akash: deposit drawn per block, provider withdraws, lease can close when empty [V via search summary of https://akash.network/docs/getting-started/intro-to-akash/payments, https://akash.network/docs/other-resources/marketplace]. Filecoin: **[U]** (spec pages returned 404; from memory, deals are signed proposals with collateral and storage proofs).
- **Double spend without a global ledger.** Two partners can each owe the same credit to a third because nobody sees both balances. Fix options: a central ledger (rejected by the epic), or accept it and bound the loss with a per-partner cap. Among friends, a cap of a few GPU-hours per partner plus the social cost of cheating is enough. Rate limits (jobs per minute, concurrent leases) bound the damage window.

### 4. Privacy

- The executor sees: the prompt text, negative prompt, seed, parameters, input images/latents/masks, the model and LoRA hashes (reveal tastes), outputs, timing and size. A latent or conditioning tensor is not a prompt but can be approximately decoded or inverted **[U]**, so do not treat "we only sent the embedding" as private.
- Options: (a) trusted pool only (mine/friend); (b) cut the graph so prompt encoding and VAE decode stay local and only latents cross (also cheaper to move, per the epic), a weak privacy gain; (c) a "no log" promise: executor deletes job folder and does not write prompts to logs; unenforceable, so show as a stated policy in the peer's advertisement; (d) confidential computing is out of scope. Show the user where each part ran (the epic's UI) and a "stays local" pin per node.

## Open questions

1. Which crypto library does Fugue already link (OpenSSL via Qt, libsodium)? Pick the Ed25519 provider from that. [U]
2. Channel: mutual TLS with self-signed Ed25519 certs (Syncthing style, works with Qt Network) versus a custom Noise handshake. Recommendation: mutual TLS, since the helper already speaks HTTP.
3. Do paired Omarchy hosts keep the bearer key (spec 8.7) and gain a peer ID later, or does the mesh replace it? Suggest: key remains for the omarchy provider, the mesh uses its own identity.
4. Is a result from a different GPU model close enough to compare for spot-checks (T7)? Needs an experiment.
5. Default deficit cap and the weights per device class: owner-set; ship a table in settings.
6. How should a peer advertise its own packs' hashes without leaking an inventory to strangers? (Advertise only to paired peers.)
7. Does a bwrap/Landlock wrapper work with Vulkan/CUDA device access on this setup (Omarchy + GB10 + 3090)? [U]
8. Dispute handling UX: who resolves a mismatch, and does a repeated mismatch auto-demote a peer to `none`?

## Source URLs read

- https://tailscale.com/kb/1018/acls ; https://tailscale.com/kb/1068/tags ; https://tailscale.com/kb/1312/serve
- https://www.wireguard.com/protocol/
- https://docs.syncthing.net/dev/device-ids.html
- https://man.openbsd.org/ssh_config.5
- https://www.rfc-editor.org/rfc/rfc8032 (first 100000 of 117288 chars)
- https://github.com/containers/bubblewrap/blob/main/README.md ; https://docs.kernel.org/userspace-api/landlock.html
- https://www.bittorrent.org/bittorrentecon.pdf
- https://github.com/Haidra-Org/AI-Horde/blob/main/README.md
- https://docs.golem.network/docs/golem/overview ; https://docs.golem.network/docs/golem/payments
- Akash docs via search results (page fetches 404): https://akash.network/docs/getting-started/intro-to-akash/payments ; https://akash.network/docs/other-resources/marketplace
- https://en.wikipedia.org/wiki/Mutual_credit
- libwebp CVE via search snippets only: https://www.tenable.com/cve/CVE-2023-4863 ; https://www.csa.gov.sg/alerts-and-advisories/alerts/al-2023-118/
- Not reached: Filecoin spec, SWIM/memberlist (not part of this topic), Tailscale WhoIs API.
