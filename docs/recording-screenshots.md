# Recording the README's screenshots

The pictures in `docs/images/` are drawn by Fugue's own window with Qt's offscreen platform:
no compositor, no desktop session, no pointer. Each is a test that skips unless `FUGUE_SHOT_DIR`
names a folder, so CI runs them as skips. Nothing in them reads your files: the window, its
settings and templates folder are the test's own, and the templates are the bundled ones
(`git submodule update --init` first).

```sh
bin/build test_readme_shots test_canvas_run
export FUGUE_SHOT_DIR=$PWD/docs/images QT_QPA_PLATFORM=offscreen
build/test_readme_shots                                   # the ones below
build/test_canvas_mask_node saveTheEditorOnTheTemplatesPicture   # mask-editor.png
build/test_canvas_run savesFinishedWorkflowsWhenAsked     # results.png, results-light.png
```

| File | Test |
| --- | --- |
| `hero.png`, `hero-light.png` | `test_readme_shots hero` (`FUGUE_SHOT_TEMPLATE=<folder>` picks another template) |
| `templates.png` | `test_readme_shots templatesDialog` |
| `node-library.png` | `test_readme_shots nodeLibrary` |
| `group.png` | `test_readme_shots group` |
| `results.png`, `results-light.png` | `test_canvas_run savesFinishedWorkflowsWhenAsked` |
| `inpaint-graph.png`, `outpaint-graph.png` | `test_readme_shots maskTemplates` |
| `mask-editor.png` | `test_canvas_mask_node saveTheEditorOnTheTemplatesPicture` (the editor open on the Inpaint template's Load Image; the window, not trimmed) |
| `mask-paint-node.png` | `test_readme_shots paintMaskNode` |
| `mask-group.png` | `test_readme_shots maskGroup` (inside Inpaint (crop and stitch)) |
| `inpaint-result.png` | not a test: the sample, a real result of the template and their difference side by side, from a `fugue run` on the 3090 (ImageMagick) |
| `menubar-out.png`, `menubar-handle-hover.png`, `menubar-in.png`, `menubar-in-menu.png` | `test_chrome_window picturesOfTheBarOutAndIn` (top-left crops for reading, not committed) |

The graph shots are trimmed to the nodes in the test (`tests/readme_shot.h`). The results come
from the fake provider, which hands back the picture the z-image-turbo template ships as its
preview (made with Fugue from the same prompt). Re-make the pictures after a change to the look,
and read each PNG before committing: no personal paths, no error banner, nothing cut off.

## Animated clips

The clips (`docs/images/tour-*.avif`) are tours of the real window, recorded by the
[omarchy-plugin-devkit](https://github.com/bmaltais/omarchy-plugin-devkit) (the `scripts/devkit`
submodule, `git submodule update --init`). Fugue runs in the devkit's **headless sway backend**: no
window, monitor, focus or seat on your desktop, so your pointer and keyboard stay yours. Do not
use the devkit's `pointer`, `DEVKIT_BACKEND=hyprland`, `DEV_WINDOW` or `DEV_WATCH` for this: those
use a window of your compositor and can move your real pointer.

```sh
export DEVKIT_BACKEND=headless                   # the default for an app; says it twice
nice bin/build                                   # or FUGUE_BIN=/path/to/fugue (default: build/fugue)
scripts/devkit/bin/dev status                    # "down" or "up headless ..." before you start
scripts/devkit/bin/tour tours/wire.sh            # records into /tmp/tour-wire.avif
scripts/devkit/bin/sheet /tmp/tour-wire.avif     # a contact sheet to look at (then Read the PNG)
scripts/devkit/bin/tour tours/wire.sh --install  # copies it to docs/images/
```

| Clip | Tour | Shows |
| --- | --- | --- |
| `tour-wire.avif` | `tours/wire.sh` | node search (Tab), add Number and Preview, drag a link between their ports (a crop of the canvas) |
| `tour-run-template.avif` | `tours/run-template.sh` | File > Browse templates, a template whose models the machine has, Run: a real generation that finishes in the sandbox (the models are read in place from the settings' model folders) |
| `tour-group.avif` | `tours/group.sh` | open a template, rubber-band a block of nodes, Node > Make a group (the sandbox's settings use the standard navigation so a left-drag selects) |

### How a tour finds its targets

No tour clicks a fixed pixel. `tours/lib.sh` (sourced by each tour) asks the window where an item is, by `objectName`, the
way the shot tests find items:

- **The probe** (`src/tour_probe.cpp`, the one app-side hook): when `FUGUE_TOUR_PROBE` names a folder (the tours set it in
  `tour_prepare`; it is unset otherwise, so the app does nothing), a 150 ms timer answers the queries in `<folder>/ask` in
  `<folder>/reply`: per query, each visible item with that objectName and its rectangle in window coordinates. A query is
  names joined by `/` (searched below one another) and `*` is a wildcard. It reads the item tree only and writes nothing
  but the reply file.
- `probe_wait`, `click_on QUERY` and `menu_command COMMAND` use it. `click_on` waits for the item to stop moving (dialogs
  slide in), then clicks its middle. `menu_command` opens the menu titles (`menuTitle-N`, a view's own `...MenuTitle-View`)
  until one holds `menuItem-<command>`, so a reordered bar still works; the devkit has no key chords. A target that never
  shows fails the tour at once, naming it.
- The templates dialog is searched by the template's id (the search matches ids), checked to leave one card, and opened with
  Return (a click on the card lets its release land on what the closing dialog uncovers).
- Ready check: `run-template.sh` and `group.sh` pick the first of `TOUR_TEMPLATE_CHOICES` whose models `fugue template list`
  says are all here (or the one in `TOUR_TEMPLATE=<id>`), before anything is recorded. When none is, they stop with the missing
  model names, so a tour never films a "Get ready" sheet (and never presses Install). `group.sh` also needs the ids of the
  nodes to select, a table at its top. On a machine whose models are all present the first choice is Klein 4B; on one with
  neither Klein it is Image Upscale (Real-ESRGAN), which needs no download.
- The run waits for the run pill (`runPillBox`) to appear and go, and fails if it stays (an error). A worktree has no
  `build-helper-<backend>/`, so `tour_find_helper` points `FUGUE_HELPER` at the main checkout's, read in place.
- Pointer moves that cannot be found by name stay: the rubber band's corners come from the nodes' rectangles, and `wire.sh`'s
  two clicks on the empty canvas (where the nodes go) are inside its crop.

`devkit.conf` sets the sandbox: Fugue gets an empty home with a copy of your settings file (so
the models are found in place, never written), and the recorder fails the run if your real
`~/.config/fugue`, `~/.local/share/fugue` or `~/Documents` change. Fugue draws with software GL
there, so a tour waits generously; a run takes 1 to 4 minutes. Left-drag and Tab work; menus are clicked. Look at each
contact sheet before committing: no personal paths, no error banner, no blank frame.
