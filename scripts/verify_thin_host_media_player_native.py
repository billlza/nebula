#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

from verify_thin_host_media_player_app import (
    assert_ok,
    assert_warning_free,
    build_app,
    fail,
    load_json_lines,
    query_all,
    repo_root,
    run_app,
    run_command,
    validate_bundle,
)


def candidate_pkg_config_paths() -> list[Path]:
    paths: list[Path] = []
    existing = os.environ.get("PKG_CONFIG_PATH", "")
    for item in existing.split(os.pathsep):
        if item:
            paths.append(Path(item))
    for prefix in (
        Path("/opt/homebrew/opt/mpv"),
        Path("/opt/homebrew/opt/libtorrent-rasterbar"),
        Path("/usr/local/opt/mpv"),
        Path("/usr/local/opt/libtorrent-rasterbar"),
        Path("/usr"),
        Path("/usr/local"),
    ):
        paths.append(prefix / "lib" / "pkgconfig")
        paths.append(prefix / "share" / "pkgconfig")
    return [path for path in paths if path.exists()]


def pkg_env() -> dict[str, str]:
    env = os.environ.copy()
    paths = candidate_pkg_config_paths()
    if paths:
        env["PKG_CONFIG_PATH"] = os.pathsep.join(str(path) for path in paths)
    return env


def pkg_config(args: list[str], env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["pkg-config", *args], cwd=repo_root(), env=env, capture_output=True, text=True, check=False)


def dependency_diagnostics(env: dict[str, str]) -> dict[str, Any]:
    mpv = pkg_config(["--modversion", "mpv"], env)
    torrent = pkg_config(["--modversion", "libtorrent-rasterbar"], env)
    return {
        "schema": "media-player.native-dependency-diagnostics.v1",
        "mpv": {
            "pkg": "mpv",
            "available": mpv.returncode == 0,
            "version": mpv.stdout.strip(),
            "diagnostic": mpv.stderr.strip(),
        },
        "torrent": {
            "pkg": "libtorrent-rasterbar",
            "available": torrent.returncode == 0,
            "version": torrent.stdout.strip(),
            "diagnostic": torrent.stderr.strip(),
        },
        "pkg_config_path": env.get("PKG_CONFIG_PATH", ""),
    }


def require_native_deps(env: dict[str, str]) -> dict[str, Any]:
    diag = dependency_diagnostics(env)
    missing = [
        item["pkg"]
        for item in (diag["mpv"], diag["torrent"])
        if not item["available"]
    ]
    if missing:
        fail(
            "native media dependencies are missing: "
            + ", ".join(missing)
            + "\nInstall libmpv and libtorrent-rasterbar development files, then rerun this opt-in gate.\n"
            + json.dumps(diag, indent=2, sort_keys=True)
        )
    return diag


def pkg_flags(env: dict[str, str]) -> list[str]:
    result = pkg_config(["--cflags", "--libs", "mpv", "libtorrent-rasterbar"], env)
    assert_ok(result, "native media pkg-config flags")
    return result.stdout.split()


def compile_native_probe(out_root: Path, env: dict[str, str]) -> Path:
    source = repo_root() / "examples" / "thin_host_media_player" / "native" / "media_sidecar_probe.cpp"
    probe = out_root / "native" / "media-sidecar-probe"
    probe.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "clang++",
        "-std=c++20",
        "-O2",
        "-DNDEBUG",
        "-Wall",
        "-Wextra",
        "-Werror",
        str(source),
        "-o",
        str(probe),
        *pkg_flags(env),
    ]
    result = subprocess.run(cmd, cwd=repo_root(), env=env, capture_output=True, text=True, check=False)
    assert_ok(result, "native media sidecar probe compile")
    assert_warning_free(result, "native media sidecar probe compile")
    return probe


def run_native_probe(probe: Path, env: dict[str, str]) -> dict[str, Any]:
    result = subprocess.run([str(probe)], cwd=repo_root(), env=env, capture_output=True, text=True, check=False)
    assert_ok(result, "native media sidecar probe")
    assert_warning_free(result, "native media sidecar probe")
    proof = json.loads(result.stdout)
    if proof.get("schema") != "media-player.native-sidecar-proof.v1" or proof.get("status") != "ok":
        fail(f"bad native media sidecar proof: {proof!r}")
    return proof


def write_fixture(out_root: Path) -> tuple[str, str]:
    fixture = out_root / "fixtures" / "public-domain-sample.media"
    fixture.parent.mkdir(parents=True, exist_ok=True)
    fixture.write_bytes(b"Nebula thin-host media-player public-domain fixture\n")
    digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    return fixture.resolve().as_uri(), "sha256-" + digest


def validate_native_app_output(output: str, receipt_db: Path) -> None:
    events = load_json_lines(output, "media-player-event:")
    snapshots = load_json_lines(output, "media-player-snapshot:")
    diagnostics = load_json_lines(output, "media-player-recovery-diagnostics:")
    expected = [
        "media_player_booted",
        "library_file_imported",
        "library_item_selected",
        "playback_sidecar_open_requested",
        "playback_progress_recorded",
        "playback_paused",
        "playback_resumed",
        "playback_seeked",
        "download_import_queued",
        "download_progress_recorded",
        "download_progress_recorded",
        "media_player_quit",
    ]
    if [event["kind"] for event in events] != expected:
        fail(f"native media event sequence drifted: {events!r}")
    if [event["state_revision"] for event in events] != list(range(0, 12)):
        fail(f"native media revision sequence drifted: {events!r}")
    if not diagnostics:
        fail("native media recovery diagnostics missing")
    sidecars = diagnostics[0].get("sidecars", {})
    if sidecars.get("player_adapter") != "libmpv-c-api":
        fail(f"native media did not expose libmpv sidecar: {diagnostics!r}")
    if sidecars.get("torrent_adapter") != "libtorrent-rasterbar-loopback":
        fail(f"native media did not expose libtorrent sidecar: {diagnostics!r}")
    final = snapshots[-1]
    if final["playback"]["status"] != "playing" or final["playback"]["position_ms"] != 60000:
        fail(f"native playback state missing from final snapshot: {final!r}")
    if final["downloads"]["completed"] != 1 or final["downloads"]["progress_percent"] != 100:
        fail(f"native torrent completion missing from final snapshot: {final!r}")

    media_db = repo_root() / "work" / "thin_host_media_player" / "media.db"
    playback_rows = query_all(
        media_db,
        "SELECT event_kind, position_ms, status FROM media_playback_events ORDER BY state_revision",
    )
    expected_playback = [
        ("playback_sidecar_open_requested", 0, "sidecar_open_requested"),
        ("playback_progress_recorded", 12000, "playing"),
        ("playback_paused", 12000, "paused"),
        ("playback_resumed", 12000, "playing"),
        ("playback_seeked", 60000, "playing"),
    ]
    if playback_rows != expected_playback:
        fail(f"native playback event persistence drifted: {playback_rows!r}")
    download_rows = query_all(media_db, "SELECT progress_percent, status FROM media_download_tasks")
    if download_rows != [(100, "download_complete")]:
        fail(f"native download state did not persist completion: {download_rows!r}")
    if not receipt_db.exists():
        fail("native receipt DB missing")


def run_native_app(binary: Path, out_root: Path, proof: dict[str, Any]) -> None:
    app = repo_root() / "examples" / "thin_host_media_player"
    validate_bundle(app, out_root)
    entry = out_root / "bundle" / "bin" / "thin-host-media-player"
    entry.parent.mkdir(parents=True, exist_ok=True)
    build_app(binary, app, entry)

    file_uri, file_sha = write_fixture(out_root)
    env = {
        "NEBULA_MEDIA_PLAYER_NATIVE_SIDECARS": "1",
        "NEBULA_MEDIA_PLAYER_FILE_URI": file_uri,
        "NEBULA_MEDIA_PLAYER_FILE_SHA256": file_sha,
        "NEBULA_MEDIA_PLAYER_TORRENT_URI": "magnet:?xt=urn:btih:NATIVEPUBLICDOMAINFIXTURE",
        "NEBULA_MEDIA_PLAYER_TORRENT_SHA256": "sha256-native-public-domain-torrent",
        "NEBULA_MEDIA_PLAYER_NATIVE_PROOF": json.dumps(proof, sort_keys=True),
    }
    receipt_db = out_root / "state" / "native-receipts.db"
    output = run_app(entry, "native_media", receipt_db, extra_env=env)
    validate_native_app_output(output, receipt_db)


def main() -> None:
    parser = argparse.ArgumentParser(description="Opt-in native libmpv/libtorrent media-player verification")
    parser.add_argument("--binary", default=str(repo_root() / "build" / "nebula"))
    parser.add_argument("--out-root", default=str(repo_root() / "work" / "thin_host_media_player_native"))
    parser.add_argument("--diagnose-deps", action="store_true")
    args = parser.parse_args()

    env = pkg_env()
    if args.diagnose_deps:
        print(json.dumps(dependency_diagnostics(env), indent=2, sort_keys=True))
        return

    out_root = Path(args.out_root).resolve()
    shutil.rmtree(out_root, ignore_errors=True)
    out_root.mkdir(parents=True, exist_ok=True)
    diag = require_native_deps(env)
    probe = compile_native_probe(out_root, env)
    proof = run_native_probe(probe, env)
    run_native_app(Path(args.binary).resolve(), out_root, proof)
    print(json.dumps({
        "schema": "media-player.native-gate.v1",
        "status": "ok",
        "dependencies": diag,
        "proof": proof,
    }, sort_keys=True))
    print("thin-host-media-player-native-ok")


if __name__ == "__main__":
    main()
