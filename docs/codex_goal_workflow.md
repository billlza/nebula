# Codex Goal Workflow

Nebula Goal-mode work should produce small, reviewable, evidence-backed changes. A goal is not done
because the diff looks plausible; it is done when the requested repository state is present and the
required commands prove it.

## One Goal, One Branch, One Worktree

Use one branch and one worktree per active goal. Keep goal work isolated from unrelated local drift.

Recommended shape:

```bash
git status --short
git worktree add ../nebula-goals/<goal-name> -b goal/<goal-name>
cd ../nebula-goals/<goal-name>
```

If an existing goal branch must be resumed, inspect it first:

```bash
git status --short
git branch --show-current
git log --oneline -5
```

Do not use a shared dirty worktree for multiple goals unless the task is read-only.

## Goal Naming

Goal names should be stable enough to survive handoff:

```text
goal/<area>-<ticket-or-gate>-<short-slug>
```

Examples:

```text
goal/docs-uos-gates
goal/cli-uos-std-rejection
goal/abi-layout-goldens
goal/core-no-std-smoke
```

Use the gate id when the work is tied to a staged UniverseOS milestone, for example
`goal/uos-cli-001-std-import-rejection`.

## Required Build And Test Commands

Every Goal-mode delivery must state which commands were run and whether they passed. The default
strict gate from the repository root is:

```bash
cmake -S . -B build -DNEBULA_STRICT=ON -DNEBULA_WERROR=ON
cmake --build build -j
python3 tests/run.py --suite all --report text --binary build/nebula
```

Focused tests are useful while developing, but they do not replace the required strict gate unless
the goal explicitly narrows acceptance. Documentation-only goals should still run the required gate
when the acceptance criteria name it.

Also run:

```bash
git diff --check
```

when the goal changes text, fixtures, generated snippets, or whitespace-sensitive artifacts.

## Review Expectations

Before requesting review or marking a goal complete:

- compare the final diff against the original objective
- explain any pre-existing dirty files separately from files changed for the goal
- confirm that no broad rewrite, unrelated refactor, lockfile churn, or generated artifact churn was
  included without approval
- cite tests or artifacts that prove each changed contract
- verify that warnings are fixed at the source, not hidden by suppression flags
- check that UniverseOS text remains staged, evidence-gated, and free of unsupported kernel,
  driver, interrupt, MMU, scheduler, or freestanding runtime claims

## Rollback Expectations

A goal branch must be easy to roll back:

- keep commits scoped to one goal
- avoid sharing generated state between goals
- document any irreversible migration before applying it
- prefer deleting the isolated worktree/branch over cleaning a mixed dirty tree
- never revert user changes from another goal unless explicitly instructed

When a goal fails validation, report the failing command and the smallest known affected surface.
Do not rewrite the goal into an easier success condition.

## Broad Rewrite Policy

No broad rewrites without explicit approval. This includes parser rewrites, codegen restructures,
runtime replacement, test harness redesign, package layout changes, build-system migration, and
large documentation taxonomy changes.

If a broad rewrite appears necessary, first write down:

- the root cause the rewrite addresses
- why smaller changes cannot fix it
- the expected compatibility impact
- the migration and rollback plan
- the new tests that will prove the replacement

Then wait for explicit approval before implementation.
