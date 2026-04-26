#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

read_default() {
  local path="$1" fallback="$2"
  if [[ -f "$path" ]]; then
    tr -d '\r\n' < "$path"
  else
    printf '%s\n' "$fallback"
  fi
}

version="${NEBULA_INSTALL_VERSION:-$(read_default "$repo_root/VERSION" "")}"
repo="${NEBULA_INSTALL_REPOSITORY:-$(read_default "$repo_root/RELEASE_REPOSITORY" "billlza/nebula")}"
prefix="${NEBULA_INSTALL_PREFIX:-$HOME/.local}"
base_url="${NEBULA_INSTALL_BASE_URL:-}"
target_override="${NEBULA_INSTALL_TARGET:-}"
verify_attestations="${NEBULA_INSTALL_VERIFY_ATTESTATIONS:-}"
with_backend_sdk="${NEBULA_INSTALL_WITH_BACKEND_SDK:-}"

usage() {
  cat <<'EOF'
Usage: install.sh [--version VERSION] [--prefix DIR] [--repo OWNER/REPO] [--base-url URL_OR_PATH] [--target TARGET] [--verify-attestations] [--with-backend-sdk]

Defaults:
  version : VERSION file when available
  prefix  : $HOME/.local
  repo    : RELEASE_REPOSITORY file when available, otherwise billlza/nebula
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) version="$2"; shift 2 ;;
    --prefix) prefix="$2"; shift 2 ;;
    --repo) repo="$2"; shift 2 ;;
    --base-url) base_url="$2"; shift 2 ;;
    --target) target_override="$2"; shift 2 ;;
    --verify-attestations) verify_attestations="1"; shift ;;
    --with-backend-sdk) with_backend_sdk="1"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

parse_bool_switch() {
  local raw="$1"
  local lowered
  lowered="$(printf '%s' "$raw" | tr '[:upper:]' '[:lower:]')"
  case "$lowered" in
    1|true|yes|on) echo "1" ;;
    0|false|no|off|"") echo "0" ;;
    *) echo "error: invalid boolean value: $raw" >&2; exit 2 ;;
  esac
}

if [[ -z "$version" ]]; then
  echo "error: version is required when VERSION is unavailable; pass --version or set NEBULA_INSTALL_VERSION" >&2
  exit 2
fi

verify_attestations="$(parse_bool_switch "$verify_attestations")"
with_backend_sdk="$(parse_bool_switch "$with_backend_sdk")"

detect_target() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"
  case "$os/$arch" in
    Darwin/x86_64) echo "darwin-x86_64" ;;
    Darwin/arm64|Darwin/aarch64) echo "darwin-arm64" ;;
    Linux/x86_64) echo "linux-x86_64" ;;
    *) echo "error: unsupported platform: ${os}/${arch}" >&2; exit 1 ;;
  esac
}

asset_name_for_target() {
  case "$1" in
    darwin-x86_64) echo "nebula-v${version}-darwin-x86_64.tar.gz" ;;
    darwin-arm64) echo "nebula-v${version}-darwin-arm64.tar.gz" ;;
    linux-x86_64) echo "nebula-v${version}-linux-x86_64.tar.gz" ;;
    *) echo "error: unsupported install target: $1" >&2; exit 1 ;;
  esac
}

bundle_stem_for_target() {
  case "$1" in
    darwin-x86_64) echo "nebula-v${version}-darwin-x86_64" ;;
    darwin-arm64) echo "nebula-v${version}-darwin-arm64" ;;
    linux-x86_64) echo "nebula-v${version}-linux-x86_64" ;;
    *) echo "error: unsupported install target: $1" >&2; exit 1 ;;
  esac
}

backend_sdk_asset_name() {
  echo "nebula-backend-sdk-v${version}-linux-x86_64.tar.gz"
}

resolved_base_url() {
  if [[ -n "$base_url" ]]; then
    printf '%s\n' "$base_url"
  else
    printf 'https://github.com/%s/releases/download/v%s\n' "$repo" "$version"
  fi
}

download_to() {
  local source="$1" dest="$2"
  if [[ "$source" == file://* ]]; then
    cp "${source#file://}" "$dest"
    return
  fi
  if [[ -f "$source" ]]; then
    cp "$source" "$dest"
    return
  fi
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$source" -o "$dest"
    return
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -qO "$dest" "$source"
    return
  fi
  echo "error: curl or wget is required to download release assets" >&2
  exit 1
}

sha256_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
    return
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
    return
  fi
  echo "error: shasum or sha256sum is required for checksum verification" >&2
  exit 1
}

verify_attestation_prereqs() {
  if ! command -v gh >/dev/null 2>&1; then
    echo "error: gh is required when --verify-attestations is enabled" >&2
    exit 1
  fi
}

run_attestation_verify() {
  local subject="$1" bundle="$2"
  shift 2
  gh attestation verify "$subject" \
    --repo "$repo" \
    --signer-workflow "${repo}/.github/workflows/release.yml" \
    --bundle "$bundle" \
    "$@"
}

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/nebula-install.XXXXXX")"
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

install_state_dir="$prefix/share/nebula"
install_manifest="$install_state_dir/install-manifest.txt"

manifest_entry_safe() {
  local rel="$1"
  [[ -z "$rel" ]] && return 1
  [[ "$rel" = /* ]] && return 1
  local segment
  IFS='/' read -r -a parts <<< "$rel"
  for segment in "${parts[@]}"; do
    [[ -z "$segment" || "$segment" == "." || "$segment" == ".." ]] && return 1
  done
  return 0
}

remove_previous_install() {
  if [[ ! -f "$install_manifest" ]]; then
    return
  fi
  while IFS= read -r rel; do
    [[ -z "$rel" ]] && continue
    if ! manifest_entry_safe "$rel"; then
      echo "warning: skipping unsafe install-manifest entry: $rel" >&2
      continue
    fi
    rm -f "$prefix/$rel"
  done < "$install_manifest"
}

write_install_manifest() {
  mkdir -p "$install_state_dir"
  : > "$install_manifest"
  local current_payload
  for current_payload in "$@"; do
    (
      cd "$current_payload"
      local roots=()
      [[ -d bin ]] && roots+=("bin")
      [[ -d include ]] && roots+=("include")
      [[ -d share ]] && roots+=("share")
      if [[ "${#roots[@]}" -eq 0 ]]; then
        exit 0
      fi
      find "${roots[@]}" -type f 2>/dev/null | LC_ALL=C sort
    ) >> "$install_manifest"
  done
  sort -u "$install_manifest" -o "$install_manifest"
}

target="${target_override:-$(detect_target)}"
asset_name="$(asset_name_for_target "$target")"
source_base="$(resolved_base_url)"
checksums_path="$tmpdir/SHA256SUMS.txt"
asset_path="$tmpdir/$asset_name"
bundle_stem="$(bundle_stem_for_target "$target")"
checksums_bundle_path="$tmpdir/SHA256SUMS.txt.intoto.jsonl"
provenance_bundle_path="$tmpdir/${bundle_stem}.provenance.intoto.jsonl"
sbom_bundle_path="$tmpdir/${bundle_stem}.sbom.intoto.jsonl"
backend_sdk_asset=""
backend_sdk_path=""
backend_sdk_checksums_bundle_path="$tmpdir/backend-sdk-checksums.intoto.jsonl"
backend_sdk_provenance_bundle_path="$tmpdir/backend-sdk.provenance.intoto.jsonl"
backend_sdk_sbom_bundle_path="$tmpdir/backend-sdk.sbom.intoto.jsonl"

if [[ "$with_backend_sdk" == "1" && "$target" != "linux-x86_64" ]]; then
  echo "error: --with-backend-sdk is only supported for the linux-x86_64 install target" >&2
  exit 1
fi

download_to "${source_base%/}/SHA256SUMS.txt" "$checksums_path"
download_to "${source_base%/}/$asset_name" "$asset_path"

expected_sha="$(awk -v target="$asset_name" '$2 == target { print $1 }' "$checksums_path")"
if [[ -z "$expected_sha" ]]; then
  echo "error: missing checksum entry for $asset_name" >&2
  exit 1
fi

actual_sha="$(sha256_of "$asset_path")"
if [[ "$actual_sha" != "$expected_sha" ]]; then
  echo "error: checksum mismatch for $asset_name" >&2
  echo "expected: $expected_sha" >&2
  echo "actual:   $actual_sha" >&2
  exit 1
fi

if [[ "$verify_attestations" == "1" ]]; then
  verify_attestation_prereqs
  download_to "${source_base%/}/SHA256SUMS.txt.intoto.jsonl" "$checksums_bundle_path"
  download_to "${source_base%/}/${bundle_stem}.provenance.intoto.jsonl" "$provenance_bundle_path"
  download_to "${source_base%/}/${bundle_stem}.sbom.intoto.jsonl" "$sbom_bundle_path"

  run_attestation_verify "$checksums_path" "$checksums_bundle_path"
  run_attestation_verify "$asset_path" "$provenance_bundle_path"
  run_attestation_verify "$asset_path" "$sbom_bundle_path" --predicate-type "https://spdx.dev/Document/v2.3"
fi

if [[ "$with_backend_sdk" == "1" ]]; then
  backend_sdk_asset="$(backend_sdk_asset_name)"
  backend_sdk_path="$tmpdir/$backend_sdk_asset"
  download_to "${source_base%/}/$backend_sdk_asset" "$backend_sdk_path"
  backend_expected_sha="$(awk -v target="$backend_sdk_asset" '$2 == target { print $1 }' "$checksums_path")"
  if [[ -z "$backend_expected_sha" ]]; then
    echo "error: missing checksum entry for $backend_sdk_asset" >&2
    exit 1
  fi
  backend_actual_sha="$(sha256_of "$backend_sdk_path")"
  if [[ "$backend_actual_sha" != "$backend_expected_sha" ]]; then
    echo "error: checksum mismatch for $backend_sdk_asset" >&2
    echo "expected: $backend_expected_sha" >&2
    echo "actual:   $backend_actual_sha" >&2
    exit 1
  fi

  if [[ "$verify_attestations" == "1" ]]; then
    download_to "${source_base%/}/${backend_sdk_asset%.tar.gz}.provenance.intoto.jsonl" "$backend_sdk_provenance_bundle_path"
    download_to "${source_base%/}/${backend_sdk_asset%.tar.gz}.sbom.intoto.jsonl" "$backend_sdk_sbom_bundle_path"
    run_attestation_verify "$backend_sdk_path" "$backend_sdk_provenance_bundle_path"
    run_attestation_verify "$backend_sdk_path" "$backend_sdk_sbom_bundle_path" --predicate-type "https://spdx.dev/Document/v2.3"
  fi
fi

extract_root="$tmpdir/extract"
mkdir -p "$extract_root"
tar -xzf "$asset_path" -C "$extract_root"

payload_dir="$extract_root/nebula-v${version}-${target}"
binary_src="$payload_dir/bin/nebula"
runtime_header="$payload_dir/include/runtime/nebula_runtime.hpp"
if [[ ! -x "$binary_src" ]]; then
  echo "error: extracted archive missing executable: $binary_src" >&2
  exit 1
fi
if [[ ! -f "$runtime_header" ]]; then
  echo "error: extracted archive missing runtime headers: $runtime_header" >&2
  exit 1
fi

backend_payload_dir=""
if [[ "$with_backend_sdk" == "1" ]]; then
  tar -xzf "$backend_sdk_path" -C "$extract_root"
  backend_payload_dir="$extract_root/$(basename "${backend_sdk_asset%.tar.gz}")"
  backend_manifest="$backend_payload_dir/share/nebula/sdk/backend/nebula-service/nebula.toml"
  backend_observe_manifest="$backend_payload_dir/share/nebula/sdk/backend/nebula-observe/nebula.toml"
  backend_auth_manifest="$backend_payload_dir/share/nebula/sdk/backend/nebula-auth/nebula.toml"
  backend_config_manifest="$backend_payload_dir/share/nebula/sdk/backend/nebula-config/nebula.toml"
  backend_db_manifest="$backend_payload_dir/share/nebula/sdk/backend/nebula-db-sqlite/nebula.toml"
  if [[ ! -f "$backend_manifest" || ! -f "$backend_observe_manifest" || ! -f "$backend_auth_manifest" || ! -f "$backend_config_manifest" || ! -f "$backend_db_manifest" ]]; then
    echo "error: extracted backend SDK archive missing package manifest: $backend_manifest" >&2
    exit 1
  fi
fi

mkdir -p "$prefix"
remove_previous_install

install_bin_dir="$prefix/bin"
mkdir -p "$install_bin_dir"
cp -R "$payload_dir/bin/." "$install_bin_dir/"
chmod +x "$install_bin_dir/nebula"

if [[ -d "$payload_dir/include" ]]; then
  install_include_dir="$prefix/include"
  mkdir -p "$install_include_dir"
  cp -R "$payload_dir/include/." "$install_include_dir/"
fi

if [[ -d "$payload_dir/share" ]]; then
  install_share_dir="$prefix/share"
  mkdir -p "$install_share_dir"
  cp -R "$payload_dir/share/." "$install_share_dir/"
fi

if [[ -n "$backend_payload_dir" && -d "$backend_payload_dir/share" ]]; then
  install_share_dir="$prefix/share"
  mkdir -p "$install_share_dir"
  cp -R "$backend_payload_dir/share/." "$install_share_dir/"
fi

if [[ -n "$backend_payload_dir" ]]; then
  write_install_manifest "$payload_dir" "$backend_payload_dir"
else
  write_install_manifest "$payload_dir"
fi

echo "Installed Nebula to $install_bin_dir/nebula"
"$install_bin_dir/nebula" --version
echo "Verified archive integrity against SHA256SUMS.txt before installation."
if [[ "$verify_attestations" == "1" ]]; then
  echo "Verified checksum, provenance, and SBOM attestations with gh before installation."
fi
echo "Nebula build/run/test/bench uses a host C++23 compiler. Default contract: clang++ when CXX is unset."
if [[ "$with_backend_sdk" == "1" ]]; then
  echo "Installed the Nebula backend SDK under $prefix/share/nebula/sdk/backend."
  echo "That payload includes GA backend packages (nebula-service, nebula-observe) plus preview installed packages nebula-auth, nebula-config, and nebula-db-sqlite."
else
  echo "Nebula backend SDK is not installed by default; pass --with-backend-sdk on Linux to install it."
fi
echo "Hosted registry helpers ship under $prefix/share/nebula/registry; --registry-url workflows require Python 3.11+ on PATH, or set PYTHON to a compatible interpreter."
echo "For upgrade/rollback/uninstall and stronger provenance/SBOM verification, see $prefix/share/doc/nebula/install_lifecycle.md and $prefix/share/doc/nebula/release_verification.md."
echo "Set CXX=/path/to/clang++ if you need to override the host compiler explicitly."
echo "Git-backed dependencies require git on PATH before nebula add --git, fetch, or update."

case ":$PATH:" in
  *":$install_bin_dir:"*) ;;
  *) echo "Add $install_bin_dir to PATH if it is not already available in your shell startup." ;;
esac
