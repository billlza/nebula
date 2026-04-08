# Changelog

All notable changes to Nebula are documented in this file.

The format follows a simple release-oriented structure and keeps the `1.0.x`
patch line separate from future `1.1.x` development work.

## [1.0.0] - 2026-04-08

### Added
- Formalized Nebula 1.0 CLI surface with `publish`, workspace-aware `test`/`bench`,
  comment-preserving `fmt`, explain, and minimal LSP.
- Local deterministic registry publishing/consumption flow with immutable version semantics.
- Workspace root lock resolution for `fetch/update/check/build/run/test/bench`.
- Release packaging, installer, Homebrew formula, and release workflow support for
  macOS, Linux, and Windows.

### Changed
- Versioning is now sourced from the repository `VERSION` file and released as `v1.0.0`.
- Documentation, specs, and contract harness text are aligned to the 1.0 GA narrative.
- Contract and CI gating now treat multi-platform strict builds and release packaging
  artifacts as first-class release concerns.

### Stability
- Strict build passes with warnings-as-errors.
- Contract suite baseline: `287/287`.

## Earlier releases

- See [RELEASE_NOTES_v0.5.0](./RELEASE_NOTES_v0.5.0.md) and earlier archived notes for
  pre-1.0 milestones.
