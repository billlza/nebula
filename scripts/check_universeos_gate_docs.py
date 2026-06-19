#!/usr/bin/env python3
"""Validate the machine-checkable UniverseOS gate registry."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path, PurePosixPath
from typing import Any


REQUIRED_FAMILIES = {
    "UOS-DOC": r"^UOS-DOC-\d{3}$",
    "UOS-CLI": r"^UOS-CLI-\d{3}$",
    "UOS-ABI": r"^UOS-ABI-\d{3}$",
    "UOS-CORE": r"^UOS-CORE-\d{3}$",
    "UOS-BE": r"^UOS-BE-\d{3}$",
    "UOS-BOOT": r"^UOS-BOOT-\d{3}$",
}
ALLOWED_STATUSES = {"planned", "experimental", "candidate", "accepted"}
REQUIRED_SOURCE_DOCS = {
    "docs/system_profile.md",
    "docs/support_matrix.md",
    "docs/universeos_convergence.md",
    "spec/abi_layout.md",
    "spec/library_layers.md",
}
ALLOWED_SOURCE_DOC_ROOTS = ("docs", "spec", "rfcs")
ALLOWED_SOURCE_DOC_SUFFIXES = {".md", ".ebnf"}
NON_CLAIM_TERMS = (
    "does not",
    "do not",
    "not prove",
    "not claim",
    "unsupported",
)


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[1]


def extract_json_block(text: str, registry_path: Path) -> dict[str, Any]:
    blocks = re.findall(r"```json\s*\n(.*?)\n```", text, flags=re.DOTALL)
    if not blocks:
        raise ValueError(f"{registry_path} has no fenced json registry block")
    for block in blocks:
        try:
            payload = json.loads(block)
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict) and "gate_registry_version" in payload and "gates" in payload:
            return payload
    raise ValueError(f"{registry_path} has no valid UniverseOS gate registry json block")


def is_nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def normalize_title(value: str) -> str:
    return re.sub(r"\s+", " ", value.strip().lower())


def is_safe_source_doc_path(value: str) -> bool:
    if "\\" in value:
        return False
    path = PurePosixPath(value)
    if path.is_absolute():
        return False
    if not path.parts or path.parts[0] not in ALLOWED_SOURCE_DOC_ROOTS:
        return False
    if any(part in {"", ".", ".."} for part in path.parts):
        return False
    return path.suffix in ALLOWED_SOURCE_DOC_SUFFIXES


def validate_registry(payload: dict[str, Any], repo_root: Path, registry_path: Path) -> list[str]:
    errors: list[str] = []

    registry_version = payload.get("gate_registry_version")
    if type(registry_version) is not int or registry_version != 1:
        errors.append("gate_registry_version must be integer 1")

    families = payload.get("gate_naming")
    if not isinstance(families, dict):
        errors.append("gate_naming must be an object")
    else:
        for family in REQUIRED_FAMILIES:
            if not is_nonempty_string(families.get(family)):
                errors.append(f"gate_naming missing {family}")

    gates = payload.get("gates")
    if not isinstance(gates, list) or not gates:
        errors.append("gates must be a non-empty list")
        return errors

    seen: set[str] = set()
    gate_ids: set[str] = set()
    gate_metadata: dict[str, tuple[str, str]] = {}
    for index, gate in enumerate(gates):
        label = f"gates[{index}]"
        if not isinstance(gate, dict):
            errors.append(f"{label} must be an object")
            continue

        gate_id = gate.get("id")
        if not is_nonempty_string(gate_id):
            errors.append(f"{label}.id is required")
            continue
        if gate_id in seen:
            errors.append(f"{gate_id}: duplicate id")
        seen.add(gate_id)
        gate_ids.add(gate_id)

        family = "-".join(gate_id.split("-")[:2])
        pattern = REQUIRED_FAMILIES.get(family)
        if pattern is None or re.match(pattern, gate_id) is None:
            errors.append(f"{gate_id}: id does not match a registered UniverseOS gate family")

        status = gate.get("status")
        if status not in ALLOWED_STATUSES:
            errors.append(f"{gate_id}: status must be one of {sorted(ALLOWED_STATUSES)}")

        title = gate.get("title")
        if not is_nonempty_string(title):
            errors.append(f"{gate_id}: title is required")
        elif isinstance(status, str):
            gate_metadata[gate_id] = (title, status)

        if not is_nonempty_string(gate.get("owner_area")):
            errors.append(f"{gate_id}: owner_area is required")

        required_evidence = gate.get("required_evidence")
        if not isinstance(required_evidence, list) or not required_evidence:
            errors.append(f"{gate_id}: required_evidence must be a non-empty list")
        elif not all(is_nonempty_string(item) for item in required_evidence):
            errors.append(f"{gate_id}: required_evidence entries must be non-empty strings")

        non_claim = gate.get("non_claim")
        if not is_nonempty_string(non_claim):
            errors.append(f"{gate_id}: non_claim is required")
        else:
            lowered = non_claim.lower()
            if not any(term in lowered for term in NON_CLAIM_TERMS):
                errors.append(f"{gate_id}: non_claim must contain explicit non-claim wording")

    source_doc_mapping = payload.get("source_doc_mapping")
    if not isinstance(source_doc_mapping, list) or not source_doc_mapping:
        errors.append("source_doc_mapping must be a non-empty list")
    else:
        mapped_paths: set[str] = set()
        mapped_gate_ids: set[str] = set()
        for index, mapping in enumerate(source_doc_mapping):
            label = f"source_doc_mapping[{index}]"
            if not isinstance(mapping, dict):
                errors.append(f"{label} must be an object")
                continue
            path_value = mapping.get("path")
            if not is_nonempty_string(path_value):
                errors.append(f"{label}.path is required")
                continue
            if not is_safe_source_doc_path(path_value):
                errors.append(f"{label}.path must be a safe repo-relative docs/spec/rfcs path: {path_value}")
                continue
            mapped_paths.add(path_value)
            resolved_path = (repo_root / path_value).resolve()
            try:
                resolved_path.relative_to(repo_root.resolve())
            except ValueError:
                errors.append(f"{label}.path escapes repo root: {path_value}")
                continue
            if not resolved_path.exists():
                errors.append(f"{label}.path does not exist: {path_value}")
            maps_to = mapping.get("maps_to")
            if not isinstance(maps_to, list) or not maps_to:
                errors.append(f"{label}.maps_to must be a non-empty list")
            else:
                for gate_id in maps_to:
                    if not is_nonempty_string(gate_id):
                        errors.append(f"{label}.maps_to entries must be non-empty strings")
                    elif gate_id not in gate_ids:
                        errors.append(f"{label}.maps_to references unknown gate {gate_id}")
                    else:
                        mapped_gate_ids.add(gate_id)
            if not is_nonempty_string(mapping.get("relationship")):
                errors.append(f"{label}.relationship is required")
        missing_docs = REQUIRED_SOURCE_DOCS - mapped_paths
        for missing in sorted(missing_docs):
            errors.append(f"source_doc_mapping missing {missing}")
        unmapped_gates = gate_ids - mapped_gate_ids
        for gate_id in sorted(unmapped_gates):
            errors.append(f"{gate_id}: gate must be mapped by at least one source_doc_mapping entry")

    default_registry = repo_root / "docs" / "universeos" / "gate_registry.md"
    if registry_path.resolve() == default_registry.resolve():
        gates_doc = repo_root / "docs" / "universeos" / "gates.md"
        if gates_doc.exists():
            gates_text = gates_doc.read_text(encoding="utf-8")
            sections: dict[str, tuple[str, str]] = {}
            section_pattern = re.compile(
                r"^## (UOS-[A-Z]+-\d{3}):\s*(.*?)\n(.*?)(?=^## UOS-[A-Z]+-\d{3}:|\Z)",
                re.MULTILINE | re.DOTALL,
            )
            for match in section_pattern.finditer(gates_text):
                sections[match.group(1)] = (match.group(2).strip(), match.group(3))
            headings = set(sections)
            for gate_id in sorted(headings - gate_ids):
                errors.append(f"docs/universeos/gates.md lists {gate_id} but registry does not")
            for gate_id in sorted(gate_ids - headings):
                errors.append(f"registry lists {gate_id} but docs/universeos/gates.md does not")
            for gate_id, (title, status) in sorted(gate_metadata.items()):
                if gate_id not in sections:
                    continue
                section_title, body = sections[gate_id]
                if normalize_title(section_title) != normalize_title(title):
                    errors.append(f"docs/universeos/gates.md title for {gate_id} does not match registry")
                status_match = re.search(r"^Status:\s*`?([a-z]+)`?\.?\s*$", body, flags=re.MULTILINE)
                if status_match is None:
                    errors.append(f"docs/universeos/gates.md missing status for {gate_id}")
                elif status_match.group(1) != status:
                    errors.append(f"docs/universeos/gates.md status for {gate_id} does not match registry")
                if "Non-claim:" not in body:
                    errors.append(f"docs/universeos/gates.md missing non-claim for {gate_id}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument(
        "--registry",
        type=Path,
        default=None,
        help="Path to a registry markdown file. Defaults to docs/universeos/gate_registry.md.",
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    registry_path = (args.registry or (repo_root / "docs" / "universeos" / "gate_registry.md")).resolve()

    try:
        payload = extract_json_block(registry_path.read_text(encoding="utf-8"), registry_path)
        errors = validate_registry(payload, repo_root, registry_path)
    except (OSError, ValueError) as exc:
        print(f"universeos-gate-docs-error: {exc}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"universeos-gate-docs-error: {error}", file=sys.stderr)
        return 1

    gate_count = len(payload.get("gates", []))
    print(f"universeos-gate-docs-ok: {gate_count} gates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
