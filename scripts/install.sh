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

version="${NEBULA_INSTALL_VERSION:-$(read_default "$repo_root/VERSION" "1.0.0")}"
repo="${NEBULA_INSTALL_REPOSITORY:-$(read_default "$repo_root/RELEASE_REPOSITORY" "billlza/nebula")}"
prefix="${NEBULA_INSTALL_PREFIX:-$HOME/.local}"
base_url="${NEBULA_INSTALL_BASE_URL:-}"
target_override="${NEBULA_INSTALL_TARGET:-}"

usage() {
  cat <<'EOF'
Usage: install.sh [--version VERSION] [--prefix DIR] [--repo OWNER/REPO] [--base-url URL_OR_PATH] [--target TARGET]

Defaults:
  version : VERSION file when available, otherwise 1.0.0
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
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

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

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/nebula-install.XXXXXX")"
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

target="${target_override:-$(detect_target)}"
asset_name="$(asset_name_for_target "$target")"
source_base="$(resolved_base_url)"
checksums_path="$tmpdir/SHA256SUMS.txt"
asset_path="$tmpdir/$asset_name"

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

extract_root="$tmpdir/extract"
mkdir -p "$extract_root"
tar -xzf "$asset_path" -C "$extract_root"

payload_dir="$extract_root/nebula-v${version}-${target}"
binary_src="$payload_dir/bin/nebula"
if [[ ! -x "$binary_src" ]]; then
  echo "error: extracted archive missing executable: $binary_src" >&2
  exit 1
fi

install_bin_dir="$prefix/bin"
mkdir -p "$install_bin_dir"
cp "$binary_src" "$install_bin_dir/nebula"
chmod +x "$install_bin_dir/nebula"

echo "Installed Nebula to $install_bin_dir/nebula"
"$install_bin_dir/nebula" --version

case ":$PATH:" in
  *":$install_bin_dir:"*) ;;
  *) echo "Add $install_bin_dir to PATH if it is not already available in your shell startup." ;;
esac
