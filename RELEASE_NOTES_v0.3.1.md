# Nebula v0.3.1 Release Notes

## Overview

v0.3.1 is a CLI usability patch release. It does not change language semantics or safety-analysis
contracts; it focuses on improving command discoverability and tightening parser-contract coverage.

## Highlights

- Added global `--help` / `-h` (success path, `rc=0`).
- Added global `--version` (success path, `rc=0`).
- Expanded parse-contract tests for missing/invalid option values.
- Kept strict gate policy in CI (`NEBULA_STRICT=ON` + `NEBULA_WERROR=ON`).

## CLI Contract Notes

### Global flags (new in v0.3.1)

- `nebula --help` / `nebula -h` prints usage and exits `0`.
- `nebula --version` prints `nebula v0.3.1` and exits `0`.

### Parse-stage errors (unchanged from v0.3.0, now better covered)

- Unknown option for any subcommand: `rc=2`.
- Missing option value: `rc=2`.
- Invalid option value: `rc=2`.
- Parse errors remain plain CLI errors (non-structured diagnostics).

### `--san` migration status (unchanged)

- `--san` remains removed and has no replacement in v0.3.1.
- Passing `--san` is treated as unknown option (`rc=2`).

Migration snippet:

```diff
- ./build/nebula check examples/smoke.nb --san
+ ./build/nebula check examples/smoke.nb
```

### Exit-code propagation (unchanged from v0.3.0)

`run` continues returning the executed artifact exit code (e.g. `1`, `137`).

## Added/Updated Contract Cases

- Global flags: `CHK-072`, `CHK-073`
- Missing/invalid parse values: `CHK-074`, `CHK-075`, `BLD-008`, `RUN-018`, `TST-004`, `BEN-004`
- Existing parse/removed-flag coverage retained: `CHK-070`, `CHK-071`, `BLD-006`, `RUN-016`, `TST-003`, `BEN-003`
