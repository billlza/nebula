# Engineering Playbook

This playbook is the default engineering rulebook for Nebula work. It complements
`CONTRIBUTING.md`, `ARTIFACT_POLICY.md`, `tests/README.md`, `docs/stability_policy.md`, and
`docs/support_matrix.md`.

## Default Development Bias

Nebula's default product and engineering bias is:

1. Prove real platform capability before expanding surface area.
2. Prefer backend-first internal systems, app-core contracts, and thin-host applications before
   broad GUI, kernel, or ecosystem claims.
3. Keep Nebula-owned logic in Nebula whenever practical:
   - application state
   - command validation
   - domain transitions
   - event generation
   - snapshots
   - recovery policy
   - receipts
   - jobs/outbox semantics
   - observability markers
4. Use C/C++ or host-native code only at explicit boundaries:
   - host shell and window/platform I/O
   - codec/player engines
   - torrent or other mature external libraries
   - OS APIs and native dependency probes
   - C++ reference benchmarks
5. Keep GA, installed-preview, repo-preview, and experimental surfaces separate. Shipping a preview
   package in an installed artifact does not promote it to GA.
6. Do not position Nebula as a full native GUI platform, kernel language, or universeOS substrate
   until the documented gates in `docs/app_platform_convergence.md`,
   `docs/universeos_convergence.md`, and `docs/system_profile.md` are satisfied.

If a proposed change conflicts with this bias, the change needs an explicit doc update and a clear
reason. It should not drift in quietly through tests or examples.

## Code Change Rules

- Fix root causes. Do not hide failures behind broad fallbacks, relaxed thresholds, larger timeouts,
  or catch-all recovery paths.
- Keep patches focused. Avoid bundling unrelated refactors, artifact churn, or lockfile drift with
  product changes.
- Preserve existing user or generated work unless the task explicitly asks to remove it.
- Prefer established repo patterns over new abstractions. Add abstractions only when they reduce
  real complexity or define a contract that will be tested.
- For official packages, update package docs whenever guarantees, non-goals, or source-kind
  boundaries change.
- For preview package tightening, prefer explicit rejection semantics over silent compatibility
  behavior.
- For runtime and compiler changes, preserve diagnostics and error semantics unless the contract is
  deliberately updated.

## Safety And Native Boundaries

- Unsafe or native boundaries must stay narrow, named, and testable.
- Host-native code must not own Nebula application business state.
- Native probes must fail with clear diagnostics when required dependencies are missing. They must
  not silently degrade a real native gate into a fake pass.
- C/C++ probes and adapters should build with warning-as-error policy where practical, using
  `-Wall -Wextra -Werror` for focused probes.
- Security or crypto claims require matching docs and negative tests. Do not treat sample code as a
  security certification.

## Worktree And Commit Boundaries

- Check `git status --short` before staging.
- Stage only files that belong to the current module.
- Do not include unrelated `nebula.lock` drift, generated C++ output, benchmark artifacts, or local
  run artifacts unless they are the explicit contract input being changed.
- If generated artifact noise is recurring, update ignore rules or artifact docs instead of hiding
  unrelated work.
- Commit messages should describe the contract or behavior changed, not only the implementation
  mechanism.

## Testing Rules

Use the smallest test set that proves the change first, then widen only as needed.

Baseline command shape:

```bash
cmake --build build -j2
python3 tests/run.py --suite <suite> --filter '<case-id-or-glob>' --timeout 300 --report text
python3 scripts/app_platform_bench.py verify
git diff --check
```

Run the full contract suite when:

- a module changes shared runtime/compiler behavior
- release, installer, package resolution, or source-kind behavior changes
- a feature is being prepared for a public claim
- focused failures cannot prove the blast radius
- the user explicitly asks for a long validation run

Full-suite command shape:

```bash
python3 tests/run.py --suite all --timeout 300 --report text \
  --text-out artifacts/<topic>-full.txt \
  --json-out artifacts/<topic>-full.json \
  --perf-json-out artifacts/<topic>-full.perf.json
```

If full suite fails, diagnose from the JSON/text report. Do not loosen timeouts, thresholds, or
contracts unless the failure is proven to be an obsolete contract.

## Performance Rules

- Performance claims must come from repeatable benchmark JSON on the same machine.
- C++ parity or win/loss claims require a matching C++ reference workload where applicable.
- Report real ratios. Do not hide slowdowns by changing thresholds or excluding unfavorable
  workloads.
- Optimize Nebula-owned hot paths first: JSON wire traversal, bridge/action dispatch, snapshot/state
  sync, Result access, struct-copy costs, HTTP routing, bytes framing, and SQLite CRUD.
- Do not trade away safety, deterministic rejection semantics, or observability for benchmark wins.

## Documentation Rules

Update docs when a change affects:

- user-visible CLI behavior
- package source kind or installed-preview status
- public/non-public API shape
- preview boundary or explicit non-goal
- release artifact contents
- support matrix
- operator runbook
- performance claim surface

Docs should say what is guaranteed, what is preview, what is explicitly not promised, and what test
or smoke proves the claim.

## Failure Handling Rules

- Build warnings in strict or native probe paths are failures.
- Missing dependencies in a real native gate are explicit failures with installation diagnostics.
- Invalid commands, schema mismatches, stale revisions, and illegal transitions should produce
  explicit rejected events without changing state.
- Unwritable required receipt/state stores should fail fast or enter a documented degraded mode.
- Recovery diagnostics should explain facts; application-specific recovery actions belong to the
  app.
- Quarantine is only acceptable for an explicitly documented external/environment condition. Unknown
  red tests are not a healthy signal.

## Review Checklist

Before landing a change, verify:

- the change follows the default development bias
- GA/preview wording is still correct
- no unrelated worktree drift is staged
- focused tests prove the changed behavior
- performance claims, if any, are backed by benchmark artifacts
- docs and examples match the implementation
- release impact is either none or explicitly documented
