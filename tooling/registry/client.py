#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path
from typing import Any
from urllib import error, request
from urllib.parse import quote

try:
    import tomllib
except ModuleNotFoundError as exc:
    raise SystemExit("nebula hosted registry client requires Python 3.11+ (or set PYTHON to a compatible interpreter)") from exc


def read_toml(path: Path) -> dict[str, Any]:
    return tomllib.loads(path.read_text(encoding="utf-8"))


def registry_headers(token: str | None) -> dict[str, str]:
    headers = {"User-Agent": "nebula-registry-client/0.1"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


def default_timeout_seconds() -> float:
    raw = os.environ.get("NEBULA_REGISTRY_TIMEOUT_SECONDS", "15")
    try:
        timeout = float(raw)
    except ValueError as exc:
        raise SystemExit(f"invalid NEBULA_REGISTRY_TIMEOUT_SECONDS: {raw}") from exc
    if timeout <= 0:
        raise SystemExit("NEBULA_REGISTRY_TIMEOUT_SECONDS must be > 0")
    return timeout


def http_json(url: str, token: str | None, timeout_seconds: float) -> dict[str, Any]:
    req = request.Request(url, headers=registry_headers(token), method="GET")
    with request.urlopen(req, timeout=timeout_seconds) as response:
        return json.loads(response.read().decode("utf-8"))


def http_download(url: str, token: str | None, timeout_seconds: float) -> bytes:
    req = request.Request(url, headers=registry_headers(token), method="GET")
    with request.urlopen(req, timeout=timeout_seconds) as response:
        return response.read()


def http_put_archive(url: str, token: str | None, body: bytes, timeout_seconds: float) -> dict[str, Any]:
    headers = registry_headers(token)
    headers["Content-Type"] = "application/gzip"
    headers["Content-Length"] = str(len(body))
    req = request.Request(url, headers=headers, data=body, method="PUT")
    with request.urlopen(req, timeout=timeout_seconds) as response:
        return json.loads(response.read().decode("utf-8"))


def manifest_path_for(project: Path) -> Path:
    if project.is_dir():
        return project / "nebula.toml"
    return project


def package_identity(project: Path) -> tuple[str, str]:
    manifest = read_toml(manifest_path_for(project))
    package = manifest.get("package")
    if not isinstance(package, dict):
        raise SystemExit(f"publish target has no [package] manifest: {project}")
    return str(package["name"]), str(package["version"])


def registry_root_for(project: Path, override: str | None) -> Path:
    if override:
        return Path(override).resolve()
    env = os.environ.get("NEBULA_REGISTRY_ROOT")
    if env:
        return Path(env).resolve()
    return (project / ".nebula" / "registry").resolve()


def compare_trees(lhs: Path, rhs: Path) -> bool:
    lhs_files = sorted(path for path in lhs.rglob("*") if path.is_file())
    rhs_files = sorted(path for path in rhs.rglob("*") if path.is_file())
    lhs_rel = [path.relative_to(lhs) for path in lhs_files]
    rhs_rel = [path.relative_to(rhs) for path in rhs_files]
    if lhs_rel != rhs_rel:
        return False
    for rel in lhs_rel:
        if (lhs / rel).read_bytes() != (rhs / rel).read_bytes():
            return False
    return True


def validate_archive_members(archive: tarfile.TarFile) -> None:
    for member in archive.getmembers():
        member_path = Path(member.name)
        if member_path.is_absolute() or ".." in member_path.parts:
            raise RuntimeError("archive contains unsafe path")
        if member.isfile() or member.isdir():
            continue
        raise RuntimeError("archive contains unsupported entry type")


def mirror_package(server: str, token: str | None, name: str, version: str, out_root: Path,
                   timeout_seconds: float, force: bool = False) -> Path:
    url = f"{server.rstrip('/')}/v1/packages/{quote(name)}/{quote(version)}/archive"
    body = http_download(url, token, timeout_seconds)
    target = out_root / name / version
    with tempfile.TemporaryDirectory(prefix="nebula-registry-mirror-") as tmp_dir:
        extract_root = Path(tmp_dir) / "pkg"
        extract_root.mkdir(parents=True, exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(body), mode="r:gz") as archive:
            validate_archive_members(archive)
            archive.extractall(extract_root)
        if target.exists():
            if compare_trees(extract_root, target):
                return target
            if not force:
                raise RuntimeError(f"local mirror already exists with different contents: {target}")
            shutil.rmtree(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(extract_root), str(target))
    return target


def collect_version_dependencies(manifest_path: Path) -> list[tuple[str, str]]:
    manifest = read_toml(manifest_path)
    deps = manifest.get("dependencies", {})
    out: list[tuple[str, str]] = []
    for alias, value in deps.items():
        if isinstance(value, str):
            out.append((str(alias), value))
    return out


def mirror_dependency_closure(server: str, token: str | None, manifest_path: Path,
                              registry_root: Path, timeout_seconds: float) -> None:
    queue = collect_version_dependencies(manifest_path)
    seen: set[tuple[str, str]] = set()
    while queue:
        name, version = queue.pop(0)
        key = (name, version)
        if key in seen:
            continue
        seen.add(key)
        package_root = mirror_package(server, token, name, version, registry_root, timeout_seconds)
        queue.extend(collect_version_dependencies(package_root / "nebula.toml"))


def run_nebula(binary: str, args: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run([binary, *args], cwd=cwd, env=env, text=True, capture_output=True)


def command_list(args: argparse.Namespace) -> int:
    url = f"{args.server.rstrip('/')}/v1/packages"
    if args.query:
        url += f"?q={quote(args.query)}"
    payload = http_json(url, args.token, args.timeout_seconds)
    for package in payload.get("packages", []):
        versions = ",".join(package.get("versions", []))
        print(f"{package['name']} {versions}".rstrip())
    return 0


def command_search(args: argparse.Namespace) -> int:
    payload = http_json(f"{args.server.rstrip('/')}/v1/packages?q={quote(args.query)}", args.token,
                        args.timeout_seconds)
    for package in payload.get("packages", []):
        versions = ",".join(package.get("versions", []))
        print(f"{package['name']} {versions}".rstrip())
    return 0


def command_mirror(args: argparse.Namespace) -> int:
    target = mirror_package(args.server, args.token, args.name, args.version, Path(args.out_root).resolve(),
                            args.timeout_seconds, args.force)
    print(f"mirrored: {args.name}@{args.version} -> {target}")
    return 0


def command_push(args: argparse.Namespace) -> int:
    project = Path(args.project).resolve()
    name, version = package_identity(project)
    binary = args.nebula_binary
    with tempfile.TemporaryDirectory(prefix="nebula-registry-push-") as tmp_dir:
        temp_root = Path(tmp_dir) / "registry"
        temp_root.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ)
        env["NEBULA_REGISTRY_ROOT"] = str(temp_root)
        result = run_nebula(binary, ["publish", str(project), *(["--force"] if args.force else [])], cwd=project.parent, env=env)
        if result.returncode != 0:
            print(result.stdout, end="")
            print(result.stderr, end="")
            return result.returncode

        package_root = temp_root / name / version
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
            archive.add(package_root, arcname=".")
        url = f"{args.server.rstrip('/')}/v1/packages/{quote(name)}/{quote(version)}/archive"
        if args.force:
            url += "?force=1"
        payload = http_put_archive(url, args.token, buffer.getvalue(), args.timeout_seconds)
        print(f"{payload['status']}: {name}@{version}")
    return 0


def command_fetch(args: argparse.Namespace) -> int:
    project = Path(args.project).resolve()
    binary = args.nebula_binary
    registry_root = registry_root_for(project if project.is_dir() else project.parent, args.registry_root)
    registry_root.mkdir(parents=True, exist_ok=True)

    manifest_path = manifest_path_for(project)
    mirror_dependency_closure(args.server, args.token, manifest_path, registry_root, args.timeout_seconds)

    env = dict(os.environ)
    env["NEBULA_REGISTRY_ROOT"] = str(registry_root)
    result = run_nebula(binary, ["fetch", str(project)], cwd=project.parent if project.is_file() else project, env=env)
    print(result.stdout, end="")
    print(result.stderr, end="")
    return result.returncode


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Nebula hosted registry client")
    parser.add_argument("--server", default=os.environ.get("NEBULA_REGISTRY_URL"), help="Hosted registry base URL")
    parser.add_argument("--token", default=os.environ.get("NEBULA_REGISTRY_TOKEN"), help="Bearer token")
    parser.add_argument("--nebula-binary", default=os.environ.get("NEBULA_BINARY", "nebula"))
    parser.add_argument("--timeout-seconds", type=float, default=default_timeout_seconds(),
                        help="HTTP timeout for registry requests")

    subparsers = parser.add_subparsers(dest="command", required=True)

    list_parser = subparsers.add_parser("list", help="List packages")
    list_parser.add_argument("--query", default="")
    list_parser.set_defaults(func=command_list)

    search_parser = subparsers.add_parser("search", help="Search packages")
    search_parser.add_argument("query")
    search_parser.set_defaults(func=command_search)

    mirror_parser = subparsers.add_parser("mirror", help="Mirror a package into a local registry root")
    mirror_parser.add_argument("name")
    mirror_parser.add_argument("version")
    mirror_parser.add_argument("--out-root", required=True)
    mirror_parser.add_argument("--force", action="store_true")
    mirror_parser.set_defaults(func=command_mirror)

    push_parser = subparsers.add_parser("push", help="Publish a project to a hosted registry")
    push_parser.add_argument("project")
    push_parser.add_argument("--force", action="store_true")
    push_parser.set_defaults(func=command_push)

    fetch_parser = subparsers.add_parser("fetch", help="Mirror remote exact-version deps, then run nebula fetch")
    fetch_parser.add_argument("project")
    fetch_parser.add_argument("--registry-root")
    fetch_parser.set_defaults(func=command_fetch)

    args = parser.parse_args()
    if not args.server and args.command in {"list", "search", "mirror", "push", "fetch"}:
        parser.error("--server or NEBULA_REGISTRY_URL is required")
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be > 0")
    return args


def main() -> int:
    args = parse_args()
    try:
        return args.func(args)
    except error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        print(body or exc.reason)
        return 1
    except Exception as exc:  # noqa: BLE001
        print(str(exc))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
