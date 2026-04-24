# Experimental / Infra layer

This document holds non-core language mechanics so language semantics stay foregrounded.

## Scope

- cross-stage analysis reuse (`--cross-stage-reuse`)
- run-stage disk cache (`--disk-cache*`)
- grouped/root-cause diagnostic shaping (`--diag-view grouped`, root-cause v2 controls)
- cache telemetry output (`--cache-report*`)

## Positioning

These features are engineering accelerators around the compiler pipeline.
They are not part of Nebula language identity and should not redefine the core narrative.

## Stable user surface

User-facing flag/exit behavior remains specified in `spec/tooling_cli.md`
(compat link: `spec/cli_contract.md`).
This file tracks infra intent and implementation boundaries:

- cache and grouping paths must preserve deterministic diagnostics for the same inputs/options
- preflight-only advisories must not leak into build-stage diagnostics during reuse
- grouped budget fallback keeps machine-readable compatibility shape

## Current priorities (non-goal reminder)

Until Rep x Owner semantics, region escape/promotion rules, and epistemic lint loop are hardened:
- avoid expanding infra flag surface unless it unlocks correctness/clarity work
- prefer reducing accidental complexity over adding new toggles
