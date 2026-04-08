from __future__ import annotations

import shutil
import tempfile
from pathlib import Path


def make_case_sandbox(case_id: str, tests_root: Path) -> Path:
    safe_id = "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in case_id)
    base = Path(tempfile.mkdtemp(prefix=f"nebula-{safe_id}-"))

    fixtures_src = tests_root / "fixtures"
    fixtures_dst = base / "fixtures"
    shutil.copytree(fixtures_src, fixtures_dst, dirs_exist_ok=True)

    (base / "generated_cpp").mkdir(parents=True, exist_ok=True)
    (base / "artifacts").mkdir(parents=True, exist_ok=True)
    (base / "work").mkdir(parents=True, exist_ok=True)

    return base


def cleanup_case_sandbox(path: Path) -> None:
    shutil.rmtree(path, ignore_errors=True)
