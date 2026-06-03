# Codex Agent Guide

This file applies to the entire Nebula repository.

## Repository Baseline

Nebula is a C++23 compiler and language/tooling repository. The current supported path is:

```text
Nebula source -> AST -> typed AST -> NIR/CFG -> analysis -> C++23 -> clang++
```

Do not describe the project as having kernel, driver, interrupt, MMU, scheduler, freestanding
runtime, or backend-independent object-code support unless the relevant repository gates prove that
claim. UniverseOS is a staged future direction, not a current support promise.

## Work Discipline

- Check the current worktree before editing: `git status --short`.
- Keep changes scoped to the requested goal. Do not mix unrelated lockfile, benchmark, generated, or
  formatting churn into a goal branch.
- Prefer root-cause fixes and explicit contract failures. Do not add fallback paths, broad retries,
  catch-all recovery, warning suppression, or compatibility shims that hide real errors.
- Treat warnings as failures in strict builds. Fix the warning source instead of suppressing it.
- Preserve user or generated changes you did not make unless the task explicitly asks to remove
  them.
- Use a GPT-5.5 xhigh sub-agent for non-trivial Codex work when sub-agents are available, with a
  bounded task and a disjoint write scope. If that exact sub-agent is unavailable, state that
  explicitly before continuing.

## Build And Test

The strict baseline from the repository root is:

```bash
cmake -S . -B build -DNEBULA_STRICT=ON -DNEBULA_WERROR=ON
cmake --build build -j
python3 tests/run.py --suite all --report text --binary build/nebula
```

Use focused test filters first only when they prove a narrow local change. For Goal-mode delivery,
finish with the strict baseline above unless the goal explicitly names a stronger gate.

Useful focused forms:

```bash
python3 tests/run.py --suite check --filter '<case-id-or-glob>' --report text --binary build/nebula
python3 tests/run.py --suite build --filter '<case-id-or-glob>' --report text --binary build/nebula
python3 tests/run.py --suite run --filter '<case-id-or-glob>' --report text --binary build/nebula
git diff --check
```

Do not claim a command passed unless it was run in the current worktree and completed successfully.

## Documentation Rules

Documentation must separate:

- current GA surfaces
- installed-preview surfaces
- repo-local preview packages
- experimental gates
- future plans
- explicit non-goals

For UniverseOS-related text:

- State that UniverseOS is staged and evidence-gated.
- Link claims to concrete gates, tests, specifications, or artifacts.
- Use `docs/system_profile.md` and `docs/universeos_convergence.md` as the current boundary.
- Do not imply that the system-profile CLI gate is a freestanding runtime, kernel target, driver
  framework, interrupt model, MMU model, scheduler, syscall ABI, or independent native backend.

If a document changes support posture, update the related support matrix, execution plan, and tests
in the same goal, or state that the document is only a staged plan.

## Review And Delivery

Before reporting a goal complete:

- Re-read the objective and map each acceptance item to current evidence.
- Inspect `git diff --stat` and `git diff --check`.
- Run the required build/test commands and report exact results.
- List changed files and any unrelated pre-existing worktree changes.
- Call out follow-up risks without upgrading future plans into current product claims.
