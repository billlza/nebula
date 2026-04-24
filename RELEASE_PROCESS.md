# Nebula Release Process

This document defines the release procedure for Nebula `1.0.x`.

Concrete `1.0.0` release-candidate closeout items live in `docs/rc_checklist_v1.0.md`.
Operational branch-cut / workflow / tag sign-off steps live in `docs/release_signoff.md`.

## 1. Release branches

- `release/1.0` is the stable patch line for `1.0.0`, `1.0.1`, and later `1.0.x` releases.
- `main` carries forward-looking work and is bumped to the next planned version after each GA cut.
  Prerelease values such as `1.1.0-dev` are allowed in `VERSION`.

## 2. Pre-release gates

Before tagging a release:

1. `VERSION` matches the intended release version.
2. Release candidate scope is frozen on `release/1.0`; only blocker fixes, documentation
   corrections, and release engineering hardening are in scope.
3. The `must-have` items in `docs/rc_checklist_v1.0.md` are complete.
4. Strict build passes on the supported matrix.
5. Contract suite passes on the supported matrix.
6. Release workflow dry-run produces:
   - platform archives
   - Linux backend SDK archive
   - per-platform SPDX SBOM sidecars
   - per-platform provenance attestation bundles
   - per-platform SBOM attestation bundles
   - backend SDK SPDX SBOM sidecar
   - backend SDK provenance attestation bundle
   - backend SDK SBOM attestation bundle
   - `SHA256SUMS.txt`
   - `SHA256SUMS.txt.intoto.jsonl`
   - `release-manifest.json`
   - `nebula.rb`
7. Release notes for the target version exist.
8. Release smoke and installer smoke run Nebula with the official default host compiler contract:
   `clang++` on Unix platforms and `clang++.exe` on Windows. No silent `g++` fallback is part of
   the GA release guarantee.
9. Installed or archive binaries must also pass at least one `git` dependency resolve flow before
   tagging a release.
10. Attestation verification succeeds for each platform archive, each platform SPDX SBOM predicate,
   the backend SDK archive and SPDX SBOM predicate, and `SHA256SUMS.txt`.
11. Mainline A Linux backend hardening passes on a clean Ubuntu x86_64 runner:
   - source build
   - `python3 scripts/package_release.py --include-backend-sdk`
   - `bash scripts/install.sh --with-backend-sdk`
   - `nebula new --template backend-service`
   - `nebula fetch`
   - `nebula.lock` records `nebula-service` with `source_kind = "installed"`

Use `docs/release_signoff.md` and `scripts/release_signoff.py` to turn the downloaded dry-run
bundle into a concrete local sign-off artifact before tagging.

## 3. GA cut

1. Branch from the release candidate state to the matching release branch, for example `release/1.0`.
2. Ensure `VERSION` matches the GA version being cut, for example `1.0.0`.
3. Tag the release commit as `v<VERSION>`.
4. Run the release workflow from the tag.
5. Publish GitHub Release assets and notes from `RELEASE_NOTES_v<VERSION>.md`.

## 4. Post-GA step

After `v<VERSION>` is published:

1. Bump `main` version markers to the next planned version, for example `1.1.0-dev`.
2. Keep the release branch at the released GA version until patch work is needed.

## 5. Patch releases (`1.0.1+`)

1. Cherry-pick or implement the patch on `release/1.0`.
2. Update `VERSION` to the patch version, for example `1.0.1`.
3. Add release notes for that patch version.
4. Re-run strict build, full contract suite, and release dry-run.
5. Tag `v1.0.1` from `release/1.0`.

## 6. Release asset policy

Official 1.0 artifacts are:

- `nebula-v<version>-darwin-x86_64.tar.gz`
- `nebula-v<version>-darwin-arm64.tar.gz`
- `nebula-v<version>-linux-x86_64.tar.gz`
- `nebula-v<version>-windows-x86_64.zip`
- `nebula-backend-sdk-v<version>-linux-x86_64.tar.gz`
- `nebula-v<version>-<target>.spdx.json`
- `nebula-v<version>-<target>.provenance.intoto.jsonl`
- `nebula-v<version>-<target>.sbom.intoto.jsonl`
- `nebula-backend-sdk-v<version>-linux-x86_64.spdx.json`
- `nebula-backend-sdk-v<version>-linux-x86_64.provenance.intoto.jsonl`
- `nebula-backend-sdk-v<version>-linux-x86_64.sbom.intoto.jsonl`
- `SHA256SUMS.txt`
- `SHA256SUMS.txt.intoto.jsonl`
- `release-manifest.json`
- `nebula.rb`

Hosted registry services, MSI installers, winget, apt packages, and external tap automation are
not part of the 1.0 release contract.

Installed artifact scope is intentionally narrower than the repo-local preview packages under
`official/`: release assets install compiler/tooling, bundled `std`, runtime headers, and release
documentation only.

Official host compiler policy for release validation is:

- Nebula release assets are validated against `clang++` as the default host C++23 compiler.
- Users may still override `CXX` explicitly, but alternate host compilers are outside the default
  GA smoke matrix.

Official git dependency policy for release validation is:

- Release validation includes a `git` dependency resolve path using the shipped binary.
- Missing-`git` diagnostics and unreachable-remote diagnostics are part of the supported GA
  contract.

Release verification policy is:

- installers enforce `SHA256SUMS.txt`
- installers may opt into attestation verification with `--verify-attestations` /
  `-VerifyAttestations` or `NEBULA_INSTALL_VERIFY_ATTESTATIONS=1`
- stronger verification is via `gh attestation verify`
- the checksum file itself must ship with a verifiable attestation bundle
