# Artifact Policy

Nebula keeps build and test artifacts local to the workspace. These outputs are transient and
should not be committed.

## Managed artifact paths

- `build/`
- `generated_cpp/`
- `tests/artifacts/`
- `benchmark_results/`

## Rules

1. Treat artifact directories as disposable cache/output.
2. Do not edit generated files directly; regenerate from source.
3. Keep contract fixtures and specs under `tests/fixtures/` and `spec/` only.

## Cleanup

From repo root:

```bash
rm -rf build generated_cpp tests/artifacts benchmark_results
```
