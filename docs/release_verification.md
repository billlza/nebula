# Release Verification

Nebula 1.0 publishes multiple integrity layers:

- platform archive
- per-platform SPDX SBOM sidecar: `nebula-v<version>-<target>.spdx.json`
- per-platform provenance attestation bundle:
  `nebula-v<version>-<target>.provenance.intoto.jsonl`
- per-platform SBOM attestation bundle:
  `nebula-v<version>-<target>.sbom.intoto.jsonl`
- `SHA256SUMS.txt`
- checksum attestation bundle: `SHA256SUMS.txt.intoto.jsonl`

The install scripts verify `SHA256SUMS.txt` only. Strong verification should happen before running
the installers unless you opt into the installer attestation flow.

## Installer Opt-In Verification

The install scripts support opt-in strong verification:

```bash
bash scripts/install.sh --verify-attestations --version <version>
pwsh -File scripts/install.ps1 -VerifyAttestations -Version <version>
```

or by setting:

```bash
NEBULA_INSTALL_VERIFY_ATTESTATIONS=1
```

That path requires `gh` and verifies the checksum file plus the selected archive's provenance and
SBOM bundles before extraction.

When you install with `--with-backend-sdk` on Linux x86_64, the strong-verification path also
verifies `nebula-backend-sdk-v<version>-linux-x86_64.tar.gz` with its provenance and SBOM bundles
before the backend SDK payload is installed.

If your artifacts come from a mirror or a fork, use the matching repository identifier with the
installer's `--repo` / `NEBULA_INSTALL_REPOSITORY` so attestation verification targets the actual
signer repository.

## Online Verification

Provenance:

```bash
gh attestation verify nebula-v<version>-<target>.<ext> \
  --repo billlza/nebula \
  --signer-workflow billlza/nebula/.github/workflows/release.yml
```

SBOM attestation:

```bash
gh attestation verify nebula-v<version>-<target>.<ext> \
  --repo billlza/nebula \
  --signer-workflow billlza/nebula/.github/workflows/release.yml \
  --predicate-type https://spdx.dev/Document/v2.3
```

Checksums:

```bash
gh attestation verify SHA256SUMS.txt \
  --repo billlza/nebula \
  --signer-workflow billlza/nebula/.github/workflows/release.yml
```

## Offline Verification With Bundles

If you downloaded the release-sidecar bundles, verify against them directly:

```bash
gh attestation verify nebula-v<version>-<target>.<ext> \
  --repo billlza/nebula \
  --signer-workflow billlza/nebula/.github/workflows/release.yml \
  --bundle nebula-v<version>-<target>.provenance.intoto.jsonl
```

```bash
gh attestation verify nebula-v<version>-<target>.<ext> \
  --repo billlza/nebula \
  --signer-workflow billlza/nebula/.github/workflows/release.yml \
  --predicate-type https://spdx.dev/Document/v2.3 \
  --bundle nebula-v<version>-<target>.sbom.intoto.jsonl
```

```bash
gh attestation verify SHA256SUMS.txt \
  --repo billlza/nebula \
  --signer-workflow billlza/nebula/.github/workflows/release.yml \
  --bundle SHA256SUMS.txt.intoto.jsonl
```

## Minimum Recommended Flow

1. Verify `SHA256SUMS.txt` with its attestation.
2. Verify the platform archive with provenance.
3. Verify the same archive with the SPDX SBOM predicate.
4. Run the installer only after those checks succeed.

If you are installing the optional backend SDK on Linux x86_64, also verify the backend SDK
archive with provenance and the SPDX SBOM predicate before running
`install.sh --with-backend-sdk --verify-attestations`.

The opt-in installer flow performs the same sequence for the selected platform artifact.

`release-manifest.json` is the machine-readable index for the release page and references archives,
SBOM sidecars, and attestation sidecars by name.

Its `metadata.manifest.sha256` field is a canonical digest over the manifest body with the
self-referential hash slot blanked before hashing. That field is meant for stable machine
comparison, not as a literal `sha256sum release-manifest.json` value.

If you downloaded the assembled `release-bundle` artifact from the GitHub release dry-run, you can
wrap the same checks in one local command:

```bash
python3 scripts/release_signoff.py \
  --artifact-dir work/release-bundle \
  --verify-attestations \
  --json-out work/release-signoff.json \
  --markdown-out work/release-signoff.md
```
