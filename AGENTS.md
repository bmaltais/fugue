# Fugue

Window UI, changed or added — a `.qml` file, a canvas hit test, a menu, a dialog, or a widget: follow [docs/ui-ergonomics.md](docs/ui-ergonomics.md). Each new control and each control you move meets the rules in that file. Update the file in the same change. [docs/canvas-look.md](docs/canvas-look.md) and [docs/spec.md](docs/spec.md) stay the look and the behavior.

Tests: [docs/testing.md](docs/testing.md) has the rules for what to test and how fast (about 5 s per file, a
minute for the suite) and how to run it. `bin/test --changed` runs only the tests a change can affect; run
the whole `bin/test` before you merge.
