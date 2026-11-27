# Testing

How to run the tests and what a test here is for. The rules keep the whole suite near a minute, so it is run
before every commit.

## Running

```sh
bin/test                    # the whole suite: builds, then ctest -j<cores> (arguments go to ctest)
bin/test --changed          # only the tests the change since origin/main can affect
bin/test --changed main     # ... since another base
bin/test test_a test_b      # only these tests (and their shards): builds just them
bin/build test_menus        # one test program; run it with ctest -R test_menus or build/test_menus
```

`bin/test --changed` asks `bin/test-select` which tests the changed files (since the merge base, the working
tree and untracked files) can affect, prints the list, builds only those and runs them. A test file maps to
itself; `src/<area>/` to `tests/<area>/`, the tests that include the changed header and, for a `.qml` file,
the tests that name it, plus `qmllint` (four chunks run at the same time, and with `--changed` they lint only
the changed `.qml` files; the whole suite lints all of them); a change to the engine, the CLI or the nodes also runs the tests that
start the built `fugue` (`tests/cli`, `tests/nodes`); any change under `src/` also runs `fugue_tests`, the English-catalog check that fails on a new `tr()`/`qsTr()` string missing from `src/i18n/en.json`. A shared file (build files, resources, `Main.qml`,
core headers, test helpers, templates) runs the whole suite, and a change no test covers (documentation) runs
nothing. It errs towards running more. Run the whole suite before you merge.

`bin/build` builds the engine, core, cli and nodes libraries as shared objects (`-DFUGUE_SHARED_LIBS=ON`, the
default there): an edit to an engine or node source relinks one library instead of ~215 programs (a one-file
engine edit: 9 s static, 3 s shared). Editing a widely included header is compile-bound either way (about 4
minutes for `engine/registry.h`). For a static build, like the package's, run `FUGUE_SHARED_LIBS=OFF bin/build`
in a fresh `build/` (the choice is cached in the folder). A test that calls a library a library only links
privately (libarchive, zlib, Vulkan) must link it itself, as `test_paint_file` does.

A test never opens a window on the desktop: every test binary, run by ctest or by hand (`build/test_x`, a grab
run with a shots folder), takes the offscreen platform and has no display to reach (`tests/test_environment.cpp`).
Set `FUGUE_TEST_ON_SCREEN=1` only to watch a test on purpose. The same file switches off the FFmpeg backend's GPU
decoder probe (VAAPI opened the render node in the first window of every test: 1 to 3 s, more when 12 tests did it at once). `build/fugue` itself has no such guard: start it
offscreen (`QT_QPA_PLATFORM=offscreen`) unless a person asked to see it.

Offscreen has no GPU, so blur, frost and other effects only show their software fallback there. To see the real
window as a person does, without touching the desktop, use `bin/look-shot <out.png> [--size WxH] [--wait S] --
<fugue arguments>`: it runs Fugue in a hidden (headless) sway session on the real GPU, takes the picture with grim
and stops both (a scratch `FUGUE_HOME` or a temporary home; never the owner's store).

`bin/build` and `bin/test` share one machine-wide lock, so only one big build or run happens at a time.
A new build folder uses Ninja and lld when they are installed (a folder made with Make keeps Make; delete
`build/` to move). The window's QML (`src/**/*.qml`) is compiled ahead of time into the `Fugue` module (`qt_target_qml_sources` in
`CMakeLists.txt`, same `qrc:/` paths): a plain `.qml` resource is compiled again by every window, and Qt keeps no
disk cache for resources, so a cold checkout and a warm one cost the same. The other resources are built once for all tests, so a `Main.qml` edit rebuilds in
seconds.

The slowest test programs are sharded: `test_canvas_view_window.3of8` runs every 8th test function of the
program, starting at the 3rd (`FUGUE_TEST_MAIN`, `tests/test_shard.h`; the counts are in `CMakeLists.txt`,
`fugue_test_shards`). `ctest -R test_canvas_view_window` runs them all. A test that shares a fixed file
or folder between its functions must not use a path shared with another process (use the process id or the
test's own temporary folder), because shards run at the same time.

## What to test

1. Test only what is worth testing: logic, contracts, data handling, a fixed bug's regression. Not framework
   behaviour, trivial getters, pixel layout, anything a type or qmllint already guarantees, or a duplicate of
   another test's assertion.
2. A test file takes about 5 s alone and never over 10 s in the parallel suite; the whole suite stays near a
   minute. Test logic at the model or engine seam without a window. Window tests are for what only a window
   shows.
3. A slow test is a design problem (what cost time here, 2026-11-25: the GPU probe above, QML compiled per window, qmllint
   on one pathological function (`InputWidget.qml`, 23 s: build rows in a function with plain `var` parameters), fixed
   `qWait`s where a signal or condition exists, and the settle loop of every `click()` helper): wait on conditions (`QTRY_*`, signals), not fixed sleeps; share setup in
   `initTestCase`; shrink inputs; test a lower seam; split the file. Never raise a timeout to make a test
   pass.
4. Prune on sight. Delete a test that duplicates another, can never fail, covers removed behaviour, or is
   flaky and not worth fixing, in the same change, and say so in the PR body.
5. A flaky test is fixed or deleted, never re-run until it is green.
6. Never assert a CPU-time or wall-clock limit on a window: it measures the machine. Count the work (items made), or
   compare with a measurement of the same process. Click only on a laid-out window (`grabWindow()` runs the polish
   that moves a list's rows), and wait on state. ctest runs with `QV4_GC_TIMELIMIT=100000` (CMakeLists.txt): Qt's sliced
   GC under load made a Question read its theme as null in 1 run in 50 (the Question now reads `App.theme.look` directly, #320; the setting stays for other components).
7. A change that adds more than about 2 s to the suite says so in its PR body.
8. The moc trap (#617): moc reads a raw string literal as code. In a `.cpp` with `Q_OBJECT` (a test class), a raw
   string that spans lines and has a line with `//` before a `"` (a URL in JSON, `"source": "https://..."`) makes
   moc treat the rest of the line as a comment: it prints "No relevant classes found", emits nothing, and the link
   fails with "undefined reference to vtable". Put the URL on the string's last line, build it in pieces
   (`QStringLiteral("https:") + "//..."`), or write the text to a file. `bin/check-quick` finds this in under a
   second (no build) and `bin/test` runs it first, so it fails with the file and line.
9. `bin/check-quick` also fails a branch that moves the `templates` submodule backwards (#641): the new commit
   must contain `origin/main`'s (`git merge-base --is-ancestor`). A stale submodule checkout that a `git commit -a`
   recorded did this once (#622). In a worktree run `git submodule update --init` after you merge main; a real
   rollback says `templates rollback` in a commit message. PRs from this repository start no hosted CI, so
   `bin/test` is where the guard runs.

## CI

The full suite runs locally (`bin/test`) before every merge by the owner and agents, so a hosted run repeats that
only where it proves something new. Pull requests from forks have not been through the local suite, so they get a
fast lane. One check, `ci-ok`, covers both kinds of pull request, and branch protection requires it.

| Event | Workflow | What runs |
|---|---|---|
| Pull request from a fork | `ci.yml` | Fast lane: `test.yml` for x86_64 only (build and full suite). The first run waits for the maintainer's approval. |
| Pull request from this repo | `ci.yml` | Nothing but `ci-ok`, which passes at once. |
| Pull request that changes `test.yml` or `packaging/PKGBUILD` | `test.yml` | x86_64 and aarch64. |
| Weekly cron on main | `test.yml` | x86_64 and aarch64; saves the compiler caches. |
| Tag `v*` | `test.yml`, `package.yml`, `appimage.yml`, `helper-tarballs.yml` | The full release matrix, CUDA included. |
| `workflow_dispatch` | any workflow | On demand on a branch. `ci.yml` has `force_fast_lane` to run the fast lane on a branch of this repo. |

Only main, weekly, tag and manual runs save the compiler cache; a pull request restores it and never saves.
A new push cancels that pull request's older run. `ci.yml` uses plain `pull_request`, with no secrets and a
read-only token, so a fork's code never runs with the repo's permissions. That run uses the pull request's own workflow
files, so a fork that edits `.github/` can make `ci-ok` pass: the maintainer approves every external contributor's run
after reading the diff, and a fork's pull request that touches `.github/` is reviewed by hand, its `ci-ok` not trusted.

## Workflow lint

`workflows_lint` (tests/workflows_lint.sh) runs actionlint, and the shellcheck it applies to every `run:` block, over
`.github/workflows`. A workflow error otherwise shows only when GitHub runs it, and for the release jobs that is only on a
tag. It uses `actionlint` from PATH, else the pinned `rhysd/actionlint` image if podman already has it, and is skipped
otherwise (`podman pull docker.io/rhysd/actionlint:1.7.7` once to enable it).

## Review

Every pull request is reviewed by GitHub Copilot code review once, when it opens (the repository ruleset "Copilot review on open"). After a round of fixes, ask for another review with `gh pr edit <n> --add-reviewer @copilot`. What it looks for is in `.github/copilot-instructions.md`. Copilot reads that file from the pull request's branch, and it does not read replies to its comments. A pull request merges when no High or Medium finding is left unanswered. Low findings are fixed or filed as issues.
