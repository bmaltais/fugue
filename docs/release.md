# How a release gets its SHA256SUMS

`install.sh` verifies every download against the release's `SHA256SUMS` (`sha256  name` lines, name without a path). Several
workflows add assets to one `v*` tag's release, so none of them edits that file: a download, edit and upload by two jobs at
once loses lines (found in review of #599).

- `package.yml` creates the release with the packages and the aarch64 helper tarball, each with a `<asset>.sha256` beside it.
- `appimage.yml` and `helper-tarballs.yml` wait for the release, then upload their assets and `<asset>.sha256` files. Nothing else.
- `release-sums.yml` is the one writer. It polls the release until every glob in `packaging/release-assets.txt` has an asset and
  its checksum file, builds `SHA256SUMS` with `packaging/release-sums.sh`, and uploads it once. After six hours it fails and lists
  what is missing. When a job failed, fix it, and rerun `release-sums.yml` by hand on the tag.

A new asset kind: add its glob to `packaging/release-assets.txt` and have its job upload `<asset>.sha256`.

`tests/release_sums.sh` (CTest `release_sums`) runs the script against a fake asset folder: complete, missing, upload not
finished, disagreeing and malformed parts. The workflows themselves can only be exercised by a tag.
