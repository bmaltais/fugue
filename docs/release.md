# How a release gets its SHA256SUMS

`install.sh` verifies every download against the release's `SHA256SUMS` (`sha256  name` lines, name without a path). Several
workflows add assets to one `v*` tag's release, so none of them edits that file: a download, edit and upload by two jobs at
once loses lines (found in review of #599).

- `package.yml` creates the release with the packages and the aarch64 helper tarball, each with a `<asset>.sha256` beside it.
- `appimage.yml` and `helper-tarballs.yml` wait for the release, then upload their assets and `<asset>.sha256` files. Nothing else.
- `release-sums.yml` is the one writer. It polls the release until every glob in `packaging/release-assets.txt` has an asset and
  its checksum file, builds `SHA256SUMS` with `packaging/release-sums.sh`, and uploads it once. After about five and a half hours (a hosted job dies at six) it fails and lists
  what is missing. When a job failed, fix it, and rerun `release-sums.yml` by hand on the tag.

A new asset kind: add its glob to `packaging/release-assets.txt` and have its job upload `<asset>.sha256`.

`tests/release_sums.sh` (CTest `release_sums`) runs the script against a fake asset folder: complete, missing, upload not
finished, disagreeing and malformed parts. The workflows themselves can only be exercised by a tag.

## A hand asset: the x86_64 CUDA pacman package

`fugue-helper-cuda-<ver>-x86_64.pkg.tar.zst` is built on a machine with an NVIDIA card (the CUDA toolkit is too big for a
hosted runner) and uploaded by hand. It is **not** in `packaging/release-assets.txt`, so nothing waits on it. Upload it with its
checksum file, never by editing `SHA256SUMS`:

```sh
sha256sum fugue-helper-cuda-<ver>-x86_64.pkg.tar.zst > fugue-helper-cuda-<ver>-x86_64.pkg.tar.zst.sha256
gh release upload <tag> fugue-helper-cuda-<ver>-x86_64.pkg.tar.zst fugue-helper-cuda-<ver>-x86_64.pkg.tar.zst.sha256 --clobber
gh workflow run release-sums.yml -R bmaltais/fugue --ref <tag>   # only if release-sums already ran
```

`release-sums.sh` includes every `.sha256` part whose asset is on the release, expected or not, so the rerun adds the line.
