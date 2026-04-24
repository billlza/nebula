#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import json
import shutil
import tarfile
import tempfile
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

try:
    import tomllib
except ModuleNotFoundError as exc:
    raise SystemExit("nebula hosted registry server requires Python 3.11+ (or set PYTHON to a compatible interpreter)") from exc


def read_manifest(path: Path) -> dict[str, Any]:
    return tomllib.loads(path.read_text(encoding="utf-8"))


def json_response(handler: BaseHTTPRequestHandler, status: int, payload: dict[str, Any]) -> None:
    body = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


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


def validate_archive_members(archive: tarfile.TarFile) -> str | None:
    for member in archive.getmembers():
        member_path = Path(member.name)
        if member_path.is_absolute() or ".." in member_path.parts:
            return "archive contains unsafe path"
        if member.isfile() or member.isdir():
            continue
        return "archive contains unsupported entry type"
    return None


class RegistryServer(ThreadingHTTPServer):
    def __init__(self, server_address: tuple[str, int], handler_cls: type[BaseHTTPRequestHandler], *,
                 registry_root: Path, token: str | None, port_file: Path | None):
        super().__init__(server_address, handler_cls)
        self.registry_root = registry_root
        self.token = token
        self.port_file = port_file
        self._write_port_file()

    def _write_port_file(self) -> None:
        if self.port_file is None:
            return
        self.port_file.parent.mkdir(parents=True, exist_ok=True)
        self.port_file.write_text(str(self.server_port), encoding="utf-8")


class RegistryHandler(BaseHTTPRequestHandler):
    server: RegistryServer

    def do_GET(self) -> None:
        if not self._authorize():
            return
        parsed = urlparse(self.path)
        if parsed.path == "/healthz":
            json_response(self, HTTPStatus.OK, {"status": "ok"})
            return
        if parsed.path == "/v1/packages":
            self._handle_list(parse_qs(parsed.query))
            return
        parts = [part for part in parsed.path.split("/") if part]
        if len(parts) == 3 and parts[:2] == ["v1", "packages"]:
            self._handle_versions(parts[2])
            return
        if len(parts) == 5 and parts[:2] == ["v1", "packages"] and parts[4] == "archive":
            self._handle_archive(parts[2], parts[3])
            return
        json_response(self, HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_PUT(self) -> None:
        if not self._authorize():
            return
        parsed = urlparse(self.path)
        parts = [part for part in parsed.path.split("/") if part]
        if len(parts) == 5 and parts[:2] == ["v1", "packages"] and parts[4] == "archive":
            self._handle_publish(parts[2], parts[3], parse_qs(parsed.query))
            return
        json_response(self, HTTPStatus.NOT_FOUND, {"error": "not found"})

    def log_message(self, format: str, *args: object) -> None:
        return

    def _authorize(self) -> bool:
        if self.path.startswith("/healthz"):
            return True
        if not self.server.token:
            return True
        auth = self.headers.get("Authorization", "")
        if auth == f"Bearer {self.server.token}":
            return True
        json_response(self, HTTPStatus.UNAUTHORIZED, {"error": "unauthorized"})
        return False

    def _package_root(self, name: str, version: str) -> Path:
        return self.server.registry_root / name / version

    def _handle_list(self, query: dict[str, list[str]]) -> None:
        needle = (query.get("q", [""])[0]).strip().lower()
        packages: list[dict[str, Any]] = []
        for package_dir in sorted(path for path in self.server.registry_root.iterdir() if path.is_dir()):
            if package_dir.name.startswith("."):
                continue
            if needle and needle not in package_dir.name.lower():
                continue
            versions = sorted(
                version_dir.name
                for version_dir in package_dir.iterdir()
                if version_dir.is_dir() and (version_dir / "nebula.toml").exists()
            )
            packages.append({"name": package_dir.name, "versions": versions})
        json_response(self, HTTPStatus.OK, {"packages": packages})

    def _handle_versions(self, name: str) -> None:
        package_dir = self.server.registry_root / name
        if not package_dir.exists():
            json_response(self, HTTPStatus.NOT_FOUND, {"error": f"package not found: {name}"})
            return
        versions = sorted(
            version_dir.name
            for version_dir in package_dir.iterdir()
            if version_dir.is_dir() and (version_dir / "nebula.toml").exists()
        )
        json_response(self, HTTPStatus.OK, {"name": name, "versions": versions})

    def _handle_archive(self, name: str, version: str) -> None:
        package_root = self._package_root(name, version)
        if not (package_root / "nebula.toml").exists():
            json_response(self, HTTPStatus.NOT_FOUND, {"error": f"package not found: {name}@{version}"})
            return

        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
            archive.add(package_root, arcname=".")
        body = buffer.getvalue()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/gzip")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _handle_publish(self, name: str, version: str, query: dict[str, list[str]]) -> None:
        length_header = self.headers.get("Content-Length")
        if length_header is None:
            json_response(self, HTTPStatus.LENGTH_REQUIRED, {"error": "missing content-length"})
            return

        body = self.rfile.read(int(length_header))
        target_root = self._package_root(name, version)
        force = query.get("force", ["0"])[0] == "1"

        with tempfile.TemporaryDirectory(prefix="nebula-registry-upload-") as tmp_dir:
            extract_root = Path(tmp_dir) / "pkg"
            extract_root.mkdir(parents=True, exist_ok=True)
            with tarfile.open(fileobj=io.BytesIO(body), mode="r:gz") as archive:
                validation_error = validate_archive_members(archive)
                if validation_error is not None:
                    json_response(self, HTTPStatus.BAD_REQUEST, {"error": validation_error})
                    return
                archive.extractall(extract_root)

            manifest_path = extract_root / "nebula.toml"
            metadata_path = extract_root / "nebula-package.toml"
            if not manifest_path.exists() or not metadata_path.exists():
                json_response(self, HTTPStatus.BAD_REQUEST, {"error": "archive is missing registry metadata"})
                return

            manifest = read_manifest(manifest_path)
            package = manifest.get("package", {})
            if package.get("name") != name or package.get("version") != version:
                json_response(self, HTTPStatus.BAD_REQUEST, {"error": "archive package identity does not match URL"})
                return

            target_root.parent.mkdir(parents=True, exist_ok=True)
            if target_root.exists():
                if compare_trees(extract_root, target_root):
                    json_response(self, HTTPStatus.OK, {"status": "unchanged", "package": name, "version": version})
                    return
                if not force:
                    json_response(self, HTTPStatus.CONFLICT, {"error": "package version is immutable"})
                    return
                shutil.rmtree(target_root)
            shutil.move(str(extract_root), str(target_root))

        json_response(
            self,
            HTTPStatus.CREATED,
            {"status": "published" if not force else "replaced", "package": name, "version": version},
        )


def serve(args: argparse.Namespace) -> int:
    registry_root = Path(args.root).resolve()
    registry_root.mkdir(parents=True, exist_ok=True)
    port_file = Path(args.port_file).resolve() if args.port_file else None
    server = RegistryServer((args.host, args.port), RegistryHandler, registry_root=registry_root,
                            token=args.token, port_file=port_file)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        return 0
    finally:
        server.server_close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Nebula hosted registry service")
    subparsers = parser.add_subparsers(dest="command", required=True)

    serve_parser = subparsers.add_parser("serve", help="Serve a hosted registry over HTTP")
    serve_parser.add_argument("--root", required=True, help="Registry root directory")
    serve_parser.add_argument("--host", default="127.0.0.1")
    serve_parser.add_argument("--port", type=int, default=8080)
    serve_parser.add_argument("--port-file", help="Write the chosen port to this file")
    serve_parser.add_argument("--token", help="Bearer token required for API access")
    serve_parser.set_defaults(func=serve)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
