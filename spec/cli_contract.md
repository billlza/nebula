# CLI contract (compat entry, v1.0.0)

This file is intentionally minimal.

## Canonical docs

- Normative Tooling/CLI contract: `spec/tooling_cli.md`
- Diagnostic schema and machine fields: `spec/diagnostics.md`
- cache/reuse/grouping/baseline internals: `spec/experimental_infra.md`

## Stable CLI shape (quick reminder)

- Commands: `check | build | run | test | bench | new | add | publish | fetch | update | fmt | explain | lsp`
- Parse/option errors: exit code `2`
- `--help|-h` and `--version`: exit code `0`
- `run`: returns child exit code once execution starts

For all details, use `spec/tooling_cli.md`.
