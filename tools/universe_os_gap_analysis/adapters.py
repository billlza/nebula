"""Fail-closed adapters for gate, case, workflow, release, and artifact metadata."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import tomllib
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Mapping

from .inventory import InventoryError
from .models import ExecutionState, SourceCategory, SourceInventoryEntry

GATE_REGISTRY_VERSION = 2
_GATE_ID = re.compile(r"^UOS-[A-Z]+-\d{3}$")
_CASE_ID = re.compile(r"^(?:ABI|BEN|BLD|CHK|RUN|SAF|TST)-\d{3}(?:-[A-Za-z0-9][A-Za-z0-9-]*)?$")
_WORKFLOW_ID = re.compile(r"^[A-Za-z0-9_.-]+$")
_RELEASE_NOTE = re.compile(r"^RELEASE_NOTES_v(.+)\.md$", re.IGNORECASE)
_ALLOWED_GATE_STATUSES = ("planned", "experimental", "candidate", "accepted")
_STATUS_RANK = {value: index for index, value in enumerate(_ALLOWED_GATE_STATUSES)}
_SUITE_PREFIX = {
    "abi": "ABI", "bench": "BEN", "build": "BLD", "check": "CHK",
    "run": "RUN", "safety": "SAF", "test": "TST",
}


@dataclass(frozen=True, slots=True)
class CaseManifest:
    case_id: str
    suite: str
    path: str
    execution_state: ExecutionState = field(default=ExecutionState.NOT_RUN, init=False)
    execution_detail: str = field(
        default="case definition parsed; the case was not executed by this adapter", init=False
    )


@dataclass(frozen=True, slots=True)
class GateDefinition:
    gate_id: str
    title: str
    status: str
    owner_area: str
    dependency_ids: tuple[str, ...]
    evidence_case_ids: tuple[str, ...]
    required_evidence: tuple[str, ...]
    non_claim: str
    source_path: str
    execution_state: ExecutionState = field(default=ExecutionState.NOT_RUN, init=False)
    execution_detail: str = field(
        default="gate definition parsed; no current gate execution was performed", init=False
    )


@dataclass(frozen=True, slots=True)
class SourceDocumentMapping:
    path: str
    gate_ids: tuple[str, ...]
    relationship: str


@dataclass(frozen=True, slots=True)
class WorkflowArtifactDefinition:
    name: str
    paths: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class WorkflowJobDefinition:
    workflow_path: str
    job_id: str
    workflow_category: SourceCategory
    dependency_job_ids: tuple[str, ...]
    artifacts: tuple[WorkflowArtifactDefinition, ...]
    execution_state: ExecutionState = field(default=ExecutionState.NOT_RUN, init=False)
    execution_detail: str = field(
        default="workflow job definition parsed; CI history is not current execution evidence",
        init=False,
    )


@dataclass(frozen=True, slots=True)
class MetadataRecord:
    source_path: str
    metadata_kind: str
    identity: str
    fields: tuple[str, ...]
    execution_state: ExecutionState
    execution_detail: str


@dataclass(frozen=True, slots=True)
class AdapterBundle:
    gate_registry_version: int
    gates: tuple[GateDefinition, ...]
    source_mappings: tuple[SourceDocumentMapping, ...]
    cases: tuple[CaseManifest, ...]
    workflow_jobs: tuple[WorkflowJobDefinition, ...]
    release_metadata: tuple[MetadataRecord, ...]
    artifact_metadata: tuple[MetadataRecord, ...]


class RepositoryEvidenceAdapter:
    """Adapt a Task 3.1 inventory without executing commands or consulting CI APIs."""

    def adapt(
        self, repo_root: Path, inventory: Iterable[SourceInventoryEntry]
    ) -> AdapterBundle:
        root = self._root(repo_root)
        entries = tuple(inventory)
        by_path = self._validate_inventory(entries)

        case_entries = tuple(
            entry for entry in entries
            if entry.category is SourceCategory.TEST
            and PurePosixPath(str(entry.path)).name == "case.toml"
        )
        if not case_entries:
            self._fail("INV-CASE-MISSING", "no case.toml manifests are present in inventory", "discover-case-manifests")
        cases = tuple(sorted((self._parse_case(root, item) for item in case_entries), key=lambda item: item.case_id))
        self._reject_duplicate_values(
            (item.case_id for item in cases), "INV-CASE-DUPLICATE", "case ID", "validate-case-references"
        )
        case_ids = frozenset(item.case_id for item in cases)

        registry_entry = by_path.get("docs/universeos/gate_registry.md")
        if registry_entry is None or registry_entry.category is not SourceCategory.GATE_REGISTRY:
            self._fail("INV-GATE-MISSING", "gate registry is absent from inventory", "locate-gate-registry")
        registry = self._parse_gate_registry(root, registry_entry, case_ids, by_path)

        workflow_entries = tuple(
            item for item in entries
            if item.category in {SourceCategory.CI_WORKFLOW, SourceCategory.RELEASE_WORKFLOW}
        )
        if not workflow_entries:
            self._fail("INV-WORKFLOW-MISSING", "no CI or release workflows are present in inventory", "locate-workflows")
        jobs = tuple(sorted(
            (job for entry in workflow_entries for job in self._parse_workflow(root, entry)),
            key=lambda item: (item.workflow_path, item.job_id),
        ))

        release_records = self._release_records(root, entries)
        artifact_records = self._artifact_records(root, entries, jobs)
        return AdapterBundle(
            gate_registry_version=registry[0],
            gates=registry[1],
            source_mappings=registry[2],
            cases=cases,
            workflow_jobs=jobs,
            release_metadata=release_records,
            artifact_metadata=artifact_records,
        )

    @staticmethod
    def _root(repo_root: Path) -> Path:
        try:
            root = repo_root.expanduser().resolve(strict=True)
        except OSError as error:
            raise InventoryError(
                "INV-ADAPTER-ROOT", str(error), operation="resolve-adapter-root"
            ) from error
        if not root.is_dir():
            raise InventoryError(
                "INV-ADAPTER-ROOT", "adapter root is not a directory", operation="resolve-adapter-root"
            )
        return root

    def _validate_inventory(
        self, entries: tuple[SourceInventoryEntry, ...]
    ) -> dict[str, SourceInventoryEntry]:
        by_path: dict[str, SourceInventoryEntry] = {}
        ids: set[str] = set()
        for entry in entries:
            if not isinstance(entry, SourceInventoryEntry):
                self._fail("INV-ADAPTER-INVENTORY", "inventory contains an invalid entry", "validate-inventory")
            path = str(entry.path)
            if path in by_path:
                self._fail("INV-ADAPTER-DUPLICATE-PATH", f"duplicate inventory path: {path}", "validate-inventory", path)
            if str(entry.id) in ids:
                self._fail("INV-ADAPTER-DUPLICATE-ID", f"duplicate inventory ID: {entry.id}", "validate-inventory", path)
            by_path[path] = entry
            ids.add(str(entry.id))
        return by_path

    def _read(self, root: Path, entry: SourceInventoryEntry) -> bytes:
        relative = str(entry.path)
        if not entry.inspected:
            self._fail(
                "INV-ADAPTER-UNINSPECTED",
                "adapter source was not inspected by the Task 3.1 inventory",
                "read-inventoried-source",
                relative,
            )
        components = PurePosixPath(relative).parts
        root_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_DIRECTORY", 0)
        directory_flags = root_flags | getattr(os, "O_NOFOLLOW", 0)
        file_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            parent_fd = os.open(root, root_flags)
        except OSError as error:
            raise InventoryError(
                "INV-ADAPTER-ROOT", str(error), operation="open-adapter-root"
            ) from error
        try:
            for component in components[:-1]:
                try:
                    child_fd = os.open(component, directory_flags, dir_fd=parent_fd)
                except OSError as error:
                    raise InventoryError(
                        "INV-ADAPTER-PATH", str(error),
                        operation="open-adapter-parent-without-following-links", path=relative,
                    ) from error
                os.close(parent_fd)
                parent_fd = child_fd
            try:
                before = os.stat(components[-1], dir_fd=parent_fd, follow_symlinks=False)
                descriptor = os.open(components[-1], file_flags, dir_fd=parent_fd)
            except OSError as error:
                raise InventoryError(
                    "INV-ADAPTER-PATH", str(error),
                    operation="open-inventoried-source-without-following-links", path=relative,
                ) from error
            digest = hashlib.sha256()
            payload = bytearray()
            length = 0
            try:
                opened = os.fstat(descriptor)
                while True:
                    chunk = os.read(descriptor, 1024 * 1024)
                    if not chunk:
                        break
                    payload.extend(chunk)
                    digest.update(chunk)
                    length += len(chunk)
                after = os.fstat(descriptor)
                path_after = os.stat(components[-1], dir_fd=parent_fd, follow_symlinks=False)
            except OSError as error:
                raise InventoryError(
                    "INV-ADAPTER-READ", str(error),
                    operation="read-inventoried-source", path=relative,
                ) from error
            finally:
                os.close(descriptor)
        finally:
            os.close(parent_fd)
        identities = {
            self._stat_identity(value) for value in (before, opened, after, path_after)
        }
        if not stat.S_ISREG(before.st_mode) or len(identities) != 1 or length != after.st_size:
            self._fail(
                "INV-ADAPTER-CONTENT-DRIFT",
                "adapter source changed or ceased to be a regular file while being read",
                "verify-inventory-content-identity",
                relative,
            )
        if digest.hexdigest() != entry.content_hash:
            self._fail(
                "INV-ADAPTER-CONTENT-DRIFT",
                "source content no longer matches the Task 3.1 inventory hash",
                "verify-inventory-content-hash", relative,
            )
        return bytes(payload)

    def _text(self, root: Path, entry: SourceInventoryEntry) -> str:
        try:
            return self._read(root, entry).decode("utf-8", errors="strict")
        except UnicodeError as error:
            raise InventoryError(
                "INV-ADAPTER-DECODE", "adapter source is not valid UTF-8",
                operation="decode-inventoried-source", path=str(entry.path),
            ) from error

    def _parse_case(self, root: Path, entry: SourceInventoryEntry) -> CaseManifest:
        path = str(entry.path)
        try:
            document = tomllib.loads(self._text(root, entry))
        except tomllib.TOMLDecodeError as error:
            raise InventoryError(
                "INV-CASE-PARSE", str(error), operation="parse-case-manifest", path=path
            ) from error
        if not isinstance(document, dict):
            self._fail("INV-CASE-SCHEMA", "case manifest root must be a table", "validate-case-schema", path)
        case_id = self._required_string(document, "id", "INV-CASE-SCHEMA", path)
        suite = self._required_string(document, "suite", "INV-CASE-SCHEMA", path)
        if not _CASE_ID.fullmatch(case_id):
            self._fail("INV-CASE-ID", f"invalid case ID: {case_id}", "validate-case-id", path)
        if suite not in _SUITE_PREFIX:
            self._fail("INV-CASE-SCHEMA", f"unknown case suite: {suite}", "validate-case-schema", path)
        if not case_id.startswith(_SUITE_PREFIX[suite] + "-"):
            self._fail("INV-CASE-ID", f"case ID {case_id} does not match suite {suite}", "validate-case-id", path)
        return CaseManifest(case_id=case_id, suite=suite, path=path)

    def _parse_gate_registry(
        self,
        root: Path,
        entry: SourceInventoryEntry,
        case_ids: frozenset[str],
        inventory_by_path: Mapping[str, SourceInventoryEntry],
    ) -> tuple[int, tuple[GateDefinition, ...], tuple[SourceDocumentMapping, ...]]:
        path = str(entry.path)
        text = self._text(root, entry)
        blocks = re.findall(r"```json\s*\n(.*?)\n```", text, flags=re.DOTALL)
        if len(blocks) != 1:
            self._fail("INV-GATE-JSON-BLOCK", "gate registry must contain exactly one JSON block", "extract-gate-registry", path)
        document = self._strict_json(blocks[0], "INV-GATE-PARSE", "parse-gate-registry", path)
        if not isinstance(document, dict):
            self._fail("INV-GATE-SCHEMA", "gate registry JSON root must be an object", "validate-gate-schema", path)
        version = document.get("gate_registry_version")
        if type(version) is not int or version != GATE_REGISTRY_VERSION:
            self._fail(
                "INV-GATE-VERSION", f"gate_registry_version must be integer {GATE_REGISTRY_VERSION}",
                "validate-gate-schema", path,
            )
        naming = document.get("gate_naming")
        raw_gates = document.get("gates")
        raw_mappings = document.get("source_doc_mapping")
        if not isinstance(naming, dict) or not naming or not all(
            isinstance(key, str) and isinstance(value, str) and value.strip()
            for key, value in naming.items()
        ):
            self._fail("INV-GATE-SCHEMA", "gate_naming must be a non-empty string map", "validate-gate-schema", path)
        if not isinstance(raw_gates, list) or not raw_gates:
            self._fail("INV-GATE-SCHEMA", "gates must be a non-empty list", "validate-gate-schema", path)
        if not isinstance(raw_mappings, list) or not raw_mappings:
            self._fail("INV-GATE-SCHEMA", "source_doc_mapping must be a non-empty list", "validate-gate-schema", path)

        gates: list[GateDefinition] = []
        seen_gate_ids: set[str] = set()
        for raw in raw_gates:
            if not isinstance(raw, dict):
                self._fail("INV-GATE-SCHEMA", "every gate must be an object", "validate-gate-schema", path)
            gate_id = self._required_string(raw, "id", "INV-GATE-SCHEMA", path)
            if not _GATE_ID.fullmatch(gate_id):
                self._fail("INV-GATE-ID", f"invalid gate ID: {gate_id}", "validate-gate-id", path)
            if gate_id in seen_gate_ids:
                self._fail("INV-GATE-DUPLICATE", f"duplicate gate ID: {gate_id}", "validate-gate-id", path)
            seen_gate_ids.add(gate_id)
            prefix = gate_id.rsplit("-", 1)[0]
            if prefix not in naming:
                self._fail("INV-GATE-ID", f"gate ID prefix is not declared: {prefix}", "validate-gate-id", path)
            status = self._required_string(raw, "status", "INV-GATE-SCHEMA", path)
            if status not in _STATUS_RANK:
                self._fail("INV-GATE-SCHEMA", f"unknown gate status: {status}", "validate-gate-schema", path)
            dependencies = self._string_list(
                raw, "depends_on", "INV-GATE-SCHEMA", path, allow_empty=True
            )
            evidence_cases = self._string_list(raw, "evidence_cases", "INV-GATE-SCHEMA", path, required=False)
            required_evidence = self._string_list(raw, "required_evidence", "INV-GATE-SCHEMA", path)
            self._reject_duplicate_values(dependencies, "INV-GATE-DEPENDENCY", "dependency", "validate-gate-dependencies", path)
            self._reject_duplicate_values(evidence_cases, "INV-GATE-EVIDENCE", "evidence case", "validate-gate-evidence", path)
            if status != "planned" and not evidence_cases:
                self._fail("INV-GATE-EVIDENCE", f"{gate_id} requires evidence_cases", "validate-gate-evidence", path)
            gates.append(GateDefinition(
                gate_id=gate_id,
                title=self._required_string(raw, "title", "INV-GATE-SCHEMA", path),
                status=status,
                owner_area=self._required_string(raw, "owner_area", "INV-GATE-SCHEMA", path),
                dependency_ids=tuple(dependencies), evidence_case_ids=tuple(evidence_cases),
                required_evidence=tuple(required_evidence),
                non_claim=self._required_string(raw, "non_claim", "INV-GATE-NON-CLAIM", path),
                source_path=path,
            ))

        gate_by_id = {item.gate_id: item for item in gates}
        for gate in gates:
            for dependency in gate.dependency_ids:
                if dependency not in gate_by_id:
                    self._fail("INV-GATE-DEPENDENCY", f"{gate.gate_id} references unknown dependency {dependency}", "validate-gate-dependencies", path)
                if dependency == gate.gate_id:
                    self._fail("INV-GATE-DEPENDENCY", f"{gate.gate_id} depends on itself", "validate-gate-dependencies", path)
                if _STATUS_RANK[gate.status] > _STATUS_RANK[gate_by_id[dependency].status]:
                    self._fail(
                        "INV-GATE-DEPENDENCY", f"{gate.gate_id} status outranks dependency {dependency}",
                        "validate-gate-dependencies", path,
                    )
            for case_id in gate.evidence_case_ids:
                if case_id not in case_ids:
                    self._fail("INV-GATE-EVIDENCE", f"{gate.gate_id} references unknown evidence case {case_id}", "validate-gate-evidence", path)
        cycle = self._dependency_cycle({item.gate_id: item.dependency_ids for item in gates})
        if cycle:
            self._fail("INV-GATE-DEPENDENCY-CYCLE", "dependency cycle: " + " -> ".join(cycle), "validate-gate-dependencies", path)

        mappings: list[SourceDocumentMapping] = []
        seen_mapping_paths: set[str] = set()
        for raw in raw_mappings:
            if not isinstance(raw, dict):
                self._fail("INV-GATE-SCHEMA", "every source mapping must be an object", "validate-gate-mappings", path)
            mapped_path = self._required_string(raw, "path", "INV-GATE-SCHEMA", path)
            if mapped_path in seen_mapping_paths:
                self._fail("INV-GATE-DUPLICATE", f"duplicate source mapping: {mapped_path}", "validate-gate-mappings", path)
            seen_mapping_paths.add(mapped_path)
            try:
                normalized = PurePosixPath(mapped_path)
                if normalized.is_absolute() or ".." in normalized.parts or normalized.as_posix() != mapped_path:
                    raise ValueError
            except (TypeError, ValueError):
                self._fail("INV-GATE-REFERENCE", f"unsafe source mapping path: {mapped_path}", "validate-gate-mappings", path)
            if mapped_path not in inventory_by_path:
                self._fail("INV-GATE-REFERENCE", f"source mapping is absent from inventory: {mapped_path}", "validate-gate-mappings", path)
            mapped_ids = self._string_list(raw, "maps_to", "INV-GATE-SCHEMA", path)
            self._reject_duplicate_values(mapped_ids, "INV-GATE-DUPLICATE", "mapped gate", "validate-gate-mappings", path)
            for gate_id in mapped_ids:
                if gate_id not in gate_by_id:
                    self._fail("INV-GATE-REFERENCE", f"source mapping references unknown gate {gate_id}", "validate-gate-mappings", path)
            mappings.append(SourceDocumentMapping(
                path=mapped_path, gate_ids=tuple(mapped_ids),
                relationship=self._required_string(raw, "relationship", "INV-GATE-SCHEMA", path),
            ))
        return version, tuple(sorted(gates, key=lambda item: item.gate_id)), tuple(sorted(mappings, key=lambda item: item.path))

    def _parse_workflow(
        self, root: Path, entry: SourceInventoryEntry
    ) -> tuple[WorkflowJobDefinition, ...]:
        path = str(entry.path)
        text = self._text(root, entry)
        lines = text.splitlines()
        if any("\t" in line[: len(line) - len(line.lstrip())] for line in lines):
            self._fail("INV-WORKFLOW-PARSE", "tabs are not allowed in YAML indentation", "parse-workflow", path)
        jobs_rows = [index for index, line in enumerate(lines) if re.fullmatch(r"\s*jobs:\s*(?:#.*)?", line)]
        if len(jobs_rows) != 1:
            self._fail("INV-WORKFLOW-SCHEMA", "workflow must contain exactly one jobs mapping", "parse-workflow", path)
        jobs_row = jobs_rows[0]
        jobs_indent = self._indent(lines[jobs_row])
        starts: list[tuple[int, int, str]] = []
        child_indent: int | None = None
        block_scalar_indent: int | None = None
        for index in range(jobs_row + 1, len(lines)):
            line = lines[index]
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            indent = self._indent(line)
            if block_scalar_indent is not None:
                if indent > block_scalar_indent:
                    continue
                block_scalar_indent = None
            if indent <= jobs_indent:
                break
            if re.search(r"[>|][-+]?\s*(?:#.*)?$", stripped):
                block_scalar_indent = indent
            match = re.match(r"^\s*([A-Za-z0-9_.-]+):(?:\s|$)", line)
            if match:
                if child_indent is None:
                    child_indent = indent
                if indent == child_indent:
                    job_id = match.group(1)
                    if not _WORKFLOW_ID.fullmatch(job_id):
                        self._fail("INV-WORKFLOW-SCHEMA", f"invalid workflow job ID: {job_id}", "parse-workflow", path)
                    starts.append((index, indent, job_id))
        if not starts:
            self._fail("INV-WORKFLOW-SCHEMA", "workflow jobs mapping is empty", "parse-workflow", path)
        self._reject_duplicate_values((item[2] for item in starts), "INV-WORKFLOW-DUPLICATE", "workflow job", "parse-workflow", path)

        jobs: list[WorkflowJobDefinition] = []
        for position, (start, indent, job_id) in enumerate(starts):
            end = starts[position + 1][0] if position + 1 < len(starts) else self._workflow_block_end(lines, start, jobs_indent)
            block = lines[start:end]
            dependencies = self._workflow_needs(block, path, job_id)
            artifacts = self._workflow_artifacts(block, path, job_id)
            jobs.append(WorkflowJobDefinition(
                workflow_path=path, job_id=job_id, workflow_category=entry.category,
                dependency_job_ids=dependencies, artifacts=artifacts,
            ))
        known = {item.job_id for item in jobs}
        for job in jobs:
            for dependency in job.dependency_job_ids:
                if dependency not in known:
                    self._fail(
                        "INV-WORKFLOW-REFERENCE",
                        f"workflow job {job.job_id} needs unknown job {dependency}",
                        "validate-workflow-dependencies", path,
                    )
                if dependency == job.job_id:
                    self._fail("INV-WORKFLOW-REFERENCE", f"workflow job {job.job_id} needs itself", "validate-workflow-dependencies", path)
        cycle = self._dependency_cycle({item.job_id: item.dependency_job_ids for item in jobs})
        if cycle:
            self._fail("INV-WORKFLOW-REFERENCE", "workflow dependency cycle: " + " -> ".join(cycle), "validate-workflow-dependencies", path)
        return tuple(sorted(jobs, key=lambda item: item.job_id))

    @staticmethod
    def _workflow_block_end(lines: list[str], start: int, jobs_indent: int) -> int:
        for index in range(start + 1, len(lines)):
            if lines[index].strip() and not lines[index].lstrip().startswith("#") and RepositoryEvidenceAdapter._indent(lines[index]) <= jobs_indent:
                return index
        return len(lines)

    def _workflow_needs(self, block: list[str], path: str, job_id: str) -> tuple[str, ...]:
        values: list[str] = []
        for line in block[1:]:
            match = re.match(r"^\s*needs:\s*(.*?)\s*(?:#.*)?$", line)
            if not match:
                continue
            raw = match.group(1).strip()
            if not raw:
                self._fail("INV-WORKFLOW-SCHEMA", f"workflow job {job_id} has unsupported empty needs", "parse-workflow-needs", path)
            if raw.startswith("[") and raw.endswith("]"):
                values.extend(item.strip().strip("'\"") for item in raw[1:-1].split(",") if item.strip())
            else:
                values.append(raw.strip("'\""))
        self._reject_duplicate_values(values, "INV-WORKFLOW-DUPLICATE", "workflow dependency", "parse-workflow-needs", path)
        if not all(_WORKFLOW_ID.fullmatch(value) for value in values):
            self._fail("INV-WORKFLOW-SCHEMA", f"workflow job {job_id} has invalid needs", "parse-workflow-needs", path)
        return tuple(values)

    def _workflow_artifacts(
        self, block: list[str], path: str, job_id: str
    ) -> tuple[WorkflowArtifactDefinition, ...]:
        artifacts: list[WorkflowArtifactDefinition] = []
        for index, line in enumerate(block):
            if not re.search(r"uses:\s*actions/upload-artifact@", line):
                continue
            uses_indent = self._indent(line)
            name: str | None = None
            paths: list[str] = []
            cursor = index + 1
            while cursor < len(block):
                current = block[cursor]
                stripped = current.strip()
                indent = self._indent(current)
                if stripped.startswith("-") and indent <= uses_indent:
                    break
                key = re.match(r"^\s*(name|path):\s*(.*?)\s*$", current)
                if key and indent > uses_indent:
                    value = key.group(2)
                    if key.group(1) == "name":
                        name = value.strip().strip("'\"")
                    elif value not in {"|", ">", "|-", ">-"}:
                        paths.append(value.strip().strip("'\""))
                    else:
                        scalar_indent = indent
                        cursor += 1
                        while cursor < len(block) and (
                            not block[cursor].strip() or self._indent(block[cursor]) > scalar_indent
                        ):
                            candidate = block[cursor].strip()
                            if candidate and not candidate.startswith("#"):
                                paths.append(candidate)
                            cursor += 1
                        continue
                cursor += 1
            if not name:
                self._fail(
                    "INV-WORKFLOW-ARTIFACT-SCHEMA",
                    f"upload-artifact step in job {job_id} has no name",
                    "parse-workflow-artifacts", path,
                )
            if not paths:
                self._fail(
                    "INV-WORKFLOW-ARTIFACT-SCHEMA",
                    f"upload-artifact step in job {job_id} has no path",
                    "parse-workflow-artifacts", path,
                )
            artifacts.append(WorkflowArtifactDefinition(name=name, paths=tuple(paths)))
        self._reject_duplicate_values(
            (item.name for item in artifacts), "INV-WORKFLOW-DUPLICATE",
            "workflow artifact name", "parse-workflow-artifacts", path,
        )
        return tuple(sorted(artifacts, key=lambda item: item.name))

    def _release_records(
        self, root: Path, entries: tuple[SourceInventoryEntry, ...]
    ) -> tuple[MetadataRecord, ...]:
        records: list[MetadataRecord] = []
        seen_versions: set[str] = set()
        for entry in entries:
            if entry.category is not SourceCategory.RELEASE_NOTES:
                continue
            source_path = str(entry.path)
            match = _RELEASE_NOTE.fullmatch(PurePosixPath(source_path).name)
            if not match:
                self._fail(
                    "INV-RELEASE-METADATA-SCHEMA",
                    "release note filename does not contain a version",
                    "parse-release-metadata", source_path,
                )
            version = match.group(1)
            if version in seen_versions:
                self._fail(
                    "INV-RELEASE-METADATA-DUPLICATE",
                    f"duplicate release-note version: {version}",
                    "parse-release-metadata", source_path,
                )
            seen_versions.add(version)
            if not self._text(root, entry).strip():
                self._fail(
                    "INV-RELEASE-METADATA-SCHEMA",
                    "release note is empty",
                    "parse-release-metadata", source_path,
                )
            records.append(MetadataRecord(
                source_path=source_path, metadata_kind="ReleaseNote",
                identity=version, fields=tuple(entry.stable_anchors),
                execution_state=ExecutionState.NOT_RUN,
                execution_detail="release history was inspected; it is not a current execution result",
            ))
        return tuple(sorted(records, key=lambda item: (item.identity, item.source_path)))

    def _artifact_records(
        self,
        root: Path,
        entries: tuple[SourceInventoryEntry, ...],
        jobs: tuple[WorkflowJobDefinition, ...],
    ) -> tuple[MetadataRecord, ...]:
        records: list[MetadataRecord] = []
        for entry in entries:
            if entry.category is not SourceCategory.ARTIFACT or not self._metadata_candidate(str(entry.path)):
                continue
            records.append(self._parse_artifact_metadata(root, entry))
        available_names = {PurePosixPath(item.source_path).name for item in records}
        for job in jobs:
            for artifact in job.artifacts:
                if artifact.name in available_names:
                    continue
                records.append(MetadataRecord(
                    source_path=job.workflow_path,
                    metadata_kind="WorkflowArtifactDefinition",
                    identity=f"{job.job_id}:{artifact.name}", fields=artifact.paths,
                    execution_state=ExecutionState.UNAVAILABLE,
                    execution_detail=(
                        "workflow defines this remote asset, but no bound current-execution "
                        "artifact metadata is available in the inventory"
                    ),
                ))
        return tuple(sorted(records, key=lambda item: (item.metadata_kind, item.identity, item.source_path)))

    @staticmethod
    def _metadata_candidate(path: str) -> bool:
        name = PurePosixPath(path).name.lower()
        return (
            name == "release-manifest.json" or name.endswith(".nebmeta")
            or name.endswith(".spdx.json") or name.endswith(".intoto.json")
            or name.endswith(".intoto.jsonl")
        )

    def _parse_artifact_metadata(
        self, root: Path, entry: SourceInventoryEntry
    ) -> MetadataRecord:
        path = str(entry.path)
        name = PurePosixPath(path).name
        lower_name = name.lower()
        payload = self._read(root, entry)
        if lower_name.endswith(".nebmeta"):
            try:
                text = payload.decode("utf-8", errors="strict")
            except UnicodeError as error:
                raise InventoryError(
                    "INV-ARTIFACT-METADATA-DECODE",
                    "native artifact metadata is not valid UTF-8",
                    operation="parse-artifact-metadata",
                    path=path,
                ) from error
            values: dict[str, str] = {}
            for line_number, line in enumerate(text.splitlines(), start=1):
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                match = re.fullmatch(r"([A-Za-z0-9_.-]+)=(.*)", stripped)
                if match is None:
                    self._fail(
                        "INV-ARTIFACT-METADATA-SCHEMA",
                        f"invalid native metadata field at line {line_number}",
                        "parse-artifact-metadata",
                        path,
                    )
                key, value = match.groups()
                if key in values:
                    self._fail(
                        "INV-ARTIFACT-METADATA-DUPLICATE",
                        f"duplicate native metadata field: {key}",
                        "parse-artifact-metadata",
                        path,
                    )
                values[key] = value
            if not values:
                self._fail(
                    "INV-ARTIFACT-METADATA-SCHEMA",
                    "native artifact metadata is empty",
                    "parse-artifact-metadata",
                    path,
                )
            identity = next(
                (values[key] for key in ("artifact", "artifact_name", "name") if values.get(key)),
                name,
            )
            kind = values.get("artifact_kind", "NativeArtifactMetadata")
            return MetadataRecord(
                source_path=path,
                metadata_kind=kind,
                identity=identity,
                fields=tuple(sorted(values)),
                execution_state=ExecutionState.NOT_RUN,
                execution_detail=(
                    "artifact metadata was hash-bound and parsed; the producing command was not "
                    "executed by this adapter"
                ),
            )

        if lower_name.endswith(".intoto.jsonl"):
            try:
                text = payload.decode("utf-8", errors="strict")
            except UnicodeError as error:
                raise InventoryError(
                    "INV-ARTIFACT-METADATA-DECODE",
                    "attestation metadata is not valid UTF-8",
                    operation="parse-artifact-metadata",
                    path=path,
                ) from error
            documents: list[object] = []
            for line_number, line in enumerate(text.splitlines(), start=1):
                if not line.strip():
                    continue
                documents.append(
                    self._strict_json(
                        line,
                        "INV-ARTIFACT-METADATA-PARSE",
                        f"parse-attestation-line-{line_number}",
                        path,
                    )
                )
            if not documents or not all(isinstance(item, dict) for item in documents):
                self._fail(
                    "INV-ARTIFACT-METADATA-SCHEMA",
                    "attestation JSONL must contain one or more JSON objects",
                    "validate-attestation-metadata",
                    path,
                )
            return MetadataRecord(
                source_path=path,
                metadata_kind="InTotoAttestationBundle",
                identity=name,
                fields=tuple(
                    sorted({str(key) for item in documents for key in item})
                ),
                execution_state=ExecutionState.NOT_RUN,
                execution_detail=(
                    "attestation history was hash-bound and parsed; no attestation was produced "
                    "or verified by this adapter"
                ),
            )

        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeError as error:
            raise InventoryError(
                "INV-ARTIFACT-METADATA-DECODE",
                "artifact metadata is not valid UTF-8",
                operation="parse-artifact-metadata",
                path=path,
            ) from error
        document = self._strict_json(
            text, "INV-ARTIFACT-METADATA-PARSE", "parse-artifact-metadata", path
        )
        if not isinstance(document, dict):
            self._fail(
                "INV-ARTIFACT-METADATA-SCHEMA",
                "artifact metadata root must be an object",
                "validate-artifact-metadata",
                path,
            )
        if lower_name == "release-manifest.json":
            return self._release_manifest_record(path, document)
        if lower_name.endswith(".spdx.json"):
            return self._spdx_record(path, name, document)
        if lower_name.endswith(".intoto.json"):
            return MetadataRecord(
                source_path=path,
                metadata_kind="InTotoStatement",
                identity=name,
                fields=tuple(sorted(str(key) for key in document)),
                execution_state=ExecutionState.NOT_RUN,
                execution_detail=(
                    "attestation statement was hash-bound and parsed; it was not produced or "
                    "verified by this adapter"
                ),
            )
        self._fail(
            "INV-ARTIFACT-METADATA-SCHEMA",
            "unrecognized artifact metadata kind",
            "validate-artifact-metadata",
            path,
        )

    def _release_manifest_record(
        self, path: str, document: Mapping[str, object]
    ) -> MetadataRecord:
        version = self._required_string(
            document, "version", "INV-RELEASE-METADATA-SCHEMA", path
        )
        names: set[str] = set()
        assets: set[str] = set()
        for collection in ("artifacts", "backend_sdks", "sboms", "attestations"):
            rows = document.get(collection)
            if not isinstance(rows, list):
                self._fail(
                    "INV-RELEASE-METADATA-SCHEMA",
                    f"release manifest {collection} must be a list",
                    "validate-release-manifest",
                    path,
                )
            for row in rows:
                if not isinstance(row, dict):
                    self._fail(
                        "INV-RELEASE-METADATA-SCHEMA",
                        f"release manifest {collection} entries must be objects",
                        "validate-release-manifest",
                        path,
                    )
                item_name = self._required_string(
                    row, "name", "INV-RELEASE-METADATA-SCHEMA", path
                )
                if item_name in names:
                    self._fail(
                        "INV-RELEASE-METADATA-DUPLICATE",
                        f"duplicate release asset metadata: {item_name}",
                        "validate-release-manifest",
                        path,
                    )
                names.add(item_name)
                if collection in {"artifacts", "backend_sdks"}:
                    assets.add(item_name)
                if collection == "sboms":
                    subject = self._required_string(
                        row, "subject", "INV-RELEASE-METADATA-SCHEMA", path
                    )
                    if subject not in assets:
                        self._fail(
                            "INV-RELEASE-METADATA-REFERENCE",
                            f"SBOM {item_name} references unknown subject {subject}",
                            "validate-release-manifest",
                            path,
                        )
        metadata = document.get("metadata")
        if not isinstance(metadata, dict):
            self._fail(
                "INV-RELEASE-METADATA-SCHEMA",
                "release manifest metadata must be an object",
                "validate-release-manifest",
                path,
            )
        for key, value in metadata.items():
            if not isinstance(key, str) or not isinstance(value, dict):
                self._fail(
                    "INV-RELEASE-METADATA-SCHEMA",
                    "release manifest metadata entries must be named objects",
                    "validate-release-manifest",
                    path,
                )
            metadata_name = self._required_string(
                value, "name", "INV-RELEASE-METADATA-SCHEMA", path
            )
            if metadata_name in names and key != "manifest":
                self._fail(
                    "INV-RELEASE-METADATA-DUPLICATE",
                    f"duplicate release metadata name: {metadata_name}",
                    "validate-release-manifest",
                    path,
                )
            names.add(metadata_name)
        return MetadataRecord(
            source_path=path,
            metadata_kind="ReleaseManifest",
            identity=version,
            fields=tuple(sorted(names)),
            execution_state=ExecutionState.NOT_RUN,
            execution_detail=(
                "release artifact history was hash-bound and parsed; assets were not fetched or "
                "verified as current execution evidence"
            ),
        )

    def _spdx_record(
        self, path: str, name: str, document: Mapping[str, object]
    ) -> MetadataRecord:
        spdx_version = self._required_string(
            document, "spdxVersion", "INV-ARTIFACT-METADATA-SCHEMA", path
        )
        document_id = self._required_string(
            document, "SPDXID", "INV-ARTIFACT-METADATA-SCHEMA", path
        )
        identifiers = {document_id}
        for collection in ("packages", "files"):
            rows = document.get(collection, [])
            if not isinstance(rows, list):
                self._fail(
                    "INV-ARTIFACT-METADATA-SCHEMA",
                    f"SPDX {collection} must be a list",
                    "validate-spdx-metadata",
                    path,
                )
            for row in rows:
                if not isinstance(row, dict):
                    self._fail(
                        "INV-ARTIFACT-METADATA-SCHEMA",
                        f"SPDX {collection} entries must be objects",
                        "validate-spdx-metadata",
                        path,
                    )
                identifier = self._required_string(
                    row, "SPDXID", "INV-ARTIFACT-METADATA-SCHEMA", path
                )
                if identifier in identifiers:
                    self._fail(
                        "INV-ARTIFACT-METADATA-DUPLICATE",
                        f"duplicate SPDX identifier: {identifier}",
                        "validate-spdx-metadata",
                        path,
                    )
                identifiers.add(identifier)
        relationships = document.get("relationships", [])
        if not isinstance(relationships, list):
            self._fail(
                "INV-ARTIFACT-METADATA-SCHEMA",
                "SPDX relationships must be a list",
                "validate-spdx-metadata",
                path,
            )
        for relationship in relationships:
            if not isinstance(relationship, dict):
                self._fail(
                    "INV-ARTIFACT-METADATA-SCHEMA",
                    "SPDX relationships must be objects",
                    "validate-spdx-metadata",
                    path,
                )
            for key in ("spdxElementId", "relatedSpdxElement"):
                reference = self._required_string(
                    relationship, key, "INV-ARTIFACT-METADATA-SCHEMA", path
                )
                if reference not in identifiers and reference not in {"NONE", "NOASSERTION"}:
                    self._fail(
                        "INV-ARTIFACT-METADATA-REFERENCE",
                        f"SPDX relationship references unknown identifier {reference}",
                        "validate-spdx-metadata",
                        path,
                    )
        return MetadataRecord(
            source_path=path,
            metadata_kind="SPDX",
            identity=name,
            fields=tuple(sorted(identifiers)),
            execution_state=ExecutionState.NOT_RUN,
            execution_detail=(
                f"{spdx_version} metadata was hash-bound and parsed; the SBOM subject was not "
                "built or fetched by this adapter"
            ),
        )

    @staticmethod
    def _required_string(
        document: Mapping[str, object], key: str, code: str, path: str
    ) -> str:
        value = document.get(key)
        if not isinstance(value, str) or not value.strip():
            raise InventoryError(
                code,
                f"{key} must be a non-empty string",
                operation="validate-adapter-schema",
                path=path,
            )
        return value

    def _string_list(
        self,
        document: Mapping[str, object],
        key: str,
        code: str,
        path: str,
        *,
        required: bool = True,
        allow_empty: bool = False,
    ) -> list[str]:
        value = document.get(key)
        if value is None and not required:
            return []
        if not isinstance(value, list) or (not allow_empty and not value):
            self._fail(
                code,
                f"{key} must be {'a' if allow_empty else 'a non-empty'} list",
                "validate-adapter-schema",
                path,
            )
        if not all(isinstance(item, str) and item.strip() for item in value):
            self._fail(
                code,
                f"{key} must contain only non-empty strings",
                "validate-adapter-schema",
                path,
            )
        return list(value)

    def _strict_json(
        self, text: str, code: str, operation: str, path: str
    ) -> object:
        def reject_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
            result: dict[str, object] = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError(f"duplicate JSON key: {key}")
                result[key] = value
            return result

        try:
            return json.loads(text, object_pairs_hook=reject_duplicates)
        except (json.JSONDecodeError, ValueError) as error:
            raise InventoryError(code, str(error), operation=operation, path=path) from error

    def _reject_duplicate_values(
        self,
        values: Iterable[str],
        code: str,
        label: str,
        operation: str,
        path: str | None = None,
    ) -> None:
        seen: set[str] = set()
        for value in values:
            if value in seen:
                self._fail(code, f"duplicate {label}: {value}", operation, path)
            seen.add(value)

    @staticmethod
    def _dependency_cycle(graph: Mapping[str, Iterable[str]]) -> tuple[str, ...]:
        visiting: set[str] = set()
        visited: set[str] = set()
        stack: list[str] = []

        def visit(node: str) -> tuple[str, ...]:
            if node in visiting:
                start = stack.index(node)
                return tuple([*stack[start:], node])
            if node in visited:
                return ()
            visiting.add(node)
            stack.append(node)
            for dependency in graph[node]:
                cycle = visit(dependency)
                if cycle:
                    return cycle
            stack.pop()
            visiting.remove(node)
            visited.add(node)
            return ()

        for node in sorted(graph):
            cycle = visit(node)
            if cycle:
                return cycle
        return ()

    @staticmethod
    def _indent(line: str) -> int:
        return len(line) - len(line.lstrip(" "))

    @staticmethod
    def _stat_identity(value: os.stat_result) -> tuple[int, ...]:
        return (
            value.st_dev,
            value.st_ino,
            value.st_mode,
            value.st_size,
            value.st_mtime_ns,
            value.st_ctime_ns,
        )

    @staticmethod
    def _fail(
        code: str,
        message: str,
        operation: str,
        path: str | None = None,
    ) -> None:
        raise InventoryError(code, message, operation=operation, path=path)


def adapt_repository_evidence(
    repo_root: Path, inventory: Iterable[SourceInventoryEntry]
) -> AdapterBundle:
    """Parse repository definitions and metadata without executing commands or using a network."""

    return RepositoryEvidenceAdapter().adapt(repo_root, inventory)
