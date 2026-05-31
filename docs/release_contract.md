# Release Contract

This document is the index for Nebula release obligations. It does not replace the detailed
release runbooks; it defines where each part of the contract lives and what must stay aligned.

## Canonical Files

- Release process: `RELEASE_PROCESS.md`
- Release verification: `docs/release_verification.md`
- Release sign-off runbook: `docs/release_signoff.md`
- Release-candidate checklist: `docs/rc_checklist_v1.0.md`
- Stability policy: `docs/stability_policy.md`
- Support matrix: `docs/support_matrix.md`
- Package tiering: `docs/official_package_tiering.md`
- Install lifecycle: `docs/install_lifecycle.md`
- Toolchain profile: `docs/toolchain_profile.md`
- Artifact policy: `ARTIFACT_POLICY.md`
- Contract-test harness: `tests/README.md`

If these files disagree, treat it as release-contract drift and fix the docs or implementation
before widening public claims.

## Contract Layers

### GA Compiler And Tooling

GA surface:

- compiler and CLI
- package workflow
- bundled `std`
- runtime headers
- documented release assets
- release documentation

Required evidence:

- strict build on supported matrix
- full contract suite on supported matrix
- installed/archive binary smoke
- at least one shipped-binary `git` dependency flow
- release notes for the target version

### Linux Backend SDK GA

GA backend SDK packages:

- `nebula-service`
- `nebula-observe`

Distribution contract:

- Linux x86_64 backend SDK asset
- explicit opt-in install only
- not installed by the default compiler/tooling archive

Required evidence:

- clean Linux source build
- backend SDK package asset produced
- install with `scripts/install.sh --with-backend-sdk`
- `nebula new --template backend-service`
- `nebula fetch`
- `nebula.lock` records installed backend SDK packages with `source_kind = "installed"`

### Installed Preview Packages

Installed-preview packages may ship inside the Linux backend SDK asset for convenience, but remain
preview:

- `nebula-auth`
- `nebula-config`
- `nebula-db-sqlite`
- `nebula-jobs`

Contract:

- docs must call them preview
- source-kind behavior must be tested
- no package is promoted to GA by being included in the backend SDK payload
- breaking/tightening behavior requires preview-boundary docs and tests

### Repo-Local Preview Packages

Repo-local preview packages stay outside the installed GA contract unless explicitly listed as
installed-preview:

- `nebula-db-postgres`
- `nebula-crypto`
- `nebula-tls`
- `nebula-tls-server`
- `nebula-pqc-protocols`
- `nebula-qkd`
- `nebula-qcomm-sim`
- `nebula-app-local`
- `nebula-thin-host-bridge`
- `nebula-ui`

Contract:

- consumed from repo checkout by `path` unless docs say otherwise
- smoke-tested where practical
- explicit guarantees and non-goals documented
- no broad production claim without release/signoff evidence

## Release Gates

Before tagging:

1. `VERSION` matches the intended tag.
2. Release branch scope is frozen.
3. Supported-matrix strict build is green.
4. Supported-matrix full contract suite is green.
5. Release workflow dry-run produces the full artifact set.
6. Attestation verification succeeds for archives, SBOM predicates, backend SDK artifact, and
   `SHA256SUMS.txt`.
7. `scripts/release_signoff.py --verify-attestations` succeeds against the downloaded release
   bundle.
8. Linux backend hardening passes on a clean Ubuntu x86_64 runner.
9. GA/preview wording is aligned across README, support matrix, stability policy, release notes,
   and package docs.
10. No unresolved release blocker remains in install, upgrade, rollback, artifact, or support
    matrix docs.

## Artifact Contract

Release assets must match `RELEASE_PROCESS.md`:

- platform archive for each supported compiler/tooling target
- Linux backend SDK archive
- SPDX SBOM sidecars
- provenance attestation bundles
- SBOM attestation bundles
- `SHA256SUMS.txt`
- `SHA256SUMS.txt.intoto.jsonl`
- `release-manifest.json`
- `nebula.rb`

Hosted registry services, MSI installers, winget, apt packages, external tap automation, App Store
distribution, notarization, and auto-updaters are not part of the current release contract unless a
future release document explicitly adds them.

## Drift Rules

Treat any of the following as a release-contract problem:

- README claims a package is GA while support/stability docs say preview
- backend SDK archives include or omit packages contrary to package-tiering docs
- installer behavior diverges from release verification docs
- release manifest contents diverge from release process docs
- contract tests encode behavior not described in docs
- docs describe support on a platform that CI/signoff does not exercise

Drift fixes should be explicit documentation or implementation changes, not silent test relaxation.

## Minimum Local Preflight

For a release-adjacent change, run at least:

```bash
cmake --build build -j2
python3 tests/run.py --suite all --timeout 300 --report text \
  --text-out artifacts/release-contract-full.txt \
  --json-out artifacts/release-contract-full.json \
  --perf-json-out artifacts/release-contract-full.perf.json
python3 scripts/app_platform_bench.py verify
git diff --check
```

If a full suite is intentionally deferred because a related batch will run it once, document the
deferral and keep focused gates green.
