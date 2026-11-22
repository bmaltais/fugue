# Private mode research (epic #204)

Research notes for private mode and profiles: one root for everything Fugue writes, vault backends, sandboxing packs, and the audit of write paths. Written 2026-11-22 by research agents from primary sources and a read of this repo; claims marked **[U]** or "unverified" could not be checked against a source and must be re-checked before they drive a decision. The decisions themselves are in issue #204.

1. [Write paths and the single root](#fugue-epic-204-path-audit-and-prior-art-2026-11-22)
2. [Vault backends](#vault-backends-for-fugue-private-mode-epic-bmaltaisfugue204)
3. [Sandboxing packs and ffmpeg](#sandboxing-pack-nodes-and-ffmpeg-in-fugue-private-mode-epic-204)

# Fugue epic #204: path audit and prior art (2026-11-22)

Code read at ~/github/fugue HEAD 7b2d26b2 (read-only). `[U]` = unverified. "Sens." = can hold user prompts, images or outputs.
Key finding: almost all of Fugue's own writes already funnel through `src/engine/paths.cpp` (`configDir/stateDir/dataDir/cacheDir`, lines 13-22, built on QStandardPaths, which honors XDG_*; spec 1.4 says "XDG variables are honored"). The leaks are the exceptions listed below.

## Part A: audit

Redirect column: **XDG** = fixed by setting XDG_CONFIG/DATA/STATE/CACHE_HOME (+XDG_RUNTIME_DIR); **TMP** = TMPDIR; **ROOT** = code change to read FUGUE_HOME; **SET** = a user setting (settings.json) whose default must change; **ENV** = child must be given a scrubbed env.

| # | Path (default) | Holds | Sens. | Code (file:line) | Redirect |
|---|---|---|---|---|---|
| 1 | `~/.config/fugue/settings.json` | output/inputs/model/pack folders, language | low (paths reveal project names) | paths.cpp:19, :104 settings.cpp | XDG |
| 2 | `~/.config/fugue/providers.json` (0600) | provider hosts and API keys | secrets | providers.cpp:21 (hard-coded `~/.config/fugue/providers.json` string), :195 uses configDir | XDG; fix the :21 constant and the hint strings (omarchy_provider.cpp:430, :642) **[step 1 done: `providersFilePath()` in engine/paths]** |
| 3 | `~/.config/fugue/keybindings.json` | keymap | no | command_registry.cpp:241 | XDG |
| 4 | `~/.local/state/fugue/recent-files.json` | recent workflow paths (names leak) | yes (names) | main.cpp:149; recent_files.cpp | XDG; private mode: do not construct |
| 5 | `.../state/fugue/recovery/` | autosaved copies of open workflows (prompts, image paths) | **yes** | recovery_store.cpp:17 | XDG |
| 6 | `.../state/fugue/chrome.json, run-bar.json, side-panels.json, recent-colors.json` | UI layout | no | main.cpp:132-145 | XDG |
| 7 | `.../state/fugue/hashes.json` | file path+size+mtime to sha256 (path list of every input and model) | yes (paths, hashes prove a file was seen) | file_hashes.cpp:93 | XDG |
| 8 | `.../state/fugue/trust.json` | allowed packs | no | trust_store.cpp:20 | XDG |
| 9 | `.../state/fugue/queue/`, `engine.lock`, `gc.stamp` | run queue entries (run ids), GC stamp | low | queue.cpp:84-85, cli/process.cpp:18 | XDG. Note the queue and lock are shared by design across processes: a private session needs its own queue or it serializes with the ordinary one. |
| 10 | `.../state/fugue/cuda-helper-notice-dismissed` | flag | no | helper_provider.cpp:95-98 | XDG |
| 11 | `.../state/fugue/model-hashes.json` | helper's model hash index (written by the **helper**) | yes (paths) | helper/src/main.cpp:1794-1800 reads XDG_STATE_HOME else `$HOME/.local/state` itself | XDG, but only if the child inherits it (it does, no env set at helper_provider.cpp:~269 start) |
| 12 | `~/.cache/fugue/results/` | the result cache: every node output (images, text, latents) | **yes, the main one** | result_cache.cpp:121 | XDG (cacheDir) |
| 13 | `~/.cache/fugue/runs/<id>/` | run.json (workflow path), compiled graph with prompts, events.jsonl, node work dirs | **yes** | run_folder.cpp:96 | XDG |
| 14 | `~/.cache/fugue/thumbnails/` | thumbnails of outputs | **yes** | paths.cpp:23, run_service.cpp:139 | XDG |
| 15 | `~/.cache/fugue/thumbs/` (second, different thumbnail dir) | media library thumbnails | **yes** | thumbnail_cache.cpp:12-14 (calls QStandardPaths directly), media_library.cpp:47 | XDG, but should call `cacheDir()`; also the cache manager (cache_manager.cpp:146-148) does not know this dir, so it is never evicted or cleared [bug candidate] **[step 1 done: `mediaThumbnailsDir()`; the cache manager still does not know the folder, open]** |
| 16 | `~/.cache/fugue/package-XXXXXX` | unpacked share packages (.fugue-package) while importing | **yes** | workflows.cpp:462-463 | XDG |
| 17 | `~/.cache/fugue/qmlcache/*.qmlc` | Qt compiled-QML disk cache (observed on this machine) | no (code only) | none in Fugue; Qt writes to QStandardPaths::CacheLocation = `~/.cache/<applicationName>` | XDG_CACHE_HOME, or `QML_DISK_CACHE_PATH` (Qt docs) |
| 18 | `~/.cache/fugue/qtpipelinecache-*` and `~/.cache/qtshadercache-*` (both observed) | Qt RHI pipeline/shader caches | no (shader binaries) | Qt internal | XDG_CACHE_HOME [U for exact env names; Qt docs say only that the cache needs a writable QStandardPaths cache location] |
| 19 | `~/.cache/QtProject/Qml Runtime` (observed) | qml tool artifacts | no | Qt | XDG_CACHE_HOME |
| 20 | `~/.local/share/fugue/packs/<id>/` | installed packs (code, own data) | pack data may be sens. | pack_loader.cpp:349 | XDG; for private-pack design see below |
| 21 | `~/.local/share/fugue/providers/` | provider modules | no | provider_module.cpp:38 | XDG |
| 22 | `~/.local/share/fugue/models/` (+ `model_paths`, `download_folder`) | models; LoRAs trained on private data are sens. | LoRAs yes | model_library.cpp:75, :84 | XDG default; **SET** for model_paths and download_folder (user-chosen, absolute) |
| 23 | `~/.local/share/fugue/prompts/`, `catalog/` | user prompt templates and model catalog entries | **prompts yes** | prompt_templates.cpp:102, model_catalog.cpp:141 | XDG |
| 24 | `~/Pictures/Fugue/` (XDG_PICTURES_DIR, else QStandardPaths Pictures) | saved results from Save nodes | **yes** | paths.cpp:25-35; save_files.cpp:39 | **SET** default must become `$FUGUE_HOME/outputs`; XDG_PICTURES_DIR is read by paths.cpp:26 but Qt's PicturesLocation comes from `user-dirs.dirs`, not an env var [U] **[step 1 done: default is `$FUGUE_HOME/outputs`]** |
| 25 | `~/Pictures/Fugue/inputs/` (or `inputs_folder`) | pasted or dropped images, content-hash named | **yes** | paths.cpp:37-41 | follows the output folder; same fix as 24 **[step 1 done: follows the output folder]** |
| 26 | `<workflow dir>/inputs` | per-workflow inputs beside the saved .fugue | **yes** | paths.cpp:43; image_inputs.cpp | beside user-chosen workflow file: not redirectable by env; it is the user's own location, but the guard must treat it as a workflow-owned path |
| 27 | `~/Documents/Fugue/` and `FileRoots.write` | suggested workflow folder; allowed write root for nodes | yes | file_roots.cpp:102, workflows.cpp:228-230, templates.cpp:163-164 | ROOT: hard-coded `QDir::homePath() + "/Documents/Fugue"` at file_roots.cpp:102; templates dir at templates.cpp:163 (override: `FUGUE_TEMPLATES`, :169) **[step 1 done: `documentsDir()` gives `$FUGUE_HOME/documents`; templates and the suggested folder follow]** |
| 28 | FileRoots read root = whole `$HOME` | nodes may read any file under home | n/a (read scope) | file_roots.cpp:99 | ROOT: in private mode shrink to vault + inherited read roots |
| 29 | `$XDG_RUNTIME_DIR/fugue-view/<hash>.<ext>` (0700) | copies of images handed to an external viewer (Open) | **yes**, and the file persists until logout/reboot | image_actions.cpp:77-84, :129 | XDG_RUNTIME_DIR; code must not write it at all in private mode, or put it in the vault tmp and wipe on close **[step 1 done: `runtimeDir()`; under a root it is `$FUGUE_HOME/run`; private mode still must not write it, open]** |
| 30 | ffmpeg log temp file `/tmp/QT-*` (`QTemporaryFile`, autoRemove(false)) | ffmpeg stderr (can include input paths) | low-medium | video_frames.cpp:54-58 | TMP (QDir::tempPath honors TMPDIR on Unix) |
| 31 | ffmpeg/ffprobe children (outputs go to the node work dir) | media | yes | ffmpeg.cpp:20-48; save_media.cpp, media_video.cpp | children inherit env (no setProcessEnvironment): TMPDIR and XDG are enough; ffmpeg has no own cache. |
| 32 | `/tmp/fugue-helper-XXXXXX` | the C++ helper's work dir (latents, decoded images); removed on SIGTERM | **yes** | helper/src/main.cpp:1777, :1785-1792 | TMP (`fs::temp_directory_path` reads TMPDIR); not removed on SIGKILL/crash |
| 33 | `fugue authoring` scratch `QTemporaryDir` | pack check hashes | low | cli/authoring.cpp:299 | TMP |
| 34 | `~/.cache/mesa_shader_cache`, `~/.cache/nvidia`, `~/.nv/ComputeCache`, `~/.cache/fontconfig` (observed on this machine) | GPU driver shader/compute caches, font cache; written by Fugue and the helper but owned by drivers | no (compiled kernels; may reveal model architecture) | none | `XDG_CACHE_HOME` redirects Mesa and fontconfig; NVIDIA uses `__GL_SHADER_DISK_CACHE_PATH` and `CUDA_CACHE_PATH` [U: names from NVIDIA docs, not fetched]. Residual: `~/.nv` is not XDG. Accept or point HOME. |
| 35 | Omarchy theme, language files (read) | `~/.local/state/omarchy/current`, `~/.config/omarchy/shell.toml`, `language`, `agent-local` | no, read-only | theme.cpp:42,44,82; translator.cpp:15; omarchy_provider.cpp:373 | **reads from `$HOME`, hard-coded**: if XDG is redirected they still work (good, they are inherited read-only); if HOME is redirected they break. So redirect XDG, not HOME. |
| 36 | Skill install `~/.claude/skills`, `~/.agents/skills` | `fugue skill install` | no | cli/skill.cpp:100-102 | user-requested write, not a session artifact; the guard test never runs it |
| 37 | Window title | workflow name in `<name> — Fugue` | yes (name) | workflows.cpp:220-224 | code: private mode returns "Fugue". Compositor, taskbar and screenshots still see the title. |
| 38 | Clipboard | copied nodes (canvas_commands.cpp:24), copied images (image_actions.cpp:196-197), workflow text | yes | | no env fix; Qt writes to the display server. A clipboard manager (cliphist, klipper) stores it outside Fugue. Out of scope, state in UI. |
| 39 | Drag and drop | DropArea in `WorkflowDialogs.qml:87`; reads URLs, then `storeImageFile` into inputs (image_inputs.cpp:423) | yes | | reads only; writes land in inputs (25) |
| 40 | File dialogs / portals | QML `FileDialog` (WorkflowDialogs.qml:27-65, ImageSaveDialog.qml:13, WidgetPicker.qml:148). The xdg-desktop-portal backend (GTK/KDE) keeps its own last-folder and `~/.local/share/recently-used.xbel` entries | names of every opened/saved file | | **cannot be redirected by Fugue**: the portal runs in another process. Mitigation: private mode uses `FileDialog` with Qt's non-native fallback (`QT_QPA_PLATFORMTHEME`/`QT_QUICK_DIALOGS`... [U]) or states the limit. |
| 41 | `QDesktopServices::openUrl` / external viewer | opens files in other apps, which keep their own recents | yes | image_actions.cpp, main.cpp:119 | out of scope, state in UI |
| 42 | QSettings / QtWebEngine | **none**: `grep QSettings|WebEngine` over src returns nothing | | | n/a (good). `QtProject.conf` is a Widgets/QFileDialog thing and is not used. |
| 43 | Logs | **no log files**. stderr only (`QLoggingCategory` filter, main.cpp:66). Under a launcher it goes to the journal. | stderr may carry prompts and paths from node stderr (docs/spec.md:1019 "stderr is the node's log") | | cannot redirect the journal; in private mode do not forward node stderr to the app's stderr, or send it to a vault file |
| 44 | Crash dumps | no crash handler in Fugue (grep). systemd-coredump writes `/var/lib/systemd/coredump` | **yes** (memory) | | `setrlimit(RLIMIT_CORE,0)` and `prctl(PR_SET_DUMPABLE,0)` in private mode; code change |
| 45 | Swap, GPU memory | | yes | | out of scope, state in UI (as the epic already says) |
| 46 | Pack nodes | arbitrary: `command_node.cpp:766-769` passes `systemEnvironment()` plus FUGUE_PROTOCOL/NODE_DIR/WORK_DIR; cwd = node work dir (:192). Pack managers (git, setup scripts) run via `pack_manager.cpp:117-120` with `systemEnvironment()` + GIT_TERMINAL_PROMPT. | anything | | ENV for cooperative packs (XDG/TMPDIR inherited). **No guarantee** for malicious/careless packs: absolute paths, `$HOME` via `getpwuid`. Only a sandbox (Landlock/bwrap) guarantees it. |
| 47 | Remote helper provider | sends prompts and images over HTTP to another host (remote_helper_provider.cpp) | yes (leaves the machine) | | policy, not paths |
| 48 | Model downloads | writes to `download_folder` or models dir; public data | no | model_library.cpp:75 | may stay shared (epic); partial files must not land in the vault |

**Status after step 1 (single root, 2026-11-22).** `FUGUE_HOME` / `--home` (spec 1.4.1, `src/engine/fugue_home.cpp`) sets XDG_*, XDG_RUNTIME_DIR, TMPDIR and the NVIDIA cache variables before Qt starts, so rows marked XDG or TMP (1, 3-14, 16-23, 30-34) and the children of rows 31, 32, 46 are fixed for cooperative programs; rows 2, 15, 24, 25, 27, 29 had code changes (marked in the table). The guard test is `tests/cli/test_cli_home_guard.cpp` (empty HOME diff, plus `strace -f`). Row 9 (queue and lock) is per root now, so a private root has its own. Still open: 15 (cache manager does not evict `thumbs/`), 22 (`model_paths` and `download_folder` are user settings), 26, 28 (read root is still `$HOME`), 35 (reads from `$HOME`, intended), 36, 37 (window title), 40, 41, 43, 44 (core dumps), 46 (packs not sandboxed), 47, 48.

### Environment inheritance (every place a child inherits)
- `command_node.cpp:766`: pack node processes, `QProcessEnvironment::systemEnvironment()` + 3 FUGUE_* vars. Persistent-mode packs live across runs, so env is fixed at spawn.
- `pack_manager.cpp:118`: git clone/pull and pack setup scripts, full environment (a `setup` script can pip/npm-install into `~/.cache/pip` etc.: `XDG_CACHE_HOME` fixes pip and npm cache; `~/.npm` is not XDG [U]).
- `helper_provider.cpp` (~:269-282) and `remote_helper_provider.cpp:471`: the helper process, no env call, so full inheritance. Env from FUGUE_HELPER only picks the binary.
- `ffmpeg.cpp:20,45` (QProcess, inherits) and `video_frames.cpp:60-80` (`posix_spawn`, inherits `environ`).
- No `setenv/qputenv` anywhere in src (grep): Fugue never sanitizes. A single `qputenv` of XDG_*/TMPDIR at startup, before the first QStandardPaths call and before any thread starts, redirects Fugue and every child. `QStandardPaths` caches nothing at the env level on Unix [U].

### Hard-coded absolute paths and home assumptions
- `providers.cpp:21` `"~/.config/fugue/providers.json"` string constant (and messages at omarchy_provider.cpp:430, :642).
- `file_roots.cpp:99,102` `$HOME` as read root, `$HOME/Documents/Fugue` as write root.
- `workflows.cpp:228-230`, `templates.cpp:163-164`: Documents dir.
- `omarchy_provider.cpp:373-374`, `theme.cpp:42-44`, `translator.cpp:15`: Omarchy files under `QDir::homePath()` (read only).
- `paths.cpp:47-52` `expandHome` maps `~` to `$HOME`: a vault-relative `~` has no meaning; settings holding `~/...` strings keep pointing at real home.
- `helper/src/main.cpp:1796-1800` reimplements XDG_STATE_HOME/HOME lookup (must stay in sync).
- `helper_provider.cpp:70` `/proc/driver/nvidia/version` (read). `thumbnail_cache.cpp:13` bypasses `paths.cpp`. `image_actions.cpp:112-115` suggests `Pictures` as the save dialog's folder.
- Packaging/CI already redirect for tests: `packaging/src/fugue/CMakeLists.txt:242-243` set `XDG_DATA_HOME` and `XDG_CACHE_HOME` (shows the approach works).

## Part B: prior art

**Firefox private browsing.** Mozilla's "Common Myths about Private Browsing" (support.mozilla.org/kb/common-myths-about-private-browsing, read via search snippets only; the page did not render for the fetcher): it prevents saving activity locally and does not make you anonymous to sites, ISP, employer; downloaded files and bookmarks stay; it does not protect against malware on your computer. Lesson: private window = "no local traces from the app", with an explicit what-it-cannot-do list, same as the epic's UI note.
**Chrome Incognito** (support.google.com/chrome/answer/95464): "Chrome doesn't retain site data or a record of the sites you visited"; cookies deleted when all Incognito windows close; "retains bookmarks that you save and files that you download"; "doesn't make you invisible"; school/employer/ISP and sites still see activity. Lesson: user-initiated saves (our Save node) are an explicit exit from the vault, and should be a labelled action.
**Firefox profiles**: profile folder `~/.mozilla/firefox/` holds places.sqlite, logins.json, key4.db (Mozilla support, search snippets); cache lives apart from the roaming profile (community post, **[U] as official fact**). Firefox never inherits between profiles [U].
**Chromium user-data-dir** (chromium.googlesource.com/chromium/src/+/main/docs/user_data_dir.md): default `~/.config/chromium`, overridden by `--user-data-dir` or `$CHROME_USER_DATA_DIR` (flag wins); one subdirectory per profile (`Default`); cache dir is derived: `~/.cache` (or `$XDG_CACHE_HOME`) plus the profile's path relative to the config dir, else the profile dir itself. Design to copy: **one root flag/env that wins, cache derived from it**. Their cache-follows-config rule means a root outside `~/.config` puts the cache inside the root, which is exactly what a vault wants.
**VS Code profiles** (code.visualstudio.com/docs/editor/profiles): a profile is settings, keybindings, snippets, tasks, extensions, UI layout; "Apply Setting/Extension to all Profiles" shares explicit items; "it's not possible to inherit settings from another profile" and a copied profile keeps no link. Lesson for `inherits`: VS Code chose explicit sharing of named items over inheritance, which matches the epic's short explicit list. `--user-data-dir`/portable mode page not fetched [U].
**Tails** (tails.net/about): "never writes anything to the hard disk and only runs from the memory"; memory erased on shutdown; optional encrypted Persistent Storage where "you choose what persists, everything else is amnesic"; not magic (malicious hardware, infected install). Lesson: two vault flavours (ephemeral and persistent) map to Tails' amnesic vs Persistent Storage; the amnesia is a property of the OS (no swap, RAM FS), which Fugue cannot promise; say so.
**Qt locations**:
- QStandardPaths (doc.qt.io/qt-6/qstandardpaths.html): CacheLocation = `~/.cache/<APPNAME>`, GenericCacheLocation = `~/.cache`; StateLocation/GenericStateLocation (`~/.local/state`) exist since Qt 6.7; `setTestModeEnabled(true)` redirects data/config/state/cache locations to `~/.qttest/...` (the page lists them as XDG_*_HOME values). The page does not itself document non-test XDG overrides; spec 1.4 and the CI config in this repo rely on them, and Qt source implements them on Unix [U: not read here]. This machine runs qt6-base 6.12.0.
- QML disk cache (doc.qt.io/qt-6.8/qmldiskcache.html via search): default is a `qmlcache` subdirectory of QStandardPaths::CacheLocation; `QML_DISK_CACHE_PATH` sets a custom location (Qt 6.5+); `QML_DISK_CACHE` tunes behaviour; `QML_DISABLE_DISK_CACHE` disables. Observed here: `~/.cache/fugue/qmlcache`.
- Shader cache: Qt Quick 3D docs say the disk cache needs a writable QStandardPaths cache location, and `Qt::AA_DisableShaderDiskCache` plus its env var disables both low-level and Quick 3D caches; env var name not confirmed [U] (candidates `QT_DISABLE_SHADER_DISK_CACHE`, `QSG_RHI_DISABLE_DISK_CACHE`). Pipeline cache save/load is in QQuickGraphicsConfiguration docs, not read [U]. Observed here: both `~/.cache/qtshadercache-*` and `~/.cache/fugue/qtpipelinecache-*`.
- QtWebEngine: **not used by Fugue** (no hit in src). For reference, QtWebEngineProfile can be off-the-record (permissions, favicons not stored) and `setPersistentStoragePath()` relocates storage (doc-snapshots.qt.io/qtwebengine/qtwebengine-features.html).
- Easiest relocation: set `XDG_CACHE_HOME=$FUGUE_HOME/cache` before `QGuiApplication` is constructed. That moves QML, QRhi pipeline and shader caches, Mesa and fontconfig with one variable. Or `Qt::AA_DisableShaderDiskCache` plus `QML_DISABLE_DISK_CACHE=1` in private mode (costs startup time every launch; the qmlc cache holds only code, so relocating beats disabling).

**Guard-test tooling** (docs read: inotify(7); Landlock kernel docs):
- inotify: not recursive, race on new subdirectories, no PID, queue overflow drops events, mmap writes invisible. Good for "watch these N roots", poor as an "outside the vault" audit because the set of unknown outside paths is unbounded.
- fanotify (`FAN_MARK_FILESYSTEM`, needs CAP_SYS_ADMIN) sees everything with PIDs but needs root: no for CI. [U]
- strace -f -e trace=%file,write... (`-e trace=openat,creat,mkdir,rename,unlink,symlink,link,truncate,mknod,connect` then filter write flags `O_WRONLY|O_RDWR|O_CREAT`): sees every path, every child, no root, no privileges unless ptrace is restricted (Yama ptrace_scope=1 allows tracing own children; fine). Works headless. Slow, noisy but the run is a small workflow. Misses nothing the syscalls cover; handles static binaries (unlike LD_PRELOAD).
- LD_PRELOAD shim: fast but misses static binaries, raw syscalls (the C++ helper is dynamically linked, ffmpeg too, but Go/Rust packs may bypass libc) [U].
- Landlock: a **deny-by-default enforcer**, not a logger: build a ruleset that allows write only under vault+tmp, run Fugue inside, and any outside write fails with EACCES, which the test sees as an error plus audit records (ABI 7+ logging; "denials by a program's own sandbox are audited" per kernel docs). Needs Linux 5.13+, `CONFIG_SECURITY_LANDLOCK` and LSM enabled, no root; works headless. Caveats from docs: `stat/chmod/chown/access` are not restricted, ABI differences, `REFER` denied by default. A write a program ignores errors for would go unnoticed, so pair with strace.
- **Recommendation**: test = `strace -f -ttt -e trace=file,desc? ` (use `-e trace=%file,%desc` and parse for write opens) under `unshare`-free CI, with `HOME=$tmp/home`, `XDG_*` and `TMPDIR` all pointing into the vault except a fake HOME created empty and asserted still empty. Two layers: (1) set HOME to an empty directory: any leak to `~` is found by a plain recursive diff, no tracing at all, and it needs no tooling; (2) strace to catch absolute-path writes (`/tmp`, `/var`, `/run/user`, `/dev/shm`). Landlock is the later enforcement (epic step 5), and its audit can double as the test. inotify only as a cheap supplement on `$HOME` and `/tmp`.
- Offscreen/headless: `QT_QPA_PLATFORM=offscreen` (the repo's tests already run this way); the CLI path (`fugue run`) needs no display at all. Test both: CLI run of a workflow with a command pack and a video node, and an offscreen window start that loads QML, opens a file, runs and closes.

## Smallest design: one root, `FUGUE_HOME`

1. Resolve root once in `main()` and `cli::run` before any Qt object: `FUGUE_HOME` (env or `--home`/`--private <vault>`). If set, `qputenv` (and mkdir) `XDG_CONFIG_HOME=$R/config`, `XDG_DATA_HOME=$R/data`, `XDG_STATE_HOME=$R/state`, `XDG_CACHE_HOME=$R/cache`, `XDG_RUNTIME_DIR=$R/run` (0700), `TMPDIR=$R/tmp`. `paths.cpp` then needs no change: Fugue, the helper, ffmpeg, packs and driver caches inherit them. Keep a copy of the original `XDG_*`/`HOME` only for the inherit list below.
2. Defaults for the three places that bypass XDG: output dir (`$R/outputs`, paths.cpp:25), `FileRoots` (file_roots.cpp:99,102), Documents/templates (templates.cpp:163, workflows.cpp:228).
3. Fix the strays: thumbnail_cache.cpp:13 should call `cacheDir()`; providers.cpp:21 string; image_actions.cpp viewCopy and suggestedFolder.
4. Do **not** override `HOME`: Omarchy theme/language reads (theme.cpp, translator.cpp) and the user's real `~`-based model paths depend on it. Cost: `~/.nv`, `~/.npm`, `~/.mozilla`-style tools that ignore XDG still write to the real home; the guard test lists these as known exceptions or moves to step 5's sandbox.
5. Mark inheritance in code, not env: `inherits` reads from the ordinary profile's directories (read-only), e.g. built-in nodes (compiled in), `model_paths` and `trust.json` read from the *original* config dir.

### Order of changes
1. Guard test skeleton first with HOME=empty-dir diff + strace; make it fail on today's code (proves the leaks in table rows 15, 24-29, 30).
2. `FUGUE_HOME` bootstrap in `main()`/`cli::run` (env redirect). Fixes most rows at once.
3. Stray-path fixes (rows 2, 15, 24, 27-29) and the cache manager covering `thumbs` (row 15).
4. Private-mode behaviours: no recent files (row 4), recovery policy (row 5), window title (37), viewer copy off (29), RLIMIT_CORE (44), node stderr (43), single-purpose queue/lock (9).
5. Profiles/inherits; then vault backends; then Landlock/bwrap sandbox for packs and ffmpeg.

## Risks
- Env must be set before the first `QStandardPaths` call and before any thread; static initialisers in Qt plugins may run earlier [U]. Test with the strace guard.
- Queue/lock (`engine.lock`, queue dir) are shared by design (spec 9.8) so the GPU is not oversubscribed; separate roots mean two sessions can run on the GPU at once. Decide: keep a shared lock in a non-sensitive path (e.g. `$XDG_RUNTIME_DIR` of the real user, contents are run ids only) or accept.
- Persistent-mode pack processes and the long-lived helper keep their start-up env; a profile switch needs restart.
- Crash or SIGKILL leaves `/tmp/fugue-helper-*` and ffmpeg logs (rows 30, 32): with TMPDIR in the vault they are in the vault, which is the point; an ephemeral vault wipes them.
- Portals, clipboard managers, file managers' thumbnails, swap, journal output, window titles seen by the compositor: not redirectable; must be documented in the UI.
- `hashes.json` and the result cache keys: sharing them with an ordinary session would reveal "this input was seen"; they must not be inherited (the epic says so).
- Packs without a sandbox can still write anywhere; cooperative env redirection is a hope, not a guarantee, until step 5.
- gocryptfs/FUSE and inotify/strace under `unshare`: not relevant to the env design, but the vault root must be on a filesystem that supports hard links (the result cache uses hard links, run_folder spec 9.7) [U: gocryptfs supports hard links; not checked here].

---

# Vault backends for Fugue private mode (epic bmaltais/fugue#204)

Researched 2026-11-22. Claims not confirmed from a primary source fetched in this session are marked [U]. Package facts were checked with `pacman -Si` on the dev machine (Arch/Omarchy, kernel 7.2.15).

## Summary and recommendation

- **Default: gocryptfs (FUSE), driven by QProcess.** Official `extra` repo (2.7.0-1), MIT, no root needed (needs setuid `fusermount3`, already installed here), per-file encryption so it resumes after restart and large files and many small files are fine, authenticated (GCM), audited once (2017). Matches the epic's own choice.
- **Fallback / "no-cost" backend: ephemeral directory** on a tmpfs under `$XDG_RUNTIME_DIR` (`/run/user/UID`, tmpfs, 7.9G here). Not resumable, wiped at close, swap exposure only if swap is disk-backed (here swap is zram, RAM only). Use `memfd_secret` only for the passphrase/master key, not data.
- **Optional fscrypt backend (later):** the only one with no FUSE overhead, but needs an ext4/f2fs filesystem with the `encrypt` feature enabled, a one-time root `fscrypt setup`, and leaks file sizes, mtimes, xattrs and directory shape. Worse than gocryptfs on leaks, better on speed. Not the default.
- **Skip:** eCryptfs (kernel status Orphan, see below), EncFS (author recommends gocryptfs), Cryptomator CLI (AGPL, Java, AUR only), LUKS via udisks2 (polkit `auth_admin` prompts for loop-setup; no unprivileged path), systemd-homed (scoped to login home directories).
- **Licensing:** gocryptfs is MIT, so shelling out is clean. For a Qt library route there is no maintained C++ gocryptfs library [U]; use QProcess.
- **Honest caveat:** any FUSE vault protects only the closed state. While mounted, the plaintext is readable by every process of the same user, and the page cache, swap, and metadata of the backing directory leak (see Leaks).

## Comparison table

| Backend | Arch package (repo) | Licence | Root / privilege | Resumable | Large files | Leaks beyond names/contents | Maintained |
|---|---|---|---|---|---|---|---|
| gocryptfs | `gocryptfs` 2.7.0-1 (extra), needs `fuse3` (extra) | MIT | none beyond setuid fusermount3 | yes | yes, 4 KiB blocks, +32 B/block, 18 B header | file sizes (approx), file count, dir tree shape, mtimes; names encrypted | yes (v2.7.0, 2026-11-11 per README) |
| fscrypt | `fscrypt` 0.3.7-1 (extra), kernel feature | Apache-2.0 (tool) | root once for `setup`; v2 unlock without root | yes | native speed | sizes, counts, timestamps, xattrs, dir tree, holes | yes |
| eCryptfs | `ecryptfs-utils` 111-10 (extra) | GPL [U on tools] | mount(2), setuid helper [U] | yes | 4 KiB-increment sizes, CBC no integrity | name prefix leaks | kernel side Orphan since 2023 patch; removal discussed |
| EncFS | `encfs` 1.9.5-11 (extra) | LGPL-3.0 | none | yes | ok | CBC, no integrity by default, unsigned config | dormant; author recommends gocryptfs |
| Cryptomator CLI | not in repos; AUR [U] | AGPL-3.0 (or commercial) | FUSE | yes | ok | per Cryptomator docs fetched: not stated [U] | active |
| LUKS container file | `cryptsetup` (core), `udisks2` (extra) | GPL | polkit auth_admin for loop-setup | yes | block-level, best | only container size | yes |
| systemd-homed | `systemd` (core) | LGPL | root service | yes | n/a | for human home dirs only | yes |
| Ephemeral tmpfs | none (kernel) | n/a | none via `/run/user/UID` | no | bound by RAM | swap if disk swap | n/a |

## Details with source URLs

### gocryptfs
- Licence MIT; external audit by defuse.ca on 2017-03-03 (audit URL: https://defuse.ca/audits/gocryptfs.htm); signed releases; v2.7.0 dated 2026-11-11. Source: https://github.com/rfjakob/gocryptfs and https://raw.githubusercontent.com/rfjakob/gocryptfs/master/README.md
- Comparison page: GCM with integrity, per-directory IV and EME name encryption so neither prefix nor identical-name leaks, scrypt password hashing, no file-size obfuscation. Source: https://nuetzlich.net/gocryptfs/comparison/
- Storage overhead: empty file = 0 bytes, non-empty has an 18-byte header, every 4 kB block adds 32 bytes. Source: README above. Performance: as fast as EncFS default mode; `stupidgcm` OpenSSL path gives 4x on CPUs without AES-NI; `gocryptfs -speed` benchmarks. Source: README. Per-throughput numbers for big sequential files: not verified [U] (the performance.md page 404d).
- FUSE need: built on go-fuse; the distro `fuse` package must be installed. Local check: `/usr/bin/fusermount3` is `-rwsr-xr-x root` and `/etc/fuse.conf` has no active lines (so `user_allow_other` is off). Man page: https://raw.githubusercontent.com/rfjakob/gocryptfs/master/Documentation/MANPAGE.md
- `-allow_other`: lets other users in, requires `user_allow_other` in `/etc/fuse.conf`. Fugue does not need it (same user). Be aware that without it root and other users cannot see the mount, which is the desired default.
- Filename handling: names are encrypted; default max encrypted name length 255, longer names are hashed into extra `.name` files (`-longnamemax` 62..255). `-plaintextnames` leaks names, never use for a vault. `-deterministic-names` leaks identical names across directories, do not use. `-raw64` is recommended for shorter names. Source: man page.
- Reverse mode (`-reverse`): read-only encrypted view of a plaintext tree, used for backup. Not useful as a vault; could be useful to back up an ordinary tree. Source: man page.
- Passphrase input: `-passfile FILE`, `-extpass CMD` (runs e.g. an askpass program). The fetched man page documents stdin only for `-masterkey=stdin`; the changelog says v0.12 "accept password from stdin as well". Source: man page + README changelog. Exact stdin behaviour when stdout is not a tty: [U], test in the prototype.
- Other flags: `-fg` (foreground, so Fugue can supervise the process, logs to stdout), `-nosyslog`, `-ro`, `-idle` (auto-unmount after idle; avoid with a running workflow, or set larger than the longest idle), `-sharedstorage` (disables stat caching/hard links, only for sync-shared backing dirs). Exit codes: 12 wrong password, 10 mountpoint not empty, 22 empty password on init, 6 cipherdir not empty on init, 23/24 config read/write, 26 fsck errors. Source: man page.
- `gocryptfs -init` stores `gocryptfs.conf` inside the cipher dir; print/keep the master key is advised in README. For a private vault tied to a person, do not print the master key into a log.

### fscrypt
- Kernel doc: filesystems ext4, F2FS, UBIFS, CephFS; btrfs and tmpfs not listed. Encrypts file contents, filenames, symlink targets; NOT sizes, permissions, timestamps, xattrs, hole locations; special files not encrypted. v2 policies since Linux 5.4; with v2, adding a key (`FS_IOC_ADD_ENCRYPTION_KEY`) needs no privilege (limited by keyring quota); v1 needs CAP_SYS_ADMIN. Encryption applies only to an empty directory. Source: https://www.kernel.org/doc/html/latest/filesystems/fscrypt.html
- Tool: `fscrypt setup` always needs root (creates `/etc/fscrypt.conf`, `/.fscrypt`); unlock/lock normally no root for v2; PAM module for login unlock; Apache-2.0; ext4 needs the `encrypt` feature flag. Source: https://github.com/google/fscrypt
- Fugue-relevant weakness: a fscrypt dir is not a self-contained file; backup or sync of the locked tree and moving it across filesystems do not work (tool README). Key lives in kernel keyring; remove with `FS_IOC_REMOVE_ENCRYPTION_KEY` (only removes the calling user's claim; files still open stay readable until closed [U on semantics beyond kernel doc]).
- Whether the user's home filesystem has the `encrypt` flag: [U] on this machine (not checked; ext4 vs btrfs unknown). Omarchy defaults to btrfs [U], which does not support fscrypt per the kernel doc fetched. This alone makes fscrypt a poor default.

### eCryptfs
- Kernel page contains no deprecation notice but says "currently undergoing development, maintain backup". Source: https://www.kernel.org/doc/html/latest/filesystems/ecryptfs.html
- LKML: March 2023 patch to mark it Orphan, discussion on removal; Oct 2025 thread "ecryptfs is unmaintained and untested". Sources: https://lkml.iu.edu/hypermail/linux/kernel/2303.2/05373.html , https://lkml.indiana.edu/hypermail/linux/kernel/2510.1/06014.html . Whether it was removed from mainline by now: [U].
- Crypto: CBC without integrity and name-prefix leaks (comparison page above). Not recommended.

### EncFS
- Repo: "mostly dormant for years", C++ code removed, Rust port beta; author recommends GoCryptFS or full-disk encryption; legacy format is PBKDF2-SHA1, CBC, unsigned config, unencrypted xattrs. Source: https://github.com/vgough/encfs. The 2014 Hornby audit is not mentioned on that page [U]. Not recommended.

### Cryptomator
- CLI: FUSE through libfuse, WebDAV fallback; `--password:stdin`; `unlock` single vault format 8; `fusermount -u` needed after a failed graceful unmount; AGPL-3.0 or commercial; v0.7.0. Source: https://github.com/cryptomator/cli
- Format: `vault.cryptomator` JWT, SIV_GCM or SIV_CTRMAC. Source: https://docs.cryptomator.org/security/architecture/ . Needs a JVM [U]. Not in official repos (only the pacman search for `cryptomator` returned nothing here); AUR [U]. Skip.

### LUKS container file via udisks2/cryptsetup
- udisks2 `LoopSetup(fd, options)` creates a block device from a file descriptor. Source: https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Manager.html
- Polkit defaults (upstream policy): `loop-setup` is `auth_admin` for any/inactive and `auth_admin_keep` for active sessions; `encrypted-unlock` and `filesystem-mount` are `yes` for active local sessions. So an unprivileged app gets a password prompt on every loop attach (cached by `_keep`). Source: https://raw.githubusercontent.com/storaged-project/udisks/master/data/org.freedesktop.UDisks2.policy.in . Creating the container (`cryptsetup luksFormat`, mkfs) as a user via udisks `Block.Format`/`Manager.LoopSetup` flow: [U] exact calls; plain `cryptsetup open` needs root [U].
- Pros: block-level encryption hides sizes and tree of the inside; strongest. Cons: fixed size file (preallocated or sparse), polkit prompts, container file size is visible, no per-file backup, ext4-in-LUKS growth is manual. Fine as the epic's later optional backend.

### systemd-homed
- Backends LUKS2 loop file, fscrypt, directory, subvolume, CIFS; scoped to human users' home directories, activated at login. Source: https://systemd.io/HOME_DIRECTORY/ . Not suitable for an app-level vault; could mean "Fugue data inside an already-encrypted home", a documentation recommendation only.

### Ephemeral
- tmpfs: `size=` default 50% of RAM, `size=0` unlimited (dangerous), pages swappable by default, `noswap` mount option exists. Source: https://www.kernel.org/doc/html/latest/filesystems/tmpfs.html . `/dev/shm` here is a 40G tmpfs, shared by all users and apps; `/run/user/1000` here is 7.9G tmpfs (`df` on this machine). Prefer `$XDG_RUNTIME_DIR/fugue-private-<random>` with mode 0700. An unprivileged `mount -t tmpfs` is not possible without a user namespace [U]; so use a subdirectory of an existing tmpfs, and the size cap is the shared one (no per-vault quota; enforce a soft one in Fugue by checking `statvfs`).
- This machine: `swapon` shows only `/dev/zram0` (78.4G, zstd), so RAM pages swapped there never reach disk. On a machine with a disk or file swap, tmpfs pages can reach disk unencrypted (kernel doc above). `memfd_secret` (Linux 5.14, enabled by default since 6.5, mlocked, blocks hibernation while open, bounded by RLIMIT_MEMLOCK) is suitable for the key only. Source: https://man7.org/linux/man-pages/man2/memfd_secret.2.html
- Videos of hundreds of MB each in RAM will exhaust a vault quickly; make the ephemeral backend refuse or warn when the vault grows past a chosen fraction of free RAM.

### Crash detection and cleanup
- `fusermount3 -u MOUNT` unmounts; `-z` lazy unmount; `-q` quiet; the tool must be setuid root. Source: https://man7.org/linux/man-pages/man1/fusermount3.1.html . A lazy unmount detaches immediately and leaves open files working until closed, so only use after trying `-u`.
- Cleanup design (proposed, [U] untested): a lock file `<vault>/.fugue-session.lock` holding PID, boot id, start time, held with `flock` for the session lifetime. On open: if the lock is flock-able and the mountpoint still appears in `/proc/self/mountinfo` with fstype `fuse.gocryptfs`, the previous session crashed; offer `fusermount3 -u`, then retry with `-z`. If gocryptfs ran `-fg` as a child, its death unmounts nothing automatically if gocryptfs itself crashed (the mount becomes "Transport endpoint is not connected"), so cleanup is required then too.
- A systemd user scope per session: `systemd-run --user --scope --unit=fugue-vault-<id> gocryptfs -fg ...`. The scope is tracked by the user manager, not removed at session end unless lingering/KillUserProcesses applies; with `--scope` systemd-run is the parent and blocks until exit, so launch it from QProcess. Source: https://www.freedesktop.org/software/systemd/man/latest/systemd-run.html . Benefit: `systemctl --user stop` or `list-units` gives a crash-proof way to find and stop leftover vault processes. `MemoryMax`, `KillMode`: the man page fetched does not cover them [U].

### Passphrase handling
- Do not put a passphrase in argv: `/proc/PID/cmdline` is world-readable by default [U, well known, not fetched this session]. gocryptfs offers `-passfile`, `-extpass CMD`, and stdin. Recommended order: stdin pipe (QProcess `write` then `closeWriteChannel`), or `-extpass` pointing at a tiny Fugue helper that prints the passphrase from a pipe/fd [U on which is safer; the helper's argv has no secret in either case].
- Store in keyring only if the user opts in: Secret Service D-Bus API (`org.freedesktop.Secret.Service`), implemented by gnome-keyring and KWallet (authors listed on the spec). Source: http://specifications.freedesktop.org/secret-service/latest/ . Arch: `libsecret` 0.21.8.2 (core), `gnome-keyring` 1:51.1 (extra), `kwallet` 6.32.0 (extra), `tpm2-tools` 5.8-2 (extra), `tpm2-pkcs11` 1.10.1 (extra). TPM sealing of the vault key: [U] no worked recipe verified; leave out of v1.
- Keep the passphrase in a `QByteArray` zeroed after use, `mlock`/`memfd_secret` where practical.

### Qt integration
- QProcess with gocryptfs `-fg`, parse "Filesystem mounted and ready." [U exact string] or poll `/proc/self/mountinfo`; stop with `fusermount3 -u` then wait for the child. No library route found: cppcryptfs is an independent C++ reimplementation for Windows (README), not a Linux library; libgocryptfs not found [U]. QProcess is the right choice and keeps MIT/GPL concerns away.

## Leaks (what a vault does not hide)

- **Backing dir:** with gocryptfs, file count, approximate sizes (plaintext size + 32 B/4 KiB), mtimes/ctimes/atimes, the directory tree shape, and total used size. Names are hidden. With fscrypt: all of those plus xattrs and file holes.
- **While mounted:** plaintext is visible to same-user processes, root, and any app Fugue shells out to; page cache holds plaintext, so memory dumps, hibernation images and swap can contain it; kernel `dmesg`/journal may log file names for errors [U]; core dumps (the epic notes this).
- **Swap:** disk swap leaks tmpfs pages and anonymous memory (kernel doc above); zram swap (as on this machine) stays in RAM. Recommend encrypted swap or zram in docs.
- **Journals and desktop services:** file manager thumbnails (`~/.cache/thumbnails`), recent-files (`recently-used.xbel`), clipboard managers, screenshots, the compositor, GPU memory. These are outside the vault by design, as epic notes.
- **Existence:** the vault directory name, its size, and timestamps reveal that a private session exists and when it was used.
- **FUSE specifics:** the mount shows in `mount`/`/proc/self/mountinfo` as `fuse.gocryptfs` with its path. A running gocryptfs process holds the master key in memory; `ptrace` by same-user processes can read it (Yama `ptrace_scope` default [U]).

## Open questions

1. Is the target `~` on btrfs or ext4? (fscrypt viability, `encrypt` flag.) Not checked.
2. Exact gocryptfs stdin passphrase behaviour with `-fg` and a non-tty; and its readiness message. Test in the prototype.
3. Does `-idle` interact badly with long workflows with no filesystem activity? Test.
4. Behaviour of gocryptfs when killed with SIGKILL (stale mount, "Transport endpoint is not connected") and whether `fusermount3 -u` cleans it. Test.
5. Per-vault size quotas: gocryptfs has none; backing filesystem quota or a loop-backed LUKS would be needed. Do we want a cap?
6. `-sharedstorage` or sync use (the epic mentions sync): needs a test with e.g. Syncthing; deterministic names trade-off.
7. Upstream gocryptfs maintenance risk (single maintainer) [U]; fallback is fscrypt/LUKS.

## Prototype plan

1. `VaultBackend` interface: `create(path, secret)`, `open(path, secret) -> mountpoint`, `close()`, `isStale(path)`.
2. `EphemeralBackend`: `mkdir -m700 $XDG_RUNTIME_DIR/fugue-private-XXXXXX`, close = recursive delete (best effort overwrite not meaningful on tmpfs). Check `statvfs` quota.
3. `GocryptfsBackend` via QProcess: `gocryptfs -init -raw64 -nosyslog CIPHER` (stdin passphrase), `gocryptfs -fg -nosyslog -raw64 CIPHER MOUNT` under `systemd-run --user --scope --unit=fugue-vault-<id>`. Readiness = poll mountinfo (timeout 10 s). Close = `fusermount3 -u`, fall back to `-z` after 5 s, report open handles.
4. Lock + stale detection: flock file in the cipher dir, boot-id + PID; mountinfo scan; UI offers clean unmount.
5. Manual test script: fill vault with a 2 GB file and 50k small files, time it, kill -9 gocryptfs, check recovery, check `/proc/PID/cmdline` for the passphrase, confirm `strace -f -e trace=file` shows no writes outside the mount and runtime dir.
6. Document: swap, thumbnails and clipboard caveats in the UI.
7. Later: fscrypt and LUKS (udisks2) behind the same interface.

## Not verified / fetch failures

The Arch Wiki (Anubis block), the gocryptfs security.md and performance.md (404), and the linux MAINTAINERS eCryptfs entry (truncated) could not be read. Everything from those areas is marked [U] or sourced via LKML search results.

---

# Sandboxing pack nodes and ffmpeg in Fugue private mode (epic #204)

Date 2026-11-22. Machine tested: Arch/Omarchy, kernel 7.2.15, RTX 3090, bubblewrap 0.13.1, uv. Claims marked [U] are unverified.

## Summary and recommendation

- Spec facts: `needs` has `network`, `files`, `gpu` (6.3), "declared, not enforced". Trust (6.7) says "No sandbox in v0.1... optional isolation (bubblewrap, Landlock) is later". 6.8: `setup` (e.g. `uv sync`) runs after the user confirms the command, venv inside the pack folder. Native packs run in-process and cannot be sandboxed (6.9).
- **Default: Landlock, applied in the child between fork and exec** (a tiny `fugue-sandbox-exec` helper, or in-process in Fugue's spawn code). Reason, all measured here: CUDA/nvidia-smi worked, ffmpeg worked, no mount namespace, no `/dev` surgery, no extra dependency, no runtime cost, unprivileged, ~60 lines of C. Kernel here reports ABI 10, `CONFIG_SECURITY_LANDLOCK=y`, `landlock` in the active LSM list.
- **Fallback / complement: bubblewrap** when a private view of the filesystem is wanted (hide `$HOME` entirely, empty `/tmp`) or the kernel has no Landlock. It works here, but its `--dev` breaks NVIDIA (use `--dev-bind /dev /dev`), which weakens it.
- **Network:** Landlock handles TCP bind/connect (ABI 4) and UDP (ABI 10) but NOT by itself pathname unix sockets unless `LANDLOCK_ACCESS_FS_RESOLVE_UNIX` (ABI 9) is handled. My test showed DNS (via systemd-resolved socket) and the D-Bus session socket still reachable when only the TCP rights were handled. So handle RESOLVE_UNIX and the abstract-socket scope, and grant only the sockets needed. For a hard "no network" guarantee use `bwrap --unshare-net` (or `unshare -rn` equivalent), because Landlock network rules are port-based, not address-based.
- **Honest UI:** three states, computed from what was really applied: "Sandboxed: writes only to the vault, no network"; "Partly sandboxed" (say what is open, e.g. network allowed because the pack declares `needs.network`); "Not sandboxed" (native pack, or kernel without Landlock). Never label it from the manifest alone.
- **Smallest first slice:** helper that applies Landlock write-confinement (rw = vault + temp root + `/dev/nvidia*`, `/dev/dri`, `/dev/shm`; read-only = system dirs + pack folder + model folders) and TCP/UDP deny when `needs.network` is false, used for ffmpeg first (no GPU, one binary, easy test), then command packs. Plus the hostile-pack test below.

## Options table

| Mechanism | Unprivileged | GPU (CUDA/Vulkan) | uv venvs | ffmpeg | Network deny | Cost | Licence | Verdict |
|---|---|---|---|---|---|---|---|---|
| Landlock | yes (no_new_privs) | works, device nodes must be allowed (tested nvidia-smi) | works if the python install dir and venv are readable | works (tested) | TCP, UDP per port; unix sockets only via RESOLVE_UNIX; no address filter | ~0 | kernel GPL syscalls, userspace code is ours | default |
| bubblewrap | yes (user namespaces; setuid mode removed) | needs `--dev-bind` for NVIDIA (`--dev` failed here); Vulkan ICD JSON in /usr/share/vulkan/icd.d visible if /usr bound | fine if paths bound at same absolute paths | fine | `--unshare-net` is a hard cut (no loopback to host either) | fork+mount setup, ms | LGPL-2.1+ (pacman) | fallback / hard network cut |
| firejail | SUID binary | works with profiles [U] | [U] | [U] | `--net=none` | small | GPL-2 [U, not in the fetched text] | avoid: SUID root, not installed, large attack surface |
| systemd-run --user | yes, but filesystem/ns options only with PrivateUsers (user ns) | DeviceAllow via eBPF [U in user mgr] | ok | ok | PrivateNetwork needs userns; IPAddressDeny needs cgroup eBPF, user support not stated | process spawn via D-Bus, not child of Fugue | LGPL | no: wrong ownership model (unit, not child), ProtectSystem unavailable in user mgr |
| seccomp (alone) | yes | fine | fine | fine | can block `socket(AF_INET*)` | ~0 | n/a | complement only: no path rules |
| unshare (user+mount+net) | yes | needs hand-built mounts | same | same | `-n` | ms | util-linux | what bwrap already is; do not reinvent |

## Details

**Landlock** (https://docs.kernel.org/userspace-api/landlock.html).
- ABIs: 1 base filesystem (exec, read/write file, read/list dir, make-*, remove), 2 REFER, 3 TRUNCATE, 4 TCP bind/connect ports, 5 IOCTL_DEV, 6 scopes (abstract unix sockets, signals), 7 logging flags, 8 TSYNC (all threads), 9 RESOLVE_UNIX (pathname unix sockets), 10 UDP, 11 NO_NEW_PRIVS flag, 12 capability/namespace restrictions. The doc page gives kernel versions only for ABI 1 (5.13). This machine's kernel returns ABI 10 (measured with `landlock_create_ruleset(NULL,0,VERSION)`); a current Arch kernel is therefore at least ABI 10 [Arch ships LSM landlock enabled here; other Arch kernel flavours not checked, U].
- Cannot restrict: chdir, stat, flock, chmod, chown, setxattr, utime, fcntl, access; mount/pivot_root are denied for a restricted thread but chroot is not; pipes/sockets reached through /proc/PID/fd and namespace files only partly covered; ioctl rule only for newly opened devices; max 16 stacked layers. It restricts, it does not hide: a denied path still exists and is `stat`-able, and a denied read is EACCES not ENOENT (a pack may be confused by it, a "no such file" is gentler with bwrap).
- Apply: `landlock_create_ruleset` (handled rights), `landlock_add_rule` per path with an O_PATH fd, `prctl(PR_SET_NO_NEW_PRIVS)`, `landlock_restrict_self`; inherited by all children, irrevocable. Best-effort pattern: query the ABI and mask unsupported rights. From C++: three syscalls, no library needed (a libc wrapper is absent in older glibc; use `syscall(444/445/446)`; header `linux/landlock.h`). Doing it in the forked child before exec (QProcess `setChildProcessModifier`) avoids a helper binary; a helper `fugue-sandbox-exec <policy> -- cmd` is simpler to test and reuse for ffmpeg.
- Measured here (python ctypes harness, rw only on a vault dir, ro on /usr /etc /opt /proc /sys /run /dev, GPU nodes rw):
  - write to `~` denied (EACCES), write to vault ok, read of `~/.bashrc` denied.
  - TCP connect denied; UDP sendto denied once UDP rights (ABI 10) were handled, allowed when only TCP was handled.
  - nvidia-smi saw the RTX 3090; ffmpeg encoded a test clip into the vault.
  - LEAK found: getaddrinfo still succeeded (resolved via a unix socket under /run) and connecting to `/run/user/1000/bus` (D-Bus) succeeded. Fix: do not grant /run broadly, handle RESOLVE_UNIX (ABI 9) and scope abstract sockets (ABI 6). Not re-tested with those handled [U].
  - `uv venv` inside the sandbox failed only because it could not read `~/.local/share/uv/python`: grant read to the interpreter location and set `UV_CACHE_DIR`, `HOME`, `TMPDIR`, `XDG_*` into the vault (the epic's env redirection).
- CUDA specifics [U beyond nvidia-smi]: the driver opens /dev/nvidia*, /dev/nvidia-uvm, may use /dev/shm and /dev/nvidia-caps; a real torch run was not tested. Vulkan needs read of /usr/share/vulkan/icd.d, driver libs and `/dev/dri/*`; Vulkan shader caches write under XDG_CACHE_HOME (redirect to the vault; also `__GL_SHADER_DISK_CACHE_PATH`, `CUDA_CACHE_PATH`, `TRITON_CACHE_DIR`, `TORCH_HOME` [U names, check each]). Writes to the GPU driver's cache in $HOME is exactly what private mode must catch.

**bubblewrap** (https://raw.githubusercontent.com/containers/bubblewrap/main/README.md, https://man.archlinux.org/man/bwrap.1).
- Builds an empty tmpfs root; you bind in what the program may see (`--ro-bind`, `--bind`), `--dev-bind` for device nodes, `--proc`, `--tmpfs`, `--unshare-net`, `--unshare-all`, `--die-with-parent`, `--new-session` (needed against TIOCSTI, CVE-2017-5226), `--seccomp FD`, `--cap-drop ALL`, `--clearenv`, `--disable-userns`. Setuid mode was removed; it needs unprivileged user namespaces. Here `kernel.unprivileged_userns_clone=1`, `max_user_namespaces=320491`, so it works; Arch default kernel allows it, hardened kernels may not [U].
- "Protection is entirely determined by the arguments" and anything mounted in (D-Bus socket) is an escape route.
- Measured: `--ro-bind / / --dev-bind /dev /dev --tmpfs $HOME --unshare-net nvidia-smi` works; with `--dev /dev` nvidia-smi fails ("couldn't communicate with the NVIDIA driver"). `ls ~/.ssh` is simply absent, writes to `~` go to a throwaway tmpfs, curl fails to connect. Binding `/` read-only then overlaying `$HOME` and `/tmp` is the lowest-breakage recipe.
- Flatpak uses bwrap; default no network, no device nodes except basics, `--device=dri` for GL, `--device=all` for the rest, `--share=network` also exposes host abstract unix sockets (https://docs.flatpak.org/en/latest/sandbox-permissions.html). The Flatpak page does not explain NVIDIA driver handling (it ships driver versions as runtime extensions, [U]). Lesson: GPU = grant devices, network = separate namespace.
- Licence LGPL-2.1-or-later (pacman metadata). Invoking an external binary has no linking issue; Arch has it as a dependency of many things, and it is installed here.

**firejail** (https://raw.githubusercontent.com/netblue30/firejail/master/README.md). Described as "an SUID sandbox program"; Landlock support "ongoing/experimental", needs no_new_privs; the README discusses CVE-2021-26910 (a distro package, not SUID). A SUID-root program with a large option surface is itself privilege-escalation surface (the README does not analyse this; the concern is general [U]); not installed here, wiki fetch was blocked. Skip.

**systemd-run --user** (https://man.archlinux.org/man/systemd.exec.5, https://man.archlinux.org/man/systemd.resource-control.5). In the user manager, ProtectHome, ReadWritePaths, ReadOnlyPaths, InaccessiblePaths, PrivateTmp, PrivateNetwork, PrivateDevices are available only where PrivateUsers is implicitly on (needs unprivileged userns); ProtectSystem is not available. DeviceAllow is eBPF-based; IPAddressDeny needs cgroup eBPF and the page does not say it works in per-user instances. The pack would be a transient unit under the user manager, not Fugue's child (stdio piping and cancel semantics change). Too different; reject.

**seccomp**: can deny socket families and dangerous syscalls but cannot express "only these paths"; GPU drivers use many ioctls and a filter must not break them. Use only as an extra layer later (e.g. deny AF_NETLINK except what is needed, or ptrace). [U] on a safe list for CUDA.

**Network per pack.** The manifest does have `needs.network` (bool, 6.3) and `needs.files` (reads or writes outside the run folder). Policy: `network:false` -> deny all (Landlock TCP/UDP deny plus unix-socket denial; or `--unshare-net`); `network:true` -> allow, and say so in the UI. Declared, not enforced today, so in private mode Fugue must enforce `false` (default when absent) rather than trust it. Open point: Landlock cannot restrict by destination, so "network true" is all or nothing; per-host allow needs a proxy or netns+nftables (needs root or slirp4netns/pasta [U]).

**Model download line.** Per the epic, model downloads are Fugue's job (public data, may stay shared); packs never need the network for weights in private mode. A pack that downloads weights itself (HF `from_pretrained`) would need `needs.network` and writes to a cache dir: redirect `HF_HOME` into the vault or fail with a clear error. [U: whether Fugue downloads for every pack kind; I read only the epic and 6.x].

**uv `setup`** (6.8) runs `uv sync` with network and writes to the pack folder and uv cache: it is a different phase from run. Run it with network on and writes limited to the pack folder + uv cache (redirected into the vault in private mode), shown to the user as such; the run phase is the strict one.

**Other leak surfaces.**
- Swap: Fugue cannot encrypt swap. `mlock`/`MCL_FUTURE` keeps pages out of swap but is limited by RLIMIT_MEMLOCK for unprivileged processes and does not stop suspend-to-disk copies of RAM (https://man.archlinux.org/man/mlock.2). Tensors from child programs are not Fugue's to lock. zram is RAM, safe if its backing device is not set [U]. The honest action: detect disk swap in `/proc/swaps` and warn in the UI (the epic already says to state it). The ArchWiki swap-encryption page was blocked; recommend dm-crypt random-key swap [U].
- Core dumps: with `core_pattern` piping to systemd-coredump, `RLIMIT_CORE=0` is ignored (core(5)), so use `prctl(PR_SET_DUMPABLE, 0)` in Fugue and in the sandbox helper before exec (inherited across fork, reset to dumpable by a normal execve unless the helper keeps it; so set RLIMIT_CORE=0 too, and for children rely on the helper calling it right before exec is useless [U]: verify `/proc/PID/status` and a test crash). Storage=none in coredump.conf is system config, not ours. Stored cores default to /var/lib/systemd/coredump (https://man.archlinux.org/man/coredump.conf.5), outside the vault: this is a real leak the UI should name.
- journald: anything a process prints to stdout/stderr that is a systemd service or a Qt log goes to the journal when started from a unit/desktop file. Fugue should not log prompts/paths, and in private mode route logs into the vault; children's stderr is already captured by Fugue (pipes), so it is a Fugue logging policy. [U on what the launcher does when started from the app menu: stdout of a desktop-launched app can reach the journal via the compositor/session].
- Clipboard: Wayland clipboard managers (cliphist etc. on Omarchy) can persist what Fugue copies. Fugue cannot prevent it; it can avoid copying in private mode or mark the offer with the `x-kde-passwordManagerHint=secret` MIME type some managers honour [U]. State in UI.
- Thumbnails: file managers write `~/.cache/thumbnails` for the files they show (thumbnail spec; fetch redirected, not read, U). Vault inside a FUSE mount is still thumbnailed by the file manager if opened there. Fugue can only write files with a `.nomedia`-like hint where supported [U]; document it.

## Test plan

1. Hostile command pack (shell or python), run under `--private`, vault = tmp dir, with `needs.network:false`: tries (a) `touch ~/pwn`, `echo > ~/.cache/x`, `/tmp/x` outside the temp root; (b) read `~/.ssh/id_*`, `~/.config` (create fake files first); (c) TCP connect to 127.0.0.1 and to a public IP, UDP send, DNS lookup, connect to `$XDG_RUNTIME_DIR/bus` and an abstract socket; (d) spawn a child that does the same; (e) `ptrace`/signal Fugue; (f) rewrite its own pack folder. Assert every one fails and that only vault + temp root changed (inotifywait -m -r on `$HOME`, `/tmp`, `/var/tmp`, or `strace -f -e trace=file`).
2. Positive tests: a GPU node (nvidia-smi, then a small torch CUDA matmul and a Vulkan `vulkaninfo`) works; ffmpeg encode/decode with vault paths works; a uv venv pack runs with `UV_CACHE_DIR`, `HOME` redirected; `needs.network:true` pack can connect.
3. Degradation: run with Landlock masked (older ABI, simulate via ABI cap in helper) and verify the UI shows "Partly sandboxed" or "Not sandboxed", never "sandboxed". Native pack shows "Not sandboxed".
4. Core dump test: crash a pack, confirm no core appears outside the vault (or that the UI warned).
5. Keep it as a CI guard (the epic's inotify test) so a new write path fails the build.

## Open questions

- Does Fugue need the GPU nodes' read access to whole system library dirs (`/usr`, `/opt/cuda`), or can the read set be tighter? Landlock read of `$HOME` secrets denied by default is the real goal; everything outside `$HOME` can stay read-only.
- Which user-visible read paths must be allowed: input files picked from outside the vault (copy in first, or per-run read grants)? `needs.files` suggests a grant list per run.
- Minimum kernel policy: refuse private mode, or degrade with a visible warning, when Landlock is missing? Recommend warn + "Not sandboxed".
- Per-destination network allow for `needs.network:true` nodes (remote APIs): out of scope for slice 1.
- Does Landlock hold for `uv sync`/pip builds that run compilers (writes to /tmp, many paths)? [U] test.
- Real torch/CUDA and Vulkan runs inside the sandbox on this machine and on the GB10: not tested.

## Sources

- https://docs.kernel.org/userspace-api/landlock.html
- https://raw.githubusercontent.com/containers/bubblewrap/main/README.md
- https://man.archlinux.org/man/bwrap.1
- https://man.archlinux.org/man/systemd.exec.5
- https://man.archlinux.org/man/systemd.resource-control.5
- https://docs.flatpak.org/en/latest/sandbox-permissions.html
- https://raw.githubusercontent.com/netblue30/firejail/master/README.md
- https://man.archlinux.org/man/core.5
- https://man.archlinux.org/man/coredump.conf.5
- https://man.archlinux.org/man/mlock.2
- Local measurements on this machine (kernel 7.2.15, bwrap 0.13.1, RTX 3090), 2026-11-22; harness in the scratchpad `ll/ll.py`.
