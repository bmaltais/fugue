# Publishing a pack

There is no catalog or signing in v0.1: a pack is shared as a git repository (or a folder). People
install it by address and a pin.

1. **Make it a repository.** `pack.json` at the root, one folder per node, as in
   [packs](packs.md). Set `source` in `pack.json` to the repository's address and a `license`.
2. **Test it.** `fugue node test .` from the pack folder passes, and keep `test.json` in the
   repository so others can run it.
3. **Version it.** Raise `version` in `pack.json` and in each `node.json` you changed (semver). A
   workflow remembers the version of the node it was made with, so keep outputs and input names
   stable inside a major version, and put old ids in `aliases` when you rename a node.
4. **Tag it.** `git tag v1.2.0 && git push --tags`.
5. **Say what people are trusting.** Declare `network`, `files` and `gpu` in each `needs`. Say in
   the readme what the programs do and what `setup` runs. People are asked about each of them
   and then run your programs as themselves.
6. **Install it as a user does.** `fugue pack add https://github.com/you/your-pack --ref v1.2.0`
   from a clean machine or a clean `XDG_DATA_HOME`.

Updates are explicit and pinned: a user runs `fugue pack update <id> --ref v1.3.0`, sees the new
version, and is asked again about trust.
