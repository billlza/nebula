# Nebula v0.3.0 Release Notes

## Overview

v0.3.0 is a stability/contract release focused on predictable CLI behavior, executable exit-code
propagation, and reproducible release gates.

## Highlights

- CLI parse errors are now strict and uniform across all subcommands.
- `run` now returns the executed artifact's real exit code.
- Shell-string command execution paths were hardened to argv-based execution.
- Strict build gates are standardized in CI with `NEBULA_STRICT=ON` + `NEBULA_WERROR=ON`.

## CLI Contract Changes

### 1) Unknown option handling

- Affects: `check`, `build`, `run`, `test`, `bench`.
- Behavior: unknown options are parse-stage CLI errors, always `rc=2`.
- Output format includes: `unknown option: <flag>`.
- Not emitted as structured diagnostics JSON.

### 2) `--san` removal

- `--san` is removed in v0.3.0.
- There is no replacement flag in this release.
- Passing `--san` now behaves as unknown option (`rc=2`).

Migration example:

```diff
- ./build/nebula check examples/smoke.nb --san
+ ./build/nebula check examples/smoke.nb
```

### 3) Execution exit-code propagation

`run` now preserves the executed artifact's exit code.

| Scenario | Old behavior | New behavior |
| --- | --- | --- |
| Artifact exits `0` | `0` | `0` |
| Artifact exits `1` | wrapped / unstable | `1` |
| Artifact exits `137` | wrapped / unstable | `137` |

## Compatibility Notes

- Scripts that previously ignored wrapped non-zero runtime exits may now fail correctly.
- Update CI wrappers to treat non-zero `run` as real runtime failures.

## Risk / Rollback

- Primary risk: legacy scripts relying on previous wrapped exit behavior.
- Rollback strategy:
  1. Pin to previous stable binary.
  2. Remove strict runtime gating from automation temporarily.
  3. Re-apply with updated script expectations.

## Validation Coverage Added

- Unknown option coverage across all subcommands:
  - `CHK-070`, `BLD-006`, `RUN-016`, `TST-003`, `BEN-003`
- Removed `--san` path:
  - `CHK-071`
- Exit-code propagation:
  - `RUN-014`, `RUN-017`
- Special `--out` path handling:
  - `RUN-015`
