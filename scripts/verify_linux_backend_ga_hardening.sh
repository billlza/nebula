#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: verify_linux_backend_ga_hardening.sh --dist-dir <path> [--binary <path>] [--version <version>] [--verify-attestations]

Verify the Linux x86_64 backend GA clean-room contract against a packaged release directory.
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary=""
dist_dir=""
version=""
verify_attestations=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary)
      binary="${2:-}"
      shift 2
      ;;
    --dist-dir)
      dist_dir="${2:-}"
      shift 2
      ;;
    --version)
      version="${2:-}"
      shift 2
      ;;
    --verify-attestations)
      verify_attestations=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 64
      ;;
  esac
done

if [[ -z "$dist_dir" ]]; then
  echo "error: --dist-dir is required" >&2
  usage >&2
  exit 64
fi

if [[ -z "$version" ]]; then
  version="$(tr -d '\r\n' < "$repo_root/VERSION")"
fi

if [[ -n "$binary" ]]; then
  binary="$(python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$binary")"
fi
dist_dir="$(python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$dist_dir")"

if ! command -v file >/dev/null 2>&1; then
  echo "error: required tool not found: file" >&2
  exit 1
fi

core_asset="$dist_dir/nebula-v${version}-linux-x86_64.tar.gz"
backend_asset="$dist_dir/nebula-backend-sdk-v${version}-linux-x86_64.tar.gz"
test -f "$core_asset"
test -f "$backend_asset"

if [[ ! -f "$dist_dir/SHA256SUMS.txt" ]]; then
  (
    cd "$dist_dir"
    sha256sum "$(basename "$core_asset")" "$(basename "$backend_asset")" > SHA256SUMS.txt
  )
fi

work_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/nebula-linux-ga-hardening.XXXXXX")"
cleanup() {
  rm -rf "$work_root"
}
trap cleanup EXIT

check_linux_x86_64_binary() {
  local path="$1"
  local label="$2"
  local binary_info
  binary_info="$(file "$path")"
  printf '%s\n' "$binary_info"
  if ! grep -Eq 'ELF 64-bit.*x86-64' <<<"$binary_info"; then
    echo "error: $label is not an ELF x86-64 binary: $path" >&2
    exit 1
  fi
}

if [[ -n "$binary" ]]; then
  check_linux_x86_64_binary "$binary" "source build binary"
fi

core_extract_root="$work_root/core-asset"
mkdir -p "$core_extract_root"
tar -xzf "$core_asset" -C "$core_extract_root"
packaged_bin="$core_extract_root/nebula-v${version}-linux-x86_64/bin/nebula"
test -x "$packaged_bin"
check_linux_x86_64_binary "$packaged_bin" "packaged release binary"

install_root="$work_root/prefix"
install_args=(
  --version "$version"
  --prefix "$install_root"
  --base-url "$dist_dir"
  --with-backend-sdk
)
if [[ "$verify_attestations" -eq 1 ]]; then
  install_args+=(--verify-attestations)
fi
bash "$repo_root/scripts/install.sh" "${install_args[@]}"

installed_bin="$install_root/bin/nebula"
test -x "$installed_bin"
test -f "$install_root/share/nebula/sdk/backend/nebula-service/nebula.toml"
test -f "$install_root/share/nebula/sdk/backend/nebula-observe/nebula.toml"
test -f "$install_root/share/nebula/sdk/backend/nebula-auth/nebula.toml"
test -f "$install_root/share/nebula/sdk/backend/nebula-config/nebula.toml"
test -f "$install_root/share/nebula/sdk/backend/nebula-db-sqlite/nebula.toml"
test -f "$install_root/share/nebula/sdk/backend/examples/hello_api/nebula.toml"

project_root="$work_root/backend-service"
"$installed_bin" new "$project_root" --template backend-service
grep -F 'installed = "nebula-service"' "$project_root/nebula.toml"
"$installed_bin" fetch "$project_root"
BACKEND_LOCK="$project_root/nebula.lock" python3 - <<'PY'
import os
from pathlib import Path

lock = Path(os.environ["BACKEND_LOCK"]).read_text(encoding="utf-8")
blocks = [block for block in lock.split("\n[[package]]\n") if block.strip()]
if not any('name = "nebula-service"' in block and 'source_kind = "installed"' in block for block in blocks):
    raise SystemExit("backend-service lock missing installed nebula-service entry")
PY

echo "linux-backend-ga-hardening-ok"
