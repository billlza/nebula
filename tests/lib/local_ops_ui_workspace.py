from __future__ import annotations

import json
import shutil
import tomllib
from pathlib import Path


_UI_DEPENDENCY_LINE = 'ui = { path = "../../official/nebula-ui" }'


def copy_local_ops_console_ui(repo_root: Path, destination: Path) -> Path:
    source = repo_root / "examples" / "local_ops_console_ui"
    if destination.is_symlink():
        destination.unlink()
    elif destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source, destination)

    manifest_path = destination / "nebula.toml"
    manifest_text = manifest_path.read_text(encoding="utf-8")
    occurrence_count = manifest_text.count(_UI_DEPENDENCY_LINE)
    if occurrence_count != 1:
        raise RuntimeError(
            "Local Ops UI fixture must contain exactly one canonical ui dependency; "
            f"found {occurrence_count}"
        )

    ui_root = (repo_root / "official" / "nebula-ui").resolve(strict=True)
    replacement = f"ui = {{ path = {json.dumps(str(ui_root))} }}"
    manifest_text = manifest_text.replace(_UI_DEPENDENCY_LINE, replacement, 1)
    if _UI_DEPENDENCY_LINE in manifest_text or manifest_text.count(replacement) != 1:
        raise RuntimeError("Local Ops UI fixture dependency rewrite was not exact")
    manifest_path.write_text(manifest_text, encoding="utf-8")

    parsed = tomllib.loads(manifest_text)
    configured_path = parsed.get("dependencies", {}).get("ui", {}).get("path")
    if configured_path != str(ui_root):
        raise RuntimeError("Local Ops UI fixture dependency rewrite failed validation")
    return destination
