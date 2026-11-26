# Review guidance for Fugue

Fugue is a native C++17 / Qt 6 Quick desktop app for node-based local AI generation (a ComfyUI-like graph).
The engine is in `src/engine`, nodes in `src/nodes`, the window in `src/canvas`, `src/workflow`, `src/apps`, `src/settings` (QML + C++).
The behaviour is specified in `docs/spec.md`. Repository rules are in `AGENTS.md` and `CONTRIBUTING.md`.
Every pull request has already passed the full local test suite. Review for defects the tests did not catch.

## What matters most

Report a finding only when you can describe a concrete case: the inputs or state, then the wrong result, crash, hang, leak, or data loss.
Prefer a few real defects over many possible ones. Do not report style, naming, or formatting, or refactors that change no behaviour.

1. **Deferred results that use the wrong state.** Hashing, downloads, searches, provider jobs and timers finish later.
   Check that the completion applies to the tab, view, document, request or destination captured when the request was made,
   not whatever is current when it finishes. Check repeated clicks, a cancel then a retry, a tab closed or switched, and two requests for the same file.
2. **Threads and lifetimes.** Worker threads and QtConcurrent must not touch GUI objects or `Registry` data the GUI thread can replace.
   Look for captures of `this` without a `QPointer` or weak pointer, use-after-free when a dialog or sheet closes mid-work,
   and waits on a worker that could hang the window or block quit.
3. **Consent, privacy and the user's data.**
   - Nothing may stat, list or read a place the person did not agree to. Check symlinks (resolve without following), autofs and automounts, and network mounts.
   - Nothing may write the user's settings, models or workflow files except at the explicit yes or save.
   - Nothing may download a model the person chose not to download.
   - Tests must not write real user data (`~/.config/fugue`, `~/.local/share/fugue`, `~/.local/state/fugue`).
4. **Undo and the document.** Each user action is exactly one undo step, and undo restores exactly what was changed.
   Edits inside an entered group must not overwrite the group's definition (use the whole-document commit for whole-workflow edits).
5. **Scheduler, compiler and cache.** Lazy inputs, a Switch's chosen side, mute and bypass, frozen nodes, cache keys
   (a stale hit after an input change), and the dry run (validate, the canvas check) agreeing with a real run.
6. **Release and install.** In `install.sh`, `packaging/`, `bin/package-helper` and `.github/workflows/`: checksums verified before use,
   one writer for `SHA256SUMS`, race-free release jobs, a tag-only release path, no secrets, and plain `pull_request` (never `pull_request_target`) for fork code.
7. **Tests that pass for the wrong reason.** A fixture that never compiles, an assertion that cannot fail, a comparison on 8-bit data where 16-bit is meant,
   a wait on time instead of a condition.

## Repository rules to check

- **Spec in sync:** a behaviour change updates `docs/spec.md` in the same pull request.
- **Window UI:** a change to QML, menus or dialogs adds a decision row in `docs/ui-ergonomics.md`. It uses the Drift kit components in `src/kit/` and works by keyboard alone (focus on open, Enter, Escape, Tab order).
- **Strings:** every new user-visible string is in `src/i18n/en.json`, and removed strings are removed from it.
- **Warnings:** no new compiler or qmllint warnings.
- **Tests:** a fixed bug has a regression test, and tests stay lean (a test file runs in about 5 seconds; wait on conditions, not sleeps).

## Out of scope

- Generated files, `templates/` (a submodule), and pictures under `docs/images/`.
- The choice of AI models or prompts in templates.
