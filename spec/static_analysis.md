# Static Analysis

This layer defines assistive analysis on top of language core semantics.

Scope:
- escape analysis
- exclusivity/borrow assist diagnostics
- epistemic lint minimal loop

## 1. Escape analysis

Primary contract: `spec/escape_analysis.md`.

- CFG-based intraprocedural provenance + conservative interprocedural summaries
- tri-state precision model:
  - `KnownNoEscape`
  - `KnownMayEscape`
  - `Unknown`
- invariant: `unknown=true => may=true`
- profile split:
  - `fast`: conservative on non-trivial SCCs
  - `deep`: bounded SCC fixpoint

Escape summaries feed region promotion and related diagnostics.

## 2. Region escape diagnostics

Primary diagnostics contract: `spec/diagnostics.md`.

- `NBL-R001` signals region escape with auto-promotion metadata
- machine-readable fields include:
  - `machine_reason` (`return|call|field`)
  - `machine_subreason`
  - `machine_detail` (`<machine_reason>/<machine_subreason>`)
  - `machine_trigger_family` (`return|call|field`)
  - `machine_trigger_family_detail` (`|`-joined trigger family set for mixed roots)
  - `machine_trigger_subreason` (stable trigger subtype slot)
  - `machine_owner`
  - `machine_owner_reason`
  - `machine_owner_reason_detail` (`|`-joined unknown-source split, when present)
    (including `cross-function-return-path-alias-fanout` vs
    `cross-function-return-path-alias-fanout-mixed` vs unknown-source splits)

This keeps promotion behavior machine-inspectable for tooling and CI.

## 3. Exclusivity and borrow checks (assist layer)

Borrow/exclusivity remains a conservative safety assist subsystem.

Representative diagnostics:
- `NBL-T090`, `NBL-T091`, `NBL-T092` for call/same-statement overlap
- `NBL-T093`, `NBL-T094`, `NBL-T095` for cross-statement windows

Safety details: `spec/safety_contract.md` and `spec/diagnostics.md`.

## 4. Epistemic lint minimal loop

Goal: connect ownership inference to a small actionable repair loop.

Minimal loop contract:
- root ownership/promotion issue (for example `NBL-R001`)
- inference pressure signal (`NBL-P010`)
- predictive follow-up (`NBL-X003`)
- `NBL-P010`/`NBL-X003` inherit `machine_trigger_family` +
  `machine_trigger_family_detail` + `machine_trigger_subreason`
  from source `NBL-R001` so trigger taxonomy survives into follow-up diagnostics
- `NBL-P010`/`NBL-X003` carry `machine_owner_reason_detail` from `NBL-R001` so mixed
  return-path unknown source causes remain machine-readable through the full loop

Grouped diagnostics must keep that chain machine-readable and ordered for repair priority.
The grouped `priority_fix_plan[]` queue (owner + machine + epistemic) is the canonical
fanout-first remediation ordering surface for this loop.
Exact grouped fields and ranking rules are specified in `spec/diagnostics.md`.
