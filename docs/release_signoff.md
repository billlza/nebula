# Release Sign-Off Runbook

This page turns Nebula `1.0.x` release operations into a concrete sequence for the release owner.

It complements:

- `RELEASE_PROCESS.md`
- `docs/release_verification.md`
- `docs/rc_checklist_v1.0.md`

## 1. Cut The RC Branch

Do this from a clean checkout of the exact RC candidate commit:

```bash
git switch -c release/1.0 <rc-commit>
git push -u origin release/1.0
```

Only blocker fixes, documentation corrections, and release engineering hardening belong on that
branch.

## 2. Run Supported-Matrix CI And Linux Backend Hardening

Use the committed `release/1.0` branch head, not an unpushed local tree:

```bash
gh workflow run contract-tests.yml --ref release/1.0
```

Wait for green macOS x86_64, macOS arm64, Linux x86_64, and Windows x86_64 matrix jobs, plus
the dedicated `linux-backend-ga-hardening` job.

For Mainline A, the `linux-backend-ga-hardening` job is a required sign-off gate, not an optional
smoke. It must prove from a clean Ubuntu x86_64 runner that Nebula can:

- build from source
- package both `nebula-v<VERSION>-linux-x86_64.tar.gz` and `nebula-backend-sdk-v<VERSION>-linux-x86_64.tar.gz`
- install with `scripts/install.sh --with-backend-sdk`
- scaffold `nebula new --template backend-service`
- run `nebula fetch`
- record `source_kind = "installed"` in `nebula.lock`

Treat any of the following as release blockers for Mainline A sign-off:

- Linux-only compile failures
- backend SDK opt-in install failures
- installed dependency resolution failures
- LSP daily-use regressions
- async explain regressions

## 3. Run The Full Release Dry-Run

The `release` workflow already behaves as a dry-run when invoked with `workflow_dispatch` on
`release/1.0`: it assembles the full release bundle, runs attestation verification, performs the
installer strong-verification smoke, and does not publish a GitHub Release because `publish-release`
is tag-only.

Start it with:

```bash
gh workflow run release.yml --ref release/1.0
```

Then locate the latest dry-run:

```bash
gh run list --workflow release.yml --branch release/1.0 --event workflow_dispatch --limit 1
```

Download the produced bundle:

```bash
gh run download <run-id> --name release-bundle --dir work/release-bundle
```

## 4. Verify The Bundle Locally

Run the local sign-off helper against the downloaded bundle:

```bash
python3 scripts/release_signoff.py \
  --artifact-dir work/release-bundle \
  --verify-attestations \
  --json-out work/release-signoff.json \
  --markdown-out work/release-signoff.md
```

This verifies:

- all required release files are present
- archives do not contain `official/*`
- archives contain the expected runtime headers, bundled `std`, docs, and hosted-registry helper
  payload under `share/nebula/registry`
- the Linux backend SDK archive contains the expected backend GA packages plus installed-preview
  payloads, docs, and examples
- `release-manifest.json` digests and sizes are self-consistent
- `nebula.rb` references the expected assets and SHA256 values
- bundled attestations verify with `gh attestation verify`

## 5. Required Mainline A Sign-Off Gate

Mainline A sign-off is tag-blocking. Do not create or push `v<VERSION>` until every item below is
satisfied for the exact RC candidate commit and archived as release evidence:

- supported-matrix `contract-tests.yml` is green, including `linux-backend-ga-hardening`
- `release.yml` dry-run is green for the same RC commit
- the Linux x86_64 clean-room path proves:
  - source build
  - `python3 scripts/package_release.py --include-backend-sdk`
  - `bash scripts/install.sh --with-backend-sdk`
  - `nebula new --template backend-service`
  - `nebula fetch`
  - `nebula.lock` records `nebula-service` with `source_kind = "installed"`
- local `scripts/release_signoff.py --verify-attestations` succeeds against the downloaded
  `release-bundle`
- the candidate installed binary can `publish/fetch/run` a hosted-registry project without a repo
  checkout for the registry helper path

Treat any Linux-only compile failure, opt-in backend SDK install failure, installed dependency
resolution failure, LSP daily-use regression, or async explain regression as a Mainline A release
blocker.

## 6. Final Tag Sign-Off

Before tagging, the release owner should have:

- the exact RC commit SHA
- the green contract-tests run URL
- the green `linux-backend-ga-hardening` job URL
- the green release dry-run URL
- the downloaded `release-bundle`
- the local `work/release-signoff.md` and `work/release-signoff.json`
- final-reviewed `RELEASE_NOTES_v<VERSION>.md`

Then tag the exact RC commit:

```bash
git tag v<VERSION> <rc-commit>
git push origin v<VERSION>
```

The tag push runs the same `release` workflow again, this time allowing the `publish-release` job
to publish the already-validated release bundle as the GitHub Release.
