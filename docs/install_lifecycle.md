# Install Lifecycle

Nebula 1.0 GA covers the compiler, CLI, package workflow, bundled `std`, runtime headers, release
documentation, and the optional Linux backend SDK asset.

The binary archives, install scripts, and Homebrew formula do **not** install:

- a host C++23 compiler
- `git`
- Python 3.11+ for hosted-registry `--registry-url` workflows
- repo-local preview packages that remain outside the backend SDK surface under `official/`

## Prerequisites

- Supported host for compiler/tooling release assets:
  - macOS x86_64
  - macOS arm64
  - Linux x86_64
  - Windows x86_64
- Host compiler for `nebula build`, `nebula run`, `nebula test`, and `nebula bench`:
  - default contract is `clang++` when `CXX` is unset
- `git` on `PATH` for `nebula add --git`, `nebula fetch`, and `nebula update`
- Python 3.11+ on `PATH` for hosted-registry `nebula publish/fetch/update --registry-url ...`
  workflows, or `PYTHON=/path/to/python3.11+`; Nebula preflights that helper requirement before
  invoking the registry client
- A repo checkout when you want to consume remaining preview packages via `path` dependencies
- Linux x86_64 when you want the installed backend SDK via `--with-backend-sdk`

For stronger release verification before install, see `docs/release_verification.md`.

## Strong Verification

Default installer behavior verifies only `SHA256SUMS.txt`.

Opt in to stronger verification with:

```bash
bash scripts/install.sh --verify-attestations --version <version>
pwsh -File scripts/install.ps1 -VerifyAttestations -Version <version>
```

or:

```bash
NEBULA_INSTALL_VERIFY_ATTESTATIONS=1
```

Install the Linux backend SDK alongside the core CLI/tooling surface with:

```bash
bash scripts/install.sh --with-backend-sdk --version <version>
```

When enabled, the installers:

- require `gh` on `PATH`
- verify `SHA256SUMS.txt` with `SHA256SUMS.txt.intoto.jsonl`
- verify the selected platform archive with its provenance bundle
- verify the same archive with its SBOM predicate bundle
- when `--with-backend-sdk` is selected on Linux x86_64, also verify the backend SDK archive with
  its provenance and SBOM bundles before extraction

If you install from a mirror or a fork via `--base-url`, also pass `--repo` (or
`NEBULA_INSTALL_REPOSITORY`) so `gh attestation verify` checks against the correct signer
repository.

They fail before extraction or copy if any bundle is missing or any verification command fails.

## Upgrade

Install a newer version into the same prefix:

```bash
bash scripts/install.sh --version <new-version>
pwsh -File scripts/install.ps1 -Version <new-version>
```

The installers replace the files recorded in Nebula's install manifest and leave unrelated files in
the prefix alone.

Recommended post-upgrade checks:

- `nebula --version`
- `nebula new /tmp/nebula-smoke`
- `nebula run /tmp/nebula-smoke --run-gate none`

## Rollback

Rollback is the same flow as upgrade, but with an older version:

```bash
bash scripts/install.sh --version <older-version>
pwsh -File scripts/install.ps1 -Version <older-version>
```

Always verify the target release artifacts before reinstalling them.

## Uninstall

Nebula records managed files in `share/nebula/install-manifest.txt` under the install prefix.

Typical uninstall targets are:

- `bin/nebula` or `bin/nebula.exe`
- `include/runtime/*`
- `share/nebula/registry/*`
- `share/nebula/std/*`
- `share/nebula/sdk/backend/*` when the backend SDK was installed
- `share/doc/nebula/*`
- `share/nebula/install-manifest.txt`

Installed backend SDK docs currently include:

- `share/nebula/sdk/backend/docs/service_profile.md`
- `share/nebula/sdk/backend/docs/reverse_proxy_deployment.md`
- `share/nebula/sdk/backend/docs/backend_operator_guide.md`
- `share/nebula/sdk/backend/nebula-db-sqlite/nebula.toml` as an installed preview package payload

If you installed into the default prefix, remove only Nebula-managed files, not the entire prefix,
unless you created that prefix exclusively for Nebula.

## Failure Behavior

The install scripts fail before extracting or copying files when they detect any of the following:

- unsupported host target
- missing checksum entry for the requested archive
- checksum mismatch
- missing executable or runtime header inside the extracted payload

The default installer flow verifies only `SHA256SUMS.txt`. For signed provenance and SBOM
verification, either use the installer opt-in flow above or the manual attestation flow documented
in `docs/release_verification.md`.
