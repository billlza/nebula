# Nebula v1.0.0

Nebula 1.0.0 is the first formal GA release of the compiler, CLI, and project tooling. Core
binary archives install the compiler/CLI, bundled `std`, runtime headers, and release docs.

The optional Linux backend SDK asset installs `nebula-service` and `nebula-observe` as backend SDK
GA packages. It also ships `nebula-db-sqlite` as an installed-preview package for Linux x86_64;
remaining `official/*` packages stay repo-local preview/pilot surfaces consumed from a source
checkout.

## Highlights

- Stable language/tooling baseline for small multi-file Nebula programs, with the primary 1.0
  story centered on CLI / system tools.
- Deterministic local package publishing and exact-version local registry consumption.
- Workspace-root lock resolution across `fetch`, `update`, `check`, `build`, `run`, `test`,
  and `bench`.
- Bundled `std::fs`, `nebula run ... -- <program-args...>`, and a file-oriented CLI starter path
  for real command-line tool authoring.
- Comment-preserving formatter and project-aware LSP hover/definition.
- Release assets for macOS, Linux, and Windows, plus Homebrew and installer entrypoints.

## Included release artifacts

- `nebula-v1.0.0-darwin-x86_64.tar.gz`
- `nebula-v1.0.0-darwin-arm64.tar.gz`
- `nebula-v1.0.0-linux-x86_64.tar.gz`
- `nebula-v1.0.0-windows-x86_64.zip`
- `nebula-backend-sdk-v1.0.0-linux-x86_64.tar.gz` (optional Linux backend SDK)
- `nebula-v1.0.0-<target>.spdx.json`
- `nebula-backend-sdk-v1.0.0-linux-x86_64.spdx.json`
- `nebula-v1.0.0-<target>.provenance.intoto.jsonl`
- `nebula-backend-sdk-v1.0.0-linux-x86_64.provenance.intoto.jsonl`
- `nebula-v1.0.0-<target>.sbom.intoto.jsonl`
- `nebula-backend-sdk-v1.0.0-linux-x86_64.sbom.intoto.jsonl`
- `SHA256SUMS.txt`
- `SHA256SUMS.txt.intoto.jsonl`
- `release-manifest.json`
- `nebula.rb`

## Compatibility notes

- This release line is the `1.0.x` compatibility branch.
- Post-release development on `main` advances to `v1.1.0-dev`.
- Patch releases such as `1.0.1` are cut from `release/1.0`.

## Verification baseline

- Strict build passes with warnings-as-errors.
- Current local full-suite baseline before tag: `487/487`.
- Release verification covers archive provenance, SPDX SBOM attestations, and checksum attestation.
- Final tag gate remains the supported-matrix release checklist in `docs/rc_checklist_v1.0.md`.
