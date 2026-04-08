# Nebula v1.0.0

Nebula 1.0.0 is the first formal GA release of the compiler, CLI, and project tooling.

## Highlights

- Stable language/tooling baseline for small multi-file Nebula programs.
- Deterministic local package publishing and exact-version local registry consumption.
- Workspace-root lock resolution across `fetch`, `update`, `check`, `build`, `run`, `test`,
  and `bench`.
- Comment-preserving formatter and project-aware LSP hover/definition.
- Release assets for macOS, Linux, and Windows, plus Homebrew and installer entrypoints.

## Included release artifacts

- `nebula-v1.0.0-darwin-x86_64.tar.gz`
- `nebula-v1.0.0-darwin-arm64.tar.gz`
- `nebula-v1.0.0-linux-x86_64.tar.gz`
- `nebula-v1.0.0-windows-x86_64.zip`
- `SHA256SUMS.txt`
- `release-manifest.json`
- `nebula.rb`

## Compatibility notes

- This release line is the `1.0.x` compatibility branch.
- Post-release development on `main` advances to `v1.1.0-dev`.
- Patch releases such as `1.0.1` are cut from `release/1.0`.

## Verification baseline

- Strict build passes with warnings-as-errors.
- Contract suite baseline at release cut: `287/287`.
