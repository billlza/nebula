#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sqlite3
import subprocess
from pathlib import Path
from typing import Any


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def run_command(cmd: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, check=False)


def fail(message: str) -> None:
    raise SystemExit(message)


def assert_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label} failed with rc={result.returncode}\nSTDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}")


def assert_warning_free(result: subprocess.CompletedProcess[str], label: str) -> None:
    if "warning:" in result.stdout or "warning:" in result.stderr:
        fail(f"{label} emitted warning output\nSTDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}")


def load_json_lines(output: str, prefix: str) -> list[dict[str, Any]]:
    return [
        json.loads(line[len(prefix):])
        for line in output.splitlines()
        if line.startswith(prefix)
    ]


def receipt_counts(db_path: Path) -> dict[str, int]:
    with sqlite3.connect(db_path) as conn:
        return dict(conn.execute(
            "SELECT receipt_kind, COUNT(*) FROM app_local_runtime_receipts GROUP BY receipt_kind"
        ).fetchall())


def query_all(db_path: Path, sql: str) -> list[tuple[Any, ...]]:
    with sqlite3.connect(db_path) as conn:
        return conn.execute(sql).fetchall()


def validate_bundle(app: Path, stage: Path) -> None:
    bundle_path = app / "deploy" / "bundle" / "manifest.preview.json"
    bundle = json.loads(bundle_path.read_text(encoding="utf-8"))
    if bundle.get("schema") != "nebula.gui.preview.package.v1":
        fail(f"bad bundle schema: {bundle!r}")
    runtime = bundle.get("runtime", {})
    if runtime.get("app_core") != "nebula-first":
        fail(f"bundle lost Nebula-first boundary: {bundle!r}")
    if runtime.get("wire_command_schema") != "thin-host-bridge.command.v1":
        fail(f"bad wire command schema: {bundle!r}")
    if runtime.get("domain_command_schema") != "media-player.command.v1":
        fail(f"bad domain command schema: {bundle!r}")
    sidecars = bundle.get("host_sidecars", {})
    expected_sidecars = {
        "file_picker": "preview-adapter",
        "codec": "native-sidecar-boundary",
        "player": "native-sidecar-boundary",
        "torrent_transport": "stub-progress-events",
    }
    if sidecars != expected_sidecars:
        fail(f"sidecar boundary drifted: {bundle!r}")
    update_rel = bundle.get("update_manifest", {}).get("path")
    update_sha = bundle.get("update_manifest", {}).get("sha256")
    if not isinstance(update_rel, str) or not isinstance(update_sha, str):
        fail(f"bad update manifest entry: {bundle!r}")
    update_path = app / update_rel
    actual_update_sha = hashlib.sha256(update_path.read_bytes()).hexdigest()
    if actual_update_sha != update_sha:
        fail(f"stale update checksum: {actual_update_sha} vs {update_sha}")

    stage_bundle = stage / "bundle"
    shutil.rmtree(stage_bundle, ignore_errors=True)
    stage_bundle.mkdir(parents=True, exist_ok=True)
    shutil.copy2(bundle_path, stage_bundle / "manifest.preview.json")
    for asset in bundle.get("assets", []):
        rel = asset.get("path")
        if not isinstance(rel, str):
            fail(f"bad bundle asset entry: {asset!r}")
        source = app / rel
        if not source.exists():
            fail(f"missing bundle asset: {rel}")
        destination = stage_bundle / rel
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    staged_update = stage_bundle / update_rel
    if hashlib.sha256(staged_update.read_bytes()).hexdigest() != update_sha:
        fail("staged update manifest checksum drifted")


def build_app(binary: Path, app: Path, entry: Path) -> None:
    update = run_command([str(binary), "update", str(app)], cwd=repo_root())
    assert_ok(update, "media-player update")
    check = run_command([str(binary), "check", str(app)], cwd=repo_root())
    assert_ok(check, "media-player check")
    assert_warning_free(check, "media-player check")
    build = run_command([str(binary), "build", str(app), "--out", str(entry)], cwd=repo_root())
    assert_ok(build, "media-player build")
    assert_warning_free(build, "media-player build")
    if not entry.exists():
        fail(f"entry binary missing after build: {entry}")


def run_app(
    entry: Path,
    mode: str,
    receipt_db: Path,
    *,
    preserve_fixed_work: bool = False,
    preserve_receipts: bool = False,
) -> str:
    fixed_work = repo_root() / "work" / "thin_host_media_player"
    if not preserve_fixed_work:
        shutil.rmtree(fixed_work, ignore_errors=True)
    receipt_db.parent.mkdir(parents=True, exist_ok=True)
    if receipt_db.exists() and not preserve_receipts:
        receipt_db.unlink()
    env = os.environ.copy()
    env["APP_SQLITE_PATH"] = str(receipt_db)
    env["APP_LOCAL_APP_ID"] = "thin-host-media-player"
    if mode:
        env["NEBULA_MEDIA_PLAYER_COMMAND_MODE"] = mode
    else:
        env.pop("NEBULA_MEDIA_PLAYER_COMMAND_MODE", None)
    result = run_command([str(entry)], cwd=repo_root(), env=env)
    assert_ok(result, f"media-player launch mode={mode or 'default'}")
    output = result.stdout
    if "thin-host-media-player-error" in output or "app-local-error:" in output:
        fail(f"media-player reported runtime error in mode={mode or 'default'}:\n{output}")
    if "thin-host-media-player-boundary=nebula-first-app-core" not in output:
        fail(f"missing Nebula-first host boundary marker in mode={mode or 'default'}")
    if "thin-host-media-player-sidecar-boundary=file-picker/codec/player/torrent-adapter" not in output:
        fail(f"missing host sidecar boundary marker in mode={mode or 'default'}")
    if "thin-host-media-player-end renders=" not in output:
        fail(f"missing clean host end marker in mode={mode or 'default'}")
    return output


def validate_default_mode(output: str, receipt_db: Path) -> None:
    events = load_json_lines(output, "media-player-event:")
    snapshots = load_json_lines(output, "media-player-snapshot:")
    lifecycle = load_json_lines(output, "app-local-lifecycle:")
    jobs = load_json_lines(output, "media-player-jobs:")
    expected = [
        "media_player_booted",
        "library_file_imported",
        "playback_audio_quality_set",
        "command_rejected",
        "command_rejected",
        "download_import_queued",
        "command_rejected",
        "playback_video_quality_set",
        "playback_bitrate_policy_set",
        "download_paused",
        "download_resumed",
        "download_cancelled",
        "media_player_quit",
    ]
    if [item["kind"] for item in events] != expected:
        fail(f"default mode event sequence drifted: {events!r}")
    if [item["state_revision"] for item in events] != [0, 1, 2, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9]:
        fail(f"default mode revision sequence drifted: {events!r}")
    if [item["status"] for item in lifecycle] != ["startup_started", "app_ready", "shutdown_clean"]:
        fail(f"default mode lifecycle sequence drifted: {lifecycle!r}")
    if not jobs or jobs[0].get("final_run_status") != "failed" or jobs[0].get("outbox_status") != "dead_letter":
        fail(f"default mode jobs/outbox evidence missing: {jobs!r}")
    final = snapshots[-1]
    if final["playback"]["audio_quality"] != "lossless" or final["playback"]["video_quality"] != "4k":
        fail(f"default playback settings missing from snapshot: {final!r}")
    expected_counts = {
        "command_context": 12,
        "event": 12,
        "host_snapshot_readiness": 13,
        "lifecycle_marker": 3,
        "recovery_marker": 1,
        "snapshot": 13,
        "update_marker": 1,
    }
    if receipt_counts(receipt_db) != expected_counts:
        fail(f"default receipt counts drifted: {receipt_counts(receipt_db)!r}")
    media_db = repo_root() / "work" / "thin_host_media_player" / "media.db"
    settings = dict(query_all(media_db, "SELECT setting_key, setting_value FROM media_settings"))
    if settings != {"audio_quality": "lossless", "video_quality": "4k", "bitrate_policy": "adaptive-high"}:
        fail(f"default media settings did not persist: {settings!r}")
    downloads = query_all(media_db, "SELECT progress_percent, status FROM media_download_tasks")
    if downloads != [(0, "cancelled")]:
        fail(f"default download task did not persist cancel state: {downloads!r}")


def validate_phase1_mode(output: str, receipt_db: Path) -> None:
    events = load_json_lines(output, "media-player-event:")
    snapshots = load_json_lines(output, "media-player-snapshot:")
    diagnostics = load_json_lines(output, "media-player-recovery-diagnostics:")
    expected = [
        "media_player_booted",
        "library_file_imported",
        "library_item_selected",
        "playback_sidecar_open_requested",
        "playback_progress_recorded",
        "download_import_queued",
        "download_progress_recorded",
        "download_progress_recorded",
        "media_player_quit",
    ]
    if [item["kind"] for item in events] != expected:
        fail(f"phase1 event sequence drifted: {events!r}")
    if [item["state_revision"] for item in events] != list(range(0, 9)):
        fail(f"phase1 revision sequence drifted: {events!r}")
    if len(diagnostics) != 1 or diagnostics[0].get("recommendation") != "clean_start":
        fail(f"phase1 recovery diagnostics missing: {diagnostics!r}")
    if diagnostics[0]["sidecars"]["player_adapter"] != "native-sidecar-boundary":
        fail(f"phase1 sidecar manifest missing player boundary: {diagnostics!r}")
    final = snapshots[-1]
    if final["selection"]["media_id"] != "media:sample-public-domain":
        fail(f"phase1 selection missing from snapshot: {final!r}")
    if final["playback"]["status"] != "playing" or final["playback"]["position_ms"] != 42000:
        fail(f"phase1 playback progress missing from snapshot: {final!r}")
    if final["downloads"]["completed"] != 1 or final["downloads"]["progress_percent"] != 100:
        fail(f"phase1 download completion missing from snapshot: {final!r}")
    counts = receipt_counts(receipt_db)
    if counts.get("command_context") != 8 or counts.get("event") != 8:
        fail(f"phase1 command/event receipts missing: {counts!r}")
    if counts.get("snapshot") != 9 or counts.get("host_snapshot_readiness") != 9:
        fail(f"phase1 snapshot readiness receipts missing: {counts!r}")
    media_db = repo_root() / "work" / "thin_host_media_player" / "media.db"
    library = query_all(media_db, "SELECT media_id, title, source_kind FROM media_library_items")
    if library != [("media:sample-public-domain", "Sample Public Domain Clip", "file")]:
        fail(f"phase1 media library did not persist: {library!r}")
    downloads = query_all(media_db, "SELECT progress_percent, status FROM media_download_tasks")
    if downloads != [(100, "download_complete")]:
        fail(f"phase1 download completion did not persist: {downloads!r}")


def validate_rehydration_mode(output: str, receipt_db: Path) -> None:
    events = load_json_lines(output, "media-player-event:")
    snapshots = load_json_lines(output, "media-player-snapshot:")
    diagnostics = load_json_lines(output, "media-player-recovery-diagnostics:")
    if [item["kind"] for item in events] != ["media_player_booted", "media_player_quit"]:
        fail(f"rehydration event sequence drifted: {events!r}")
    if [item["state_revision"] for item in events] != [8, 9]:
        fail(f"rehydration did not resume from previous revision: {events!r}")
    if not diagnostics or diagnostics[0].get("latest_revision") != 8:
        fail(f"rehydration diagnostics did not explain previous revision: {diagnostics!r}")
    if diagnostics[0].get("recommendation") == "clean_start":
        fail(f"rehydration diagnostics falsely reported a clean start: {diagnostics!r}")
    boot_snapshot = snapshots[0]
    if boot_snapshot["selection"]["media_id"] != "media:sample-public-domain":
        fail(f"rehydration boot snapshot lost media selection: {boot_snapshot!r}")
    if boot_snapshot["playback"]["status"] != "playing" or boot_snapshot["playback"]["position_ms"] != 42000:
        fail(f"rehydration boot snapshot lost playback progress: {boot_snapshot!r}")
    if boot_snapshot["downloads"]["completed"] != 1 or boot_snapshot["downloads"]["progress_percent"] != 100:
        fail(f"rehydration boot snapshot lost download progress: {boot_snapshot!r}")
    if receipt_counts(receipt_db).get("lifecycle_marker", 0) < 6:
        fail(f"rehydration did not append second lifecycle session: {receipt_counts(receipt_db)!r}")
    media_db = repo_root() / "work" / "thin_host_media_player" / "media.db"
    runtime_state = query_all(
        media_db,
        "SELECT library_count, active_downloads, completed_downloads, selected_media_id, playback_status, playback_position_ms, download_progress_percent, state_revision FROM media_runtime_state",
    )
    expected = [(1, 0, 1, "media:sample-public-domain", "playing", 42000, 100, 9)]
    if runtime_state != expected:
        fail(f"rehydrated runtime state did not persist final quit revision: {runtime_state!r}")


def validate_rejection_mode(output: str, receipt_db: Path) -> None:
    events = load_json_lines(output, "media-player-event:")
    rejections = [event for event in events if event["kind"] == "command_rejected"]
    if [event["message"] for event in rejections] != [
        "unsupported thin-host command schema",
        "unknown media-player command kind",
        "media file path is required",
    ]:
        fail(f"basic rejection messages drifted: {rejections!r}")
    if [event["state_revision"] for event in rejections] != [0, 0, 0]:
        fail(f"basic rejections mutated state revision: {rejections!r}")
    if receipt_counts(receipt_db).get("event") != 4:
        fail(f"basic rejection event receipts missing: {receipt_counts(receipt_db)!r}")


def validate_boundary_rejection_mode(output: str, receipt_db: Path) -> None:
    events = load_json_lines(output, "media-player-event:")
    rejections = [event for event in events if event["kind"] == "command_rejected"]
    if [event["message"] for event in rejections] != [
        "media item is not in Nebula media library",
        "no active download to cancel",
        "no active download for progress update",
    ]:
        fail(f"boundary rejection messages drifted: {rejections!r}")
    if [event["state_revision"] for event in rejections] != [1, 1, 1]:
        fail(f"boundary rejections mutated imported state: {rejections!r}")
    if events[-1]["kind"] != "media_player_quit" or events[-1]["state_revision"] != 2:
        fail(f"boundary quit did not advance from imported state: {events!r}")
    media_db = repo_root() / "work" / "thin_host_media_player" / "media.db"
    library = query_all(media_db, "SELECT media_id, title FROM media_library_items")
    if library != [("media:sample-public-domain", "Sample Public Domain Clip")]:
        fail(f"forged host selection mutated media library: {library!r}")
    if receipt_counts(receipt_db).get("event") != 5:
        fail(f"boundary rejection event receipts missing: {receipt_counts(receipt_db)!r}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify the thin-host media player as a staged, launchable preview app")
    parser.add_argument("--binary", default=str(repo_root() / "build" / "nebula"))
    parser.add_argument("--out-root", default=str(repo_root() / "work" / "thin_host_media_player_feasibility"))
    args = parser.parse_args()

    root = repo_root()
    binary = Path(args.binary).resolve()
    app = root / "examples" / "thin_host_media_player"
    out_root = Path(args.out_root).resolve()
    shutil.rmtree(out_root, ignore_errors=True)
    out_root.mkdir(parents=True, exist_ok=True)
    validate_bundle(app, out_root)
    entry = out_root / "bundle" / "bin" / "thin-host-media-player"
    entry.parent.mkdir(parents=True, exist_ok=True)
    build_app(binary, app, entry)

    default_output = run_app(entry, "", out_root / "state" / "default-receipts.db")
    validate_default_mode(default_output, out_root / "state" / "default-receipts.db")

    phase1_output = run_app(entry, "phase1", out_root / "state" / "phase1-receipts.db")
    validate_phase1_mode(phase1_output, out_root / "state" / "phase1-receipts.db")

    rehydration_output = run_app(
        entry,
        "rehydration_probe",
        out_root / "state" / "phase1-receipts.db",
        preserve_fixed_work=True,
        preserve_receipts=True,
    )
    validate_rehydration_mode(rehydration_output, out_root / "state" / "phase1-receipts.db")

    rejection_output = run_app(entry, "rejections", out_root / "state" / "rejections-receipts.db")
    validate_rejection_mode(rejection_output, out_root / "state" / "rejections-receipts.db")

    boundary_output = run_app(entry, "boundary_rejections", out_root / "state" / "boundary-receipts.db")
    validate_boundary_rejection_mode(boundary_output, out_root / "state" / "boundary-receipts.db")

    stale_jobs_output = run_app(
        entry,
        "phase1",
        out_root / "state" / "stale-jobs-receipts.db",
        preserve_fixed_work=True,
    )
    validate_phase1_mode(stale_jobs_output, out_root / "state" / "stale-jobs-receipts.db")

    print(json.dumps({
        "schema": "media-player.feasibility.v1",
        "status": "ok",
        "entry": str(entry),
        "modes": ["default", "phase1", "rehydration_probe", "rejections", "boundary_rejections"],
        "bundle": str(out_root / "bundle" / "manifest.preview.json"),
    }, sort_keys=True))
    print("thin-host-media-player-feasibility-ok")


if __name__ == "__main__":
    main()
