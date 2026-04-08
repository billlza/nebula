from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def _msg(obj: dict) -> bytes:
    body = json.dumps(obj, separators=(",", ":")).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: lsp_smoke.py <nebula-binary> <out-dir>", file=sys.stderr)
        return 2

    binary = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    bad = out_dir / "lsp_bad.nb"
    bad.write_text(
        "fn bad(x: Int) -> Int {\n"
        "  if x + 1 {\n"
        "    return 1\n"
        "  } else {\n"
        "    return 0\n"
        "  }\n"
        "}\n"
    )
    defs = out_dir / "lsp_def.nb"
    defs.write_text(
        "fn helper() -> Int {\n"
        "  return 1\n"
        "}\n\n"
        "fn main() -> Int {\n"
        "  return helper()\n"
        "}\n"
    )

    hover = subprocess.run(
        [str(binary), "lsp"],
        input=_msg(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": "file://" + str(bad.resolve())},
                    "position": {"line": 1, "character": 5},
                },
            }
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=10,
    )
    definition = subprocess.run(
        [str(binary), "lsp"],
        input=_msg(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": "file://" + str(defs.resolve())},
                    "position": {"line": 5, "character": 10},
                },
            }
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=10,
    )

    (out_dir / "lsp-hover.out").write_bytes(hover.stdout)
    (out_dir / "lsp-definition.out").write_bytes(definition.stdout)
    return 0 if hover.returncode == 0 and definition.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
