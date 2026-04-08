# Nebula Release Process

This document defines the release procedure for Nebula `1.0.x`.

## 1. Release branches

- `release/1.0` is the stable patch line for `1.0.0`, `1.0.1`, and later `1.0.x` releases.
- `main` carries forward-looking work and is bumped to `v1.1.0-dev` immediately after the
  `v1.0.0` release is cut.

## 2. Pre-release gates

Before tagging a release:

1. `VERSION` matches the intended release version.
2. Strict build passes on the supported matrix.
3. Contract suite passes on the supported matrix.
4. Release workflow dry-run produces:
   - platform archives
   - `SHA256SUMS.txt`
   - `release-manifest.json`
   - `nebula.rb`
5. Release notes for the target version exist.

## 3. 1.0.0 GA cut

1. Branch from the release candidate state to `release/1.0`.
2. Ensure `VERSION` is `1.0.0`.
3. Tag the release commit as `v1.0.0`.
4. Run the release workflow from the tag.
5. Publish GitHub Release assets and notes from `RELEASE_NOTES_v1.0.0.md`.

## 4. Post-1.0.0 step

After `v1.0.0` is published:

1. Bump `main` version markers to `v1.1.0-dev`.
2. Keep `release/1.0` at `1.0.0` until patch work is needed.

## 5. Patch releases (`1.0.1+`)

1. Cherry-pick or implement the patch on `release/1.0`.
2. Update `VERSION` to the patch version, for example `1.0.1`.
3. Add release notes for that patch version.
4. Re-run strict build, full contract suite, and release dry-run.
5. Tag `v1.0.1` from `release/1.0`.

## 6. Release asset policy

Official 1.0 artifacts are:

- `nebula-v<version>-darwin-x86_64.tar.gz`
- `nebula-v<version>-darwin-arm64.tar.gz`
- `nebula-v<version>-linux-x86_64.tar.gz`
- `nebula-v<version>-windows-x86_64.zip`
- `SHA256SUMS.txt`
- `release-manifest.json`
- `nebula.rb`

Hosted registry services, MSI installers, winget, apt packages, and external tap automation are
not part of the 1.0 release contract.
