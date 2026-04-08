# Nebula (v1.0.0)

Nebula is a minimal language + compiler pipeline focused on:
- **Region allocation** as an explicit allocation domain (`region R { ... }`)
- **Ownership inference** (bootstrap) with explicit user overrides (`shared`, `unique`, `heap`, `promote`)
- **Unsafe boundary contract** (`@unsafe fn` + `unsafe { ... }`)
- **Boolean/control-flow core**: `Bool`, comparisons/logical operators, `if` / `else`
- **Multi-file project model**: `module`, `import`, and `nebula.toml` entry manifests
- **Thin host support**: built-in `print`, `assert`, `argc`, `argv`, plus `extern fn` declarations
- **Struct ergonomics (minimal)**: field read/write (`x.f`, `x.f = v`), variable reassignment (`x = v`),
  method sugar (`x.m(...)` -> `Type_m(x, ...)`)
- **Conservative safety assist layer**: borrow/exclusivity diagnostics (`NBL-T09x`) and related
  checks are intentionally secondary to the language core narrative (see `spec/static_analysis.md`)
- **Epistemic linting** (performance/latency-oriented diagnostics)
- **Three-axis CLI model**:
  - command responsibility (`check` / `build` / `run`)
  - build mode (`--mode debug|release`)
  - analysis depth (`--profile auto|fast|deep`)

This repo implements a practical compiler in **C++**:

Source → AST → Typed AST → NIR/CFG → EscapeAnalysis → Rep×Owner inference → C++23 → clang++

## Build

From `nebula/`:

```bash
cmake -S . -B build
cmake --build build -j
```

The compiler binary is `build/nebula`.

## Install

Nebula 1.0.0 release assets cover:

- `nebula-v1.0.0-darwin-x86_64.tar.gz`
- `nebula-v1.0.0-darwin-arm64.tar.gz`
- `nebula-v1.0.0-linux-x86_64.tar.gz`
- `nebula-v1.0.0-windows-x86_64.zip`

GitHub Release is the official distribution source. Homebrew and the install scripts resolve from
those same artifacts.

Install with the shipped scripts:

```bash
bash scripts/install.sh --version 1.0.0
pwsh -File scripts/install.ps1 -Version 1.0.0
```

Defaults:

- Unix install location: `$HOME/.local/bin/nebula`
- PowerShell install location: `$HOME\\.local\\bin\\nebula.exe`
- Default release source: `https://github.com/billlza/nebula/releases/download/v<version>`

Supported overrides:

- `--prefix` or `NEBULA_INSTALL_PREFIX`
- `--version` or `NEBULA_INSTALL_VERSION`
- `--repo` or `NEBULA_INSTALL_REPOSITORY`
- `--base-url` or `NEBULA_INSTALL_BASE_URL`

For local smoke, release dry-runs, or custom mirrors, point the installers at a local artifact directory:

```bash
bash scripts/install.sh --version 1.0.0 --prefix "$PWD/dist/install-smoke" --base-url "$PWD/dist"
pwsh -File scripts/install.ps1 -Version 1.0.0 -InstallPrefix "$PWD/dist/install-smoke" -BaseUrl "$PWD/dist"
```

Homebrew formula output is published as `nebula.rb` on each release. The release workflow also
smoke-tests a local-file formula before publishing.

## CLI

- `nebula check <path>`: static analysis and diagnostics for a file, project dir, or `nebula.toml`
- `nebula build <path>`: compile and link
- `nebula run <path>`: preflight, build, execute
- `nebula test`: run `@test` harness; `--dir` accepts a raw source dir or a project/workspace root
- `nebula bench`: run `@bench` harness with stable clock/platform/perf capability output; `--dir`
  accepts a raw source dir or a project/workspace root
- `nebula new <path>`: scaffold a minimal Nebula project
- `nebula add <project> ...`: add an exact-version, path, or git dependency to `nebula.toml`
- `nebula publish <project>`: publish a package into a deterministic local file-based registry
- `nebula fetch <project>`: resolve dependencies and write `nebula.lock`
- `nebula update <project>`: refresh dependency resolution and update `nebula.lock`
- `nebula fmt <file-or-dir>`: rewrite Nebula sources into canonical formatting while preserving comments
- `nebula explain <path>`: query explain data in text or JSON via `path+span` or `symbol+file`
- `nebula lsp`: start the minimal language server entrypoint with diagnostics, hover explain,
  and definition lookup

Run `./build/nebula` to see full CLI flags.

Current 1.0 package-manager GA scope includes exact-version local-registry, `path`, and `git`
dependencies plus a reproducible `nebula.lock`. Registry resolution defaults to
`<project-or-workspace>/.nebula/registry` and can be overridden with `NEBULA_REGISTRY_ROOT`.
`[workspace]` manifests now participate in shared root-lock resolution for member-package
`fetch/update/check/build/run/test/bench`. Workspace-only roots resolve the first member package in
sorted member-path order as the deterministic default target. `nebula publish` writes deterministic
local-registry artifacts; hosted registry services remain out of scope.

Official 1.0.0 binary support is:

- macOS x86_64
- macOS arm64
- Linux x86_64
- Windows x86_64

See `spec/SPEC.md` for layered docs:
- Language Core: `spec/language_core.md`
- Static Analysis: `spec/static_analysis.md`
- Tooling/CLI: `spec/tooling_cli.md`
- Experimental/Infra: `spec/experimental_infra.md`

## Release metadata

- Version source of truth: `VERSION`
- License: Apache-2.0 in `LICENSE`
- Changelog: `CHANGELOG.md`
- Current release notes: `RELEASE_NOTES_v1.0.0.md`
- Release process: `RELEASE_PROCESS.md`

## Project quick start

Scaffold and run a minimal project:

```bash
./build/nebula new /tmp/hello-nebula
./build/nebula run /tmp/hello-nebula --run-gate none
```

Example project layout:

```text
hello-nebula/
  nebula.toml
  src/
    main.nb
    util.nb
```

Package imports use dependency-qualified module paths:

```nebula
import dep::util
```

And dependencies are declared in `nebula.toml`:

```toml
schema_version = 1

[package]
name = "hello-nebula"
version = "0.1.0"
entry = "src/main.nb"
src_dir = "src"

[dependencies]
dep = { path = "../dep" }
```

After adding dependencies, resolve them before `run/build/check`:

```bash
./build/nebula fetch /tmp/hello-nebula
./build/nebula run /tmp/hello-nebula --run-gate none
```

To seed a shared local registry from a package:

```bash
./build/nebula publish /tmp/dep
./build/nebula publish /tmp/dep --force
```

Re-publishing identical contents is a no-op. Changing contents under the same version is rejected
unless `--force` is passed.

## Contract Tests

Nebula v1.0.0 ships a CLI/diagnostics contract suite:

```bash
python3 tests/run.py --suite all --report text
```

You can run specific slices with `--suite` (`check|build|run|test|bench|safety`) and filter by case id with `--filter`.

## Cross-stage baseline

For a deterministic off-vs-safe comparison on run-stage cross-stage reuse:

```bash
./scripts/run_cross_stage_baseline.sh
```

This command does not build; it expects `build/nebula` to already exist and writes:
- `benchmark_results/cross_stage_reuse_baseline.csv`
- `benchmark_results/cross_stage_reuse_baseline.json`

Optional one-shot baseline + perf diff:

```bash
./scripts/run_cross_stage_baseline.sh \
  build/nebula tests/fixtures/smoke.nb benchmark_results \
  --perf-current /tmp/current_perf.json \
  --perf-baseline /tmp/baseline_perf.json \
  --perf-diff-json benchmark_results/perf_baseline_diff.json \
  --perf-diff-md benchmark_results/perf_baseline_diff.md
```

## Perf baseline diff

For current-vs-baseline performance judgement from `tests/run.py --perf-json-out`:

```bash
python3 scripts/perf_baseline_diff.py \
  --current /tmp/current_perf.json \
  --baseline /tmp/baseline_perf.json \
  --out-json /tmp/perf_diff.json \
  --out-md /tmp/perf_diff.md
```

Useful controls:
- `--fail-on-regression on|off` (default `off`)
- `--max-total-regression-pct` (default `15`)
- `--max-suite-regression-pct` (default `20`)
- `--max-case-regression-ms` (default `250`)
- `--min-case-duration-ms` (default `200`)
- `--max-cross-stage-reused-drop` (default `2`)
- `--max-cross-stage-saved-ms-drop` (default `200`)
- `--max-disk-hit-drop` (default `2`)
- `--max-disk-miss-increase` (default `10`)
- `--max-disk-eviction-increase` (default `5`)
- `--max-grouping-total-ms-increase` (default `400`)
- `--max-grouping-budget-fallback-increase` (default `0`)

## Artifact Policy

Build/test outputs are local artifacts and should not be committed:

- `build/`
- `generated_cpp/`
- `tests/artifacts/`
- `benchmark_results/`

See `ARTIFACT_POLICY.md` for cleanup and retention rules.
