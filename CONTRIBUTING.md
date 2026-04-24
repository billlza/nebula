# Contributing

Nebula values small, explicit, high-signal changes.

Guidelines:

- Start with the documented scope and non-goals before proposing expansion.
- Prefer focused patches with tests over broad speculative refactors.
- Keep language/runtime changes aligned with the public narrative and compatibility docs.
- For official packages, document guarantees and non-goals alongside the code.
- If `git status` noise comes from generated local output, prefer ignoring those artifact paths over
  moving or hiding unrelated in-progress local work.

Suggested flow:

1. Run the build from repo root with CMake.
2. Run targeted contract tests for the area you touched.
3. Update docs and examples when the user-facing shape changed.
4. Keep release/readiness impact visible in the PR description.
