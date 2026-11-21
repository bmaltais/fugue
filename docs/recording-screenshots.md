# Recording the README's screenshots

The pictures in `docs/images/` are drawn by Fugue's own window with Qt's offscreen platform:
no compositor, no desktop session, no pointer. Each is a test that skips unless `FUGUE_SHOT_DIR`
names a folder, so CI runs them as skips. Nothing in them reads your files: the window, its
settings and templates folder are the test's own, and the templates are the bundled ones
(`git submodule update --init` first).

```sh
bin/build test_readme_shots test_canvas_run
export FUGUE_SHOT_DIR=$PWD/docs/images QT_QPA_PLATFORM=offscreen
build/test_readme_shots                                   # the five below
build/test_canvas_run savesFinishedWorkflowsWhenAsked     # results.png, results-light.png
```

| File | Test |
| --- | --- |
| `hero.png`, `hero-light.png` | `test_readme_shots hero` (`FUGUE_SHOT_TEMPLATE=<folder>` picks another template) |
| `templates.png` | `test_readme_shots templatesDialog` |
| `node-library.png` | `test_readme_shots nodeLibrary` |
| `group.png` | `test_readme_shots group` |
| `results.png`, `results-light.png` | `test_canvas_run savesFinishedWorkflowsWhenAsked` |
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
| `tour-run-template.avif` | `tours/run-template.sh` | File > Browse templates, FLUX.2 Klein 4B, Run: a real generation that finishes in the sandbox (the models are read in place from the settings' model folders) |
| `tour-group.avif` | `tours/group.sh` | open a template, marquee three nodes, Node > Make a group (the sandbox's settings use the standard navigation so a left-drag selects) |

`devkit.conf` sets the sandbox: Fugue gets an empty home with a copy of your settings file (so
the models are found in place, never written), and the recorder fails the run if your real
`~/.config/fugue`, `~/.local/share/fugue` or `~/Documents` change. Fugue draws with software GL
there, so a tour waits (`sleep`, `wait_until`) generously; a run takes 2 to 4 minutes. Left-drag
and Tab work; the devkit has no key chords (Ctrl+...), so a tour uses the menus. Look at each
contact sheet before committing: no personal paths, no error banner, no blank frame.
