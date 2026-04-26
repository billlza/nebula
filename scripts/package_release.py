#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import stat
import subprocess
import tarfile
import zipfile
from pathlib import Path

from release_lib import (
    archive_name,
    backend_sdk_archive_name,
    backend_sdk_stage_name,
    bundle_windows_runtime_deps,
    ensure_supported_target,
    find_cmake_build_dir,
    read_repo_version,
    repo_root_from,
    staging_dir_name,
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Package a Nebula release asset")
    p.add_argument("--binary", required=True, help="path to built nebula binary")
    p.add_argument("--target", required=True, help="release target, e.g. darwin-arm64")
    p.add_argument("--output-dir", required=True, help="directory for packaged artifacts")
    p.add_argument("--include-backend-sdk", action="store_true", help="also package the Linux backend SDK asset")
    return p.parse_args()


def make_executable(path: Path) -> None:
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def ensure_no_preview_packages(stage_root: Path) -> None:
    offenders: list[str] = []
    for path in sorted(stage_root.rglob("*")):
        rel = path.relative_to(stage_root)
        if "official" in rel.parts:
            offenders.append(rel.as_posix())
    if offenders:
        preview_list = "\n".join(f"- {item}" for item in offenders)
        raise SystemExit(
            "release staging tree unexpectedly contains repo-local preview packages under official/:\n"
            + preview_list
        )


def _copytree(src: Path, dest: Path) -> None:
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))


def render_installed_hello_api_manifest() -> str:
    return """schema_version = 1

[package]
name = "hello-api"
version = "0.1.0"
entry = "src/main.nb"
src_dir = "src"

[dependencies]
service = { installed = "nebula-service" }
"""


def render_installed_hello_api_readme() -> str:
    return """# Hello API

Installed backend SDK example built on `nebula-service`.

Fetch dependencies with:

```bash
nebula fetch .
```

Run it with:

```bash
nebula run . --run-gate none
```

Override bind settings with the service env vars:

```bash
NEBULA_BIND_HOST=127.0.0.1 NEBULA_PORT=8080 nebula run . --run-gate none
```

Enter draining mode with a sentinel file:

```bash
touch /tmp/hello-api.draining
NEBULA_DRAIN_FILE=/tmp/hello-api.draining nebula run . --run-gate none
```

While draining:

- `/healthz` stays `200`
- `/readyz` flips to `503`
- business routes return a JSON draining error
- the server exits after a quiet poll interval once no new requests arrive

This example uses the service SDK's explicit route-composition and middleware helpers:

- `service::routing::dispatch_ctx3_result(...)` for health/ready/business routing
- `service::middleware::reject_when_draining_result(...)` to gate business traffic while draining

Connection lifecycle stays intentionally narrow in this wave: each accepted connection handles one
request/response and is then closed. Keep-alive remains out of scope.

Collector-side `/metrics` bridge:

```bash
nebula run . --run-gate none 2> hello-api.observe.ndjson
python3 ../../nebula-observe/prometheus_bridge.py serve --input hello-api.observe.ndjson --port 9464
curl http://127.0.0.1:9464/metrics
```

This helper is intentionally narrow: it translates logged `nebula.observe.metric.v1`
delta-counter events into Prometheus text and does not turn the example itself into a built-in
Prometheus exporter.
"""


def package_backend_sdk(repo_root: Path, version: str, output_dir: Path) -> Path:
    stage_root = output_dir / backend_sdk_stage_name(version)
    if stage_root.exists():
        shutil.rmtree(stage_root)

    sdk_root = stage_root / "share" / "nebula" / "sdk" / "backend"
    sdk_root.mkdir(parents=True, exist_ok=True)
    _copytree(repo_root / "official" / "nebula-service", sdk_root / "nebula-service")
    service_manifest = sdk_root / "nebula-service" / "nebula.toml"
    service_manifest.write_text(
        service_manifest.read_text(encoding="utf-8").replace(
            'tlss = { path = "../nebula-tls-server" }',
            "",
        ),
        encoding="utf-8",
    )
    service_tls_module = sdk_root / "nebula-service" / "src" / "tls.nb"
    if service_tls_module.exists():
        service_tls_module.unlink()
    service_readme = sdk_root / "nebula-service" / "README.md"
    if service_readme.exists():
        service_readme.write_text(
            "Installed SDK note: this payload exposes the Linux backend service GA subset. "
            "The preview `service::tls` adapter remains repo-local and is not shipped in the "
            "installed `nebula-service` package.\n\n"
            + service_readme.read_text(encoding="utf-8"),
            encoding="utf-8",
        )
    _copytree(repo_root / "official" / "nebula-observe", sdk_root / "nebula-observe")
    _copytree(repo_root / "official" / "nebula-auth", sdk_root / "nebula-auth")
    _copytree(repo_root / "official" / "nebula-config", sdk_root / "nebula-config")
    _copytree(repo_root / "official" / "nebula-db-sqlite", sdk_root / "nebula-db-sqlite")
    hello_api_root = sdk_root / "examples" / "hello_api"
    _copytree(repo_root / "examples" / "hello_api", hello_api_root)
    (hello_api_root / "nebula.toml").write_text(render_installed_hello_api_manifest(), encoding="utf-8")
    (hello_api_root / "README.md").write_text(render_installed_hello_api_readme(), encoding="utf-8")
    for lock_path in sorted(stage_root.rglob("nebula.lock")):
        lock_path.unlink()

    docs_root = sdk_root / "docs"
    docs_root.mkdir(parents=True, exist_ok=True)
    for rel in [
        "README.md",
        "docs/service_profile.md",
        "docs/support_matrix.md",
        "docs/reverse_proxy_deployment.md",
        "docs/backend_operator_guide.md",
        "docs/official_package_tiering.md",
    ]:
        src = repo_root / rel
        dest = docs_root / src.name
        shutil.copy2(src, dest)

    archive_path = output_dir / backend_sdk_archive_name(version)
    if archive_path.exists():
        archive_path.unlink()
    with tarfile.open(archive_path, "w:gz") as tf:
        tf.add(stage_root, arcname=stage_root.name)
    return archive_path


def main() -> int:
    args = parse_args()
    script_path = Path(__file__)
    repo_root = repo_root_from(script_path)
    version = read_repo_version(repo_root)
    target = ensure_supported_target(args.target)
    binary = Path(args.binary).resolve()
    build_dir = find_cmake_build_dir(binary)
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    stage_root = output_dir / staging_dir_name(version, target)
    if stage_root.exists():
        shutil.rmtree(stage_root)
    subprocess.run(["cmake", "--install", str(build_dir), "--prefix", str(stage_root)], check=True)
    ensure_no_preview_packages(stage_root)

    staged_binary = stage_root / "bin" / target.binary_name
    if not staged_binary.exists():
        raise SystemExit(f"installed release tree missing binary: {staged_binary}")
    if target.platform != "windows":
        make_executable(staged_binary)
    else:
        bundle_windows_runtime_deps(binary, stage_root / "bin")

    archive_path = output_dir / archive_name(version, target)
    if archive_path.exists():
        archive_path.unlink()

    if target.archive_kind == "tar.gz":
        with tarfile.open(archive_path, "w:gz") as tf:
            tf.add(stage_root, arcname=stage_root.name)
    else:
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(stage_root.rglob("*")):
                if path.is_dir():
                    continue
                zf.write(path, arcname=str(path.relative_to(output_dir)))

    print(str(archive_path))
    if args.include_backend_sdk:
        if target.target != "linux-x86_64":
            raise SystemExit("--include-backend-sdk is only supported for the linux-x86_64 release target")
        backend_sdk_archive = package_backend_sdk(repo_root, version, output_dir)
        print(str(backend_sdk_archive))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
