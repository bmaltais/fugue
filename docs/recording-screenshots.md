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

The graph shots are trimmed to the nodes in the test (`tests/readme_shot.h`). The results come
from the fake provider, which hands back the picture the z-image-turbo template ships as its
preview (made with Fugue from the same prompt). Re-make the pictures after a change to the look,
and read each PNG before committing: no personal paths, no error banner, nothing cut off.

Animated tours (GIFs) cannot be made this way: the offscreen window draws still frames and has
no pointer to record.
