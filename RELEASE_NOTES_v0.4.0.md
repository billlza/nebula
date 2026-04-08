# Nebula v0.4.0 Release Notes

## Overview

v0.4.0 is a CLI contract release that hardens command boundaries:

- `build` is build-only.
- `run` is execution-oriented (preflight/build/reuse/no-build policies).

This is a breaking change from the v0.3 line.

## Breaking changes

### Removed compatibility behavior

- `nebula build --run` is no longer supported.
- `--run` under `build` is now a parse error:
  - exit code: `2`
  - message includes: `unknown option: --run`

### Command-specific option enforcement

The following options are now `run`-only:

- `--preflight`
- `--run-gate`
- `--reuse`
- `--no-build`

Passing any of these options to `build` is now a parse error (`rc=2`, `unknown option`).

## Validation

Contract coverage for this release includes:

- `BLD-003`: `build --run` rejected
- `BLD-005`: `build --run --no-build` rejected
- `BLD-007`: `build --run` rejected on exit-code-forwarding path
- `BLD-009`: build rejects all run-only options

Current suite baseline:

- `cases: 122`
- `passed: 122`
- `failed: 0`
