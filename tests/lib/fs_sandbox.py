from __future__ import annotations

import shutil
import tempfile
from pathlib import Path


class SandboxCreationError(RuntimeError):
    def __init__(self, message: str, *, partial_path: Path | None = None):
        super().__init__(message)
        self.partial_path = partial_path


def make_case_sandbox(case_id: str, tests_root: Path) -> Path:
    safe_id = "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in case_id)
    try:
        base = Path(tempfile.mkdtemp(prefix=f"nebula-{safe_id}-"))
    except OSError as exc:
        raise SandboxCreationError(f"failed to allocate case sandbox: {exc}") from exc

    try:
        fixtures_src = tests_root / "fixtures"
        fixtures_dst = base / "fixtures"
        shutil.copytree(fixtures_src, fixtures_dst, dirs_exist_ok=True)

        (base / "generated_cpp").mkdir(parents=True, exist_ok=True)
        (base / "artifacts").mkdir(parents=True, exist_ok=True)
        (base / "work").mkdir(parents=True, exist_ok=True)
    except BaseException as exc:
        try:
            shutil.rmtree(base)
        except OSError as cleanup_exc:
            if not isinstance(exc, Exception):
                exc.add_note(
                    "partial sandbox cleanup failed after cancellation: "
                    f"{cleanup_exc}; path={base}"
                )
                raise
            raise SandboxCreationError(
                "failed to initialize case sandbox and failed to remove the partial "
                f"sandbox: initialization={exc}; cleanup={cleanup_exc}",
                partial_path=base,
            ) from exc
        if not isinstance(exc, Exception):
            raise
        raise SandboxCreationError(f"failed to initialize case sandbox: {exc}") from exc

    return base


def cleanup_case_sandbox(path: Path) -> None:
    shutil.rmtree(path)
