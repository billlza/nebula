# Nebula 1.0 RC Checklist

This checklist is the release-candidate closeout sheet for the `1.0.x` line.

Use it together with:

- `RELEASE_PROCESS.md`
- `docs/support_matrix.md`
- `docs/stability_policy.md`
- `docs/install_lifecycle.md`
- `docs/release_verification.md`
- `docs/release_signoff.md`
- `RELEASE_NOTES_v1.0.0.md`

## Current local baseline

Validated on `2026-04-19` on the current development host:

- `[x]` strict configure/build passes
  - `cmake -S . -B build-audit -DNEBULA_STRICT=ON -DNEBULA_WERROR=ON`
  - `cmake --build build-audit -j4`
- `[x]` full suite passes locally
  - `python3 tests/run.py --suite all --binary build-audit/nebula --report text`
  - result: `487 passed / 0 failed / 0 budget_warnings`
- `[x]` targeted regressions added for the latest RC-tightening fixes
  - HTTP close-delimited incremental body-limit coverage
  - locked git dependency rehydrate coverage
  - external escape/ownership contract coverage
  - official service handler/write timeout coverage
  - file-oriented CLI closeout coverage (`std::fs`, `run --`, template/example smoke)
- `[x]` local host release dry-run passes for the current host target (`darwin-arm64`)
  - packaged archive: `work/rc-local-dist/nebula-v1.0.0-darwin-arm64.tar.gz`
  - generated local-host SPDX SBOM: `work/rc-local-dist/nebula-v1.0.0-darwin-arm64.spdx.json`
  - generated checksums + manifest:
    - `work/rc-local-dist/SHA256SUMS.txt`
    - `work/rc-local-dist/release-manifest.json`
  - archive-binary smoke passed
  - installer smoke passed
  - shipped-binary `git` dependency smoke passed
- `[ ]` local host formula render is intentionally still blocked on full multi-platform artifact presence
  - `scripts/render_homebrew_formula.py` expects the complete cross-platform release set, not a single-host subset

This local baseline is necessary, but it is not the final tag gate by itself.

## Current External Blockers

These items still require a committed RC candidate and GitHub/CI execution, so they are not
completed by local dry-run work alone:

- `[ ]` cut `release/1.0` from the intended RC commit
- `[ ]` supported-matrix CI on the actual RC commit
- `[ ]` full release workflow dry-run with the complete multi-platform asset set
- `[ ]` GitHub attestation production and `gh attestation verify` against emitted workflow bundles
- `[ ]` final sign-off on the exact tag candidate commit and published release notes

## Tag-Blocking Must-Haves

- `[ ]` `release/1.0` branch is cut from the intended RC state and scope-frozen.
- `[ ]` `VERSION` matches the intended GA tag version.
- `[ ]` `RELEASE_NOTES_v1.0.0.md` is final-reviewed against the actual release payload and validation baseline.
- `[ ]` supported-matrix CI passes for the compiler/tooling GA contract:
  - macOS x86_64
  - macOS arm64
  - Linux x86_64
  - Windows x86_64
- `[ ]` strict build passes in the supported matrix with warnings-as-errors.
- `[ ]` full contract suite passes in the supported matrix; do not substitute local sampling for CI matrix completion.
- `[ ]` release workflow dry-run from the RC commit produces the full asset set:
  - platform archives
  - Linux backend SDK archive
  - SPDX SBOM sidecars
  - provenance attestation bundles
  - SBOM attestation bundles
  - backend SDK SPDX SBOM sidecar
  - backend SDK provenance attestation bundle
  - backend SDK SBOM attestation bundle
  - `SHA256SUMS.txt`
  - `SHA256SUMS.txt.intoto.jsonl`
  - `release-manifest.json`
  - `nebula.rb`
- `[ ]` attestation verification succeeds for every platform archive, every platform SBOM predicate,
  the backend SDK archive and SBOM predicate, and `SHA256SUMS.txt`.
- `[ ]` fresh-install smoke passes from produced release assets:
  - install into a clean prefix
  - `nebula --version`
  - `nebula new <tmp> --template cli`
  - `nebula run <tmp> --run-gate none -- <input> <output>`
  - copied output matches input bytes
- `[ ]` Mainline A Linux backend hardening passes on a clean Ubuntu x86_64 runner:
  - source build
  - `python3 scripts/package_release.py --include-backend-sdk`
  - `bash scripts/install.sh --with-backend-sdk`
  - `nebula new <tmp> --template backend-service`
  - `nebula fetch`
  - `nebula.lock` records `nebula-service` with `source_kind = "installed"`
- `[ ]` installed/archive binary passes at least one `git` dependency flow end-to-end.
- `[ ]` `scripts/release_signoff.py --verify-attestations` passes against the downloaded release
  bundle, and its Markdown/JSON outputs are archived with the release evidence.
- `[ ]` GA surface and preview-package language are still aligned everywhere:
  - compiler/CLI/package workflow/bundled `std`/runtime headers are GA
  - CLI/tooling docs still present 1.0 as a small-to-medium CLI / system tool release
  - installed-preview wording stays explicit where `nebula-db-sqlite` ships inside the opt-in
    backend SDK asset
  - remaining `official/*` packages still remain preview/pilot, not installed GA surface
- `[ ]` no unresolved release blocker remains in:
  - install/upgrade/rollback/uninstall docs
  - support matrix docs
  - release verification docs
  - release artifact manifest / checksums / attestations

## Recommended Before Tag

- `[ ]` run one manual archive verification walk-through using the published commands in `docs/release_verification.md`.
- `[ ]` run one installer path with strong verification enabled:
  - `install.sh --verify-attestations`
  - `install.ps1 -VerifyAttestations`
- `[ ]` manually inspect `release-manifest.json`, SBOM sidecars, and attestation bundle names for shape drift.
- `[ ]` verify Homebrew formula output against the final archive names and checksum file.
- `[ ]` re-read `README.md` quick-start and release metadata sections after the final version bump.
- `[ ]` confirm the support matrix and host-compiler language in docs matches what CI actually exercised.
- `[ ]` spot-check one workspace project, one path dependency project, one local-registry project,
  one hosted-registry project, and one `git` dependency project with the candidate binary.
- `[ ]` spot-check the file-oriented CLI path with the candidate binary:
  - `nebula new --template cli`
  - `nebula run ... -- <input> <output>`
  - `main() -> Int` exit code behavior
- `[ ]` capture and archive the final CI links / logs / artifact URLs used for release sign-off.

## Defer Past 1.0

These are real follow-ups, but they should not block the `1.0.0` tag unless a new concrete bug appears.

- `[ ]` hard cancellation for handler timeout instead of the current soft timeout behavior.
- `[ ]` deeper git-lock stale detection for cache-hit checkout drift and `.git` metadata integrity.
- `[ ]` richer external contract taxonomy that distinguishes declared-unknown contract edges from missing-summary edges.
- `[ ]` service-platform scope beyond the current pilot profile:
  - middleware chaining
  - keep-alive / pooling policy
  - panic-to-`500`
  - inbound TLS termination
  - TLS 1.3 / mTLS surface claims
  - Prometheus / OpenTelemetry integration
- `[ ]` hosted registry expansion beyond the current MVP slice.
- `[ ]` broader starter kits / editor integration work planned for `1.1+`.
- `[ ]` fuzzing as a standing release lane rather than post-cut follow-up.

## Release Sign-Off Prompt

Before tagging, the release owner should be able to say:

- the `1.0` GA surface is narrower than repo-local preview packages, and the docs say so consistently
- supported-matrix CI is green on strict build and full suite
- release assets, checksums, SBOMs, and attestations were produced and verified
- fresh install and shipped-binary smoke succeeded, including a `git` dependency path
- there are no unresolved blockers left in release engineering, documentation, or artifact verification
