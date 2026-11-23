# Testing

How to run the tests and what a test here is for. The rules keep the whole suite near a minute, so it is run
before every commit.

## Running

```sh
bin/test                    # the whole suite: builds, then ctest -j<cores> (arguments go to ctest)
bin/test --changed          # only the tests the change since origin/main can affect
bin/test --changed main     # ... since another base
bin/build test_menus        # one test program; run it with ctest -R test_menus or build/test_menus
```

`bin/test --changed` asks `bin/test-select` which tests the changed files (since the merge base, the working
tree and untracked files) can affect, prints the list, builds only those and runs them. A test file maps to
itself; `src/<area>/` to `tests/<area>/`, the tests that include the changed header and, for a `.qml` file,
the tests that name it, plus `qmllint`; a change to the engine, the CLI or the nodes also runs the tests that
start the built `fugue` (`tests/cli`, `tests/nodes`). A shared file (build files, resources, `Main.qml`,
core headers, test helpers, templates) runs the whole suite, and a change no test covers (documentation) runs
nothing. It errs towards running more. Run the whole suite before you merge.

`bin/build` and `bin/test` share one machine-wide lock, so only one big build or run happens at a time.
A new build folder uses Ninja and lld when they are installed (a folder made with Make keeps Make; delete
`build/` to move). The window's resources are built once for all tests, so a `Main.qml` edit rebuilds in
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
3. A slow test is a design problem: wait on conditions (`QTRY_*`, signals), not fixed sleeps; share setup in
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
