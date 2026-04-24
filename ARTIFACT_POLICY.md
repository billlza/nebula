# Artifact Policy

Nebula keeps build and test artifacts local to the workspace. These outputs are transient and
should not be committed.

## Managed artifact paths

- `build/`
- `build-*/`
- `generated_cpp/`
- nested `generated_cpp/`
- `tests/artifacts/`
- `benchmark_results/`
- nested `benchmark_results/`
- `dist/`
- `dist-*/`
- `artifacts/`
- `tmp-debug-*/`
- `--help/`
- `core` / `core.*`
- `node_modules/`
- `tooling/vscode/out/`

## Rules

1. Treat artifact directories as disposable cache/output.
2. Do not edit generated files directly; regenerate from source.
3. Keep contract fixtures and specs under `tests/fixtures/` and `spec/` only.
4. If `git status` is noisy because of new local artifact directories, prefer extending ignore rules
   for those generated paths rather than hiding unrelated user-owned work.

## Cleanup

From repo root:

```bash
rm -rf build build-* generated_cpp tests/artifacts benchmark_results artifacts dist dist-* tmp-debug-* core core.*
find benchmarks -type d \( -name generated_cpp -o -name benchmark_results \) -prune -exec rm -rf {} +
rm -rf tooling/vscode/node_modules tooling/vscode/out
rm -rf -- --help
```
