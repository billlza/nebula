# Tooling and CLI (v1.0.0)

This layer defines user-facing command behavior and stable CLI contracts.

## 1. Command model

Nebula CLI is defined by three orthogonal axes:
1. command responsibility: `check | build | run | test | bench | new | add | publish | fetch | update | fmt | explain | lsp`
2. build mode: `--mode debug|release`
3. analysis profile: `--profile auto|fast|deep`
4. project path model: commands accept a single-file source, project directory, or `nebula.toml`

## 2. Command responsibilities

- `nebula check <file.nb>`: static analysis + diagnostics only
- `nebula build <file.nb>`: compile/link artifact, never execute
- `nebula run <file.nb>`: preflight + build (if needed) + execute
- `nebula test`: run `@test` harness flow; `--dir` may name a raw source dir or a project/workspace target
- `nebula bench`: run `@bench`, emit latency/throughput summary plus stable
  `clock/platform/perf_capability/perf_reason` fields; `--dir` may name a raw source dir or a
  project/workspace target
- `nebula new <path>`: scaffold a project root with `nebula.toml` + `src/main.nb`
- `nebula add <project>`: add an exact-version, path, or git dependency entry to `nebula.toml`
- `nebula publish <project>`: publish a package into a deterministic local file-based registry
- `nebula fetch <project>`: resolve dependencies and write reproducible `nebula.lock`
- `nebula update <project>`: refresh dependency resolution and rewrite reproducible `nebula.lock`
- `nebula fmt <file-or-dir>`: rewrite Nebula source formatting in place while preserving comments
- `nebula explain <path>`: emit stable explain JSON/text for `path+span` or `symbol+file` queries
- `nebula lsp`: launch the language-server entrypoint with diagnostics publication, hover, and definition lookup

Current 1.0 GA scope:
- dependency resolution guaranteed for exact-version local-registry, `path`, and `git`
- registry resolution defaults to `<project-or-workspace>/.nebula/registry` and may be overridden
  with `NEBULA_REGISTRY_ROOT`
- `publish` writes versioned package artifacts into that local registry; identical re-publish is
  allowed, but changed contents under the same version are rejected unless `--force` is used
- `[workspace]` manifests own a shared root `nebula.lock` and member packages resolve through that
  lock for `fetch/update/check/build/run/test/bench`
- workspace-only roots resolve the first member package in sorted member-path order when a
  deterministic default target is required
- official 1.0.0 release assets are:
  `nebula-v1.0.0-darwin-x86_64.tar.gz`,
  `nebula-v1.0.0-darwin-arm64.tar.gz`,
  `nebula-v1.0.0-linux-x86_64.tar.gz`,
  `nebula-v1.0.0-windows-x86_64.zip`
- install surfaces are GitHub Release assets, `scripts/install.sh`, `scripts/install.ps1`,
  and the rendered `nebula.rb` formula

## 3. Parsing and global flags

- unknown/invalid option or extra positional args -> parse error, exit code `2`
- `check|build|run` require exactly one source argument
- `--help|-h` returns `0`
- `--version` returns `0`

## 4. Run semantics

Two-stage run:
- Stage 1 preflight: `--preflight fast|off` (default `fast`)
- Stage 2 build/link: resolved mode/profile

Run gating (`--run-gate high|all|none`) applies to preflight diagnostics only.

Artifact controls:
- `-o|--out <path>`
- `--reuse`
- `--no-build`

## 5. Diagnostics controls

Supported overlays:
- `--analysis-tier basic|smart|deep`
- `--smart on|off`
- `--diag-view raw|grouped`
- `--warn-policy strict|balanced|lenient`
- `--warn-class <class>=on|off`
- grouped/root-cause controls:
  `--diag-budget-ms`, `--diag-grouping-delay-ms`, `--max-root-causes`,
  `--root-cause-v2`, `--root-cause-top-k`, `--root-cause-min-covered`

## 6. Exit-code contract

- parse failures: `2`
- `check|build|test|bench`: non-zero when errors exist
- `run`: if execution starts, return child artifact exit code

## 7. Layer boundaries

- This file is the normative Tooling/CLI layer.
- Diagnostic schema and code taxonomy: `spec/diagnostics.md`.
- Cache/reuse/grouping internals and perf baseline tooling: `spec/experimental_infra.md`.
