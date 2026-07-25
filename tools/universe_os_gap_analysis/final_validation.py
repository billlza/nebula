"""Final published-artifact-set validator (Task 15.2).

Task 15.1 *generates* the durable deliverable -- the versioned artifact set that
lives in ``tools/universe_os_gap_analysis/artifacts/`` -- from the single
canonical :class:`~tools.universe_os_gap_analysis.models.AssessmentModel`. This
module is the *final gate* over that on-disk deliverable: it loads the published
artifact set from a directory and cross-checks that the set is internally
consistent, that every artifact is a faithful projection of the canonical model
embedded in ``assessment.json``, and that the report never oversteps its
evidence. It emits **fail-closed** findings (each carrying requirement and object
references) and a single valid / invalid decision; any finding makes the report
NOT valid (Requirements 9.6, 9.7, 13.7, 14.1-14.7).

Unlike :mod:`~tools.universe_os_gap_analysis.validator` (which validates a live,
in-memory model before publish) this module validates the *already published
bytes on disk*. It never regenerates, mutates, or re-renders anything; it only
reads the committed artifact set and reports whether it may be trusted. The
checks performed:

1. **Schema.** ``assessment.json`` validates against the published
   ``assessment.schema.json`` under JSON Schema Draft 2020-12, and the schema
   itself is a valid Draft 2020-12 schema.
2. **Table parity.** The capability matrix has exactly one row per model domain
   and the gap register exactly one row per model gap -- bidirectionally, by id
   and by count -- across both the JSON and CSV encodings, and every referenced
   identifier resolves to the canonical model.
3. **Markdown references.** Every required narrative section is present; every
   evidence source path is cited with a repository-relative path that resolves to
   the source inventory; every cited non-line anchor is present; and the report
   never expands a prerequisite gate into an OS-substrate claim.
4. **Digests.** Every artifact digest recorded in ``assessment.manifest.json``
   matches the exact on-disk bytes (and byte size), and the manifest never
   digests itself.
5. **Revision binding.** The revision / worktree fingerprint recorded in the
   manifest matches the revision embedded in ``assessment.json``.
6. **Status / wording.** OS-substrate domains without direct implementation
   evidence stay at maturity 0, the non-additive statement is present, and the
   prerequisite-gate scope statement is present (a passing prerequisite gate is
   never expanded into an OS claim).
7. **Trust assumptions.** Trust assumptions are recorded, and no evidence record
   drops a recorded limitation/trust assumption.
8. **Requirement coverage.** All fifteen requirements are represented by the
   published artifact set.
9. **Self-consistency.** The model's own embedded validation state must itself be
   valid (a report that failed its own publish gate can never be final-valid).

The public entry point is :func:`validate_final_report`, which returns a
:class:`FinalValidationResult`. On success the validator result is recorded
(``valid=True`` with an empty finding list); on any finding the report is marked
NOT valid and every finding names the governing requirement(s) and the offending
object(s). This module is purely additive: it reads artifacts and reports; it
never touches product code, the committed deliverable, tasks metadata, or any
test.
"""

from __future__ import annotations

import csv
import io
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

from jsonschema import Draft202012Validator
from jsonschema.exceptions import SchemaError

from .catalog import NON_ADDITIVE_MATURITY_STATEMENT
from .json_renderer import (
    ASSESSMENT_JSON_ARTIFACT_NAME,
    ASSESSMENT_SCHEMA_ARTIFACT_NAME,
)
from .manifest import MANIFEST_ARTIFACT_NAME, artifact_digest
from .markdown_renderer import ARTIFACT_NAME as MARKDOWN_ARTIFACT_NAME
from .table_renderer import (
    CAPABILITY_MATRIX_COLUMNS,
    CAPABILITY_MATRIX_CSV,
    CAPABILITY_MATRIX_JSON,
    GAP_REGISTER_COLUMNS,
    GAP_REGISTER_CSV,
    GAP_REGISTER_JSON,
)

# --------------------------------------------------------------------------- #
# Fail-closed finding codes (FV-* = final validation).                         #
# --------------------------------------------------------------------------- #
FV_ARTIFACT_MISSING = "FV-ARTIFACT-MISSING"
FV_ARTIFACT_CORRUPT = "FV-ARTIFACT-CORRUPT"
FV_SCHEMA_INVALID = "FV-SCHEMA-INVALID"
FV_SCHEMA_VALIDATION = "FV-SCHEMA-VALIDATION"
FV_MODEL_NOT_VALID = "FV-MODEL-NOT-VALID"

FV_TABLE_COUNT = "FV-TABLE-COUNT"
FV_TABLE_MISSING_ROW = "FV-TABLE-MISSING-ROW"
FV_TABLE_FOREIGN_ROW = "FV-TABLE-FOREIGN-ROW"
FV_TABLE_DUPLICATE_ROW = "FV-TABLE-DUPLICATE-ROW"
FV_TABLE_FOREIGN_REFERENCE = "FV-TABLE-FOREIGN-REFERENCE"
FV_TABLE_CSV_JSON_MISMATCH = "FV-TABLE-CSV-JSON-MISMATCH"

FV_MD_SECTION_MISSING = "FV-MD-SECTION-MISSING"
FV_MD_STATEMENT_MISSING = "FV-MD-STATEMENT-MISSING"
FV_MD_REFERENCE_UNRESOLVED = "FV-MD-REFERENCE-UNRESOLVED"
FV_MD_ANCHOR_MISSING = "FV-MD-ANCHOR-MISSING"
FV_MD_OS_CLAIM_EXPANSION = "FV-MD-OS-CLAIM-EXPANSION"

FV_DIGEST_MISMATCH = "FV-DIGEST-MISMATCH"
FV_DIGEST_SIZE_MISMATCH = "FV-DIGEST-SIZE-MISMATCH"
FV_MANIFEST_SELF_DIGEST = "FV-MANIFEST-SELF-DIGEST"
FV_MANIFEST_UNKNOWN_ARTIFACT = "FV-MANIFEST-UNKNOWN-ARTIFACT"
FV_REVISION_MISMATCH = "FV-REVISION-MISMATCH"

FV_SUBSTRATE_NONZERO = "FV-SUBSTRATE-NONZERO"
FV_TRUST_ASSUMPTIONS_MISSING = "FV-TRUST-ASSUMPTIONS-MISSING"
FV_REQUIREMENT_COVERAGE = "FV-REQUIREMENT-COVERAGE"

# --------------------------------------------------------------------------- #
# The complete, versioned deliverable set every final report must publish.     #
# --------------------------------------------------------------------------- #
REQUIRED_ARTIFACTS: tuple[str, ...] = (
    ASSESSMENT_JSON_ARTIFACT_NAME,
    ASSESSMENT_SCHEMA_ARTIFACT_NAME,
    CAPABILITY_MATRIX_CSV,
    CAPABILITY_MATRIX_JSON,
    GAP_REGISTER_CSV,
    GAP_REGISTER_JSON,
    MARKDOWN_ARTIFACT_NAME,
    MANIFEST_ARTIFACT_NAME,
)

# The four OS-substrate target levels whose domains must stay at maturity 0
# without direct implementation evidence (Requirements 3.6, 10.6, 15.5).
_SUBSTRATE_LEVELS: frozenset[str] = frozenset(
    {
        "T2_Freestanding_Substrate",
        "T3_Boot_And_Kernel_Foundation",
        "T4_Isolated_Userspace_Platform",
        "T5_Operable_Universe_OS",
    }
)

# The 14 mandatory narrative section headings (Requirement 14.1). Matched as
# top-level (## N.) headings so nested "### Observed facts" subsections do not
# collide with the numbered contract.
_REQUIRED_SECTION_HEADINGS: tuple[str, ...] = (
    "## 1. Executive Conclusion",
    "## 2. Assessment Revision",
    "## 3. Source Inventory",
    "## 4. Current Baseline",
    "## 5. Target Model",
    "## 6. Maturity Rubric",
    "## 7. Capability Matrix",
    "## 8. Gap Register",
    "## 9. Hard-Gate Dependency Graph",
    "## 10. Prioritized Parallel Roadmap",
    "## 11. Evidence Conflicts",
    "## 12. Trust Assumptions",
    "## 13. Non-Claims",
    "## 14. Unvalidated / Unexecuted Evidence",
)

# The standing prerequisite-gate scope statement the report must carry so a
# passing prerequisite gate is never expanded into an OS claim (Requirement
# 13.7). Matched case-insensitively as a stable fragment.
_PREREQUISITE_GATE_SCOPE_FRAGMENT = "passing prerequisite gate proves only"

# Phrases that would only appear if a prerequisite gate were expanded into an
# OS-substrate claim (Requirement 13.7 / Property 13). None of these appear in a
# correct report; any match is a fail-closed OS-claim-expansion finding.
_OS_CLAIM_EXPANSION_PATTERNS: tuple[str, ...] = (
    r"proves (?:a |an )?(?:bootable|linked image|linked elf|boot execution|"
    r"kernel|freestanding runtime)",
    r"(?:relocatable[- ]object|et_rel|primitive object)[^.\n]{0,80}"
    r"(?:is a bootable|proves .*bootable|linked image|boot execution|"
    r"is a kernel|proves .*kernel)",
    r"t[2-5]_[a-z_]+ (?:is|are) achieved",
)

# The revision fields the manifest must reproduce from assessment.json's
# embedded revision (Requirement 1.1, 14.7). Left is the manifest key, right is
# the assessment.json revision key.
_REVISION_FIELD_PAIRS: tuple[tuple[str, str], ...] = (
    ("repositoryRootId", "repositoryRootId"),
    ("commitId", "commitId"),
    ("fingerprintAlgorithm", "fingerprintAlgorithm"),
    ("worktreeFingerprint", "worktreeFingerprint"),
    ("trackedDiffHash", "trackedDiffHash"),
    ("untrackedPathSetHash", "untrackedPathSetHash"),
)


@dataclass(frozen=True, slots=True)
class FinalFinding:
    """A single fail-closed finding with governing requirement + object refs."""

    code: str
    requirement_refs: tuple[str, ...]
    object_refs: tuple[str, ...] = ()
    detail: str = ""

    def __post_init__(self) -> None:
        if not isinstance(self.code, str) or not self.code.strip():
            raise ValueError("finding code must be a non-empty string")
        object.__setattr__(
            self, "requirement_refs", tuple(str(ref) for ref in self.requirement_refs)
        )
        object.__setattr__(
            self, "object_refs", tuple(str(ref) for ref in self.object_refs)
        )


@dataclass(frozen=True, slots=True)
class FinalValidationResult:
    """The single valid / invalid decision over a published artifact set.

    ``valid`` is true only when *no* finding was produced. ``findings`` carries
    every fail-closed finding (each with requirement + object refs). ``valid`` is
    the recorded validator result the final report is trusted by (Requirement
    9.7): a report is trusted only when this is true.
    """

    valid: bool
    findings: tuple[FinalFinding, ...] = ()

    def codes(self) -> frozenset[str]:
        return frozenset(finding.code for finding in self.findings)

    def requirement_refs(self) -> frozenset[str]:
        refs: set[str] = set()
        for finding in self.findings:
            refs.update(finding.requirement_refs)
        return frozenset(refs)


class _FindingSink:
    """Accumulates fail-closed findings during a single validation pass."""

    def __init__(self) -> None:
        self._findings: list[FinalFinding] = []

    def add(
        self,
        code: str,
        requirement_refs: Iterable[str],
        object_refs: Iterable[object] = (),
        detail: str = "",
    ) -> None:
        self._findings.append(
            FinalFinding(
                code=code,
                requirement_refs=tuple(str(ref) for ref in requirement_refs),
                object_refs=tuple(str(ref) for ref in object_refs),
                detail=detail,
            )
        )

    def result(self) -> FinalValidationResult:
        findings = tuple(
            sorted(
                self._findings,
                key=lambda f: (f.code, f.requirement_refs, f.object_refs),
            )
        )
        return FinalValidationResult(valid=not findings, findings=findings)


# --------------------------------------------------------------------------- #
# Public entry point.                                                          #
# --------------------------------------------------------------------------- #
def validate_final_report(artifacts_dir: str | Path) -> FinalValidationResult:
    """Validate the published assessment artifact set in ``artifacts_dir``.

    Loads every artifact from disk and fails closed on any inconsistency,
    returning a :class:`FinalValidationResult` whose ``valid`` flag is true only
    when the whole set is internally consistent and faithfully projects the
    canonical model embedded in ``assessment.json`` (Requirements 9.6, 9.7, 13.7,
    14.1-14.7). Never mutates or regenerates anything.
    """

    directory = Path(artifacts_dir)
    sink = _FindingSink()

    raw = _load_artifacts(directory, sink)
    # Presence failures are fatal to every downstream check; report and stop.
    if any(name not in raw for name in REQUIRED_ARTIFACTS):
        return sink.result()

    document = _parse_json(raw[ASSESSMENT_JSON_ARTIFACT_NAME], ASSESSMENT_JSON_ARTIFACT_NAME, sink)
    schema = _parse_json(raw[ASSESSMENT_SCHEMA_ARTIFACT_NAME], ASSESSMENT_SCHEMA_ARTIFACT_NAME, sink)
    matrix_json = _parse_json(raw[CAPABILITY_MATRIX_JSON], CAPABILITY_MATRIX_JSON, sink)
    gaps_json = _parse_json(raw[GAP_REGISTER_JSON], GAP_REGISTER_JSON, sink)
    manifest = _parse_json(raw[MANIFEST_ARTIFACT_NAME], MANIFEST_ARTIFACT_NAME, sink)
    markdown = raw[MARKDOWN_ARTIFACT_NAME].decode("utf-8", errors="replace")

    # Schema validation always runs (even before other checks) so a structurally
    # broken assessment.json is caught first.
    if document is not None and schema is not None:
        _check_schema(document, schema, sink)

    # The embedded canonical model (assessment.json -> "assessment").
    model: Mapping[str, Any] | None = None
    if isinstance(document, Mapping):
        candidate = document.get("assessment")
        if isinstance(candidate, Mapping):
            model = candidate
        else:
            sink.add(
                FV_ARTIFACT_CORRUPT,
                ("14.1",),
                (ASSESSMENT_JSON_ARTIFACT_NAME,),
                "assessment.json has no embedded canonical model",
            )

    if model is not None:
        _check_self_validation(document, sink)
        _check_table_parity(model, matrix_json, gaps_json, raw, sink)
        _check_markdown(model, markdown, sink)
        _check_status_and_wording(model, markdown, sink)
        _check_trust_assumptions(model, sink)
        _check_requirement_coverage(model, markdown, sink)
        _check_revision_binding(model, manifest, sink)

    _check_manifest_digests(manifest, raw, sink)

    return sink.result()


# --------------------------------------------------------------------------- #
# Loading / parsing.                                                           #
# --------------------------------------------------------------------------- #
def _load_artifacts(directory: Path, sink: _FindingSink) -> dict[str, bytes]:
    raw: dict[str, bytes] = {}
    for name in REQUIRED_ARTIFACTS:
        path = directory / name
        try:
            raw[name] = path.read_bytes()
        except OSError:
            sink.add(FV_ARTIFACT_MISSING, ("14.1",), (name,), f"missing artifact {name}")
    return raw


def _parse_json(content: bytes, name: str, sink: _FindingSink) -> Any:
    try:
        return json.loads(content.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as error:
        sink.add(FV_ARTIFACT_CORRUPT, ("14.1", "14.3"), (name,), f"invalid JSON: {error}")
        return None


# --------------------------------------------------------------------------- #
# 1. Schema.                                                                   #
# --------------------------------------------------------------------------- #
def _check_schema(document: Any, schema: Any, sink: _FindingSink) -> None:
    try:
        Draft202012Validator.check_schema(schema)
    except SchemaError as error:
        sink.add(
            FV_SCHEMA_INVALID,
            ("14.1", "14.3"),
            (ASSESSMENT_SCHEMA_ARTIFACT_NAME,),
            f"schema is not a valid Draft 2020-12 schema: {error.message}",
        )
        return

    validator = Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(document), key=lambda e: list(e.absolute_path))
    for error in errors:
        location = "/".join(str(part) for part in error.absolute_path) or "(root)"
        sink.add(
            FV_SCHEMA_VALIDATION,
            ("14.1", "14.3"),
            (ASSESSMENT_JSON_ARTIFACT_NAME, location),
            error.message,
        )


# --------------------------------------------------------------------------- #
# Self-consistency: the report must have passed its own publish gate.          #
# --------------------------------------------------------------------------- #
def _check_self_validation(document: Mapping[str, Any], sink: _FindingSink) -> None:
    validation = document.get("validation")
    if not isinstance(validation, Mapping) or validation.get("valid") is not True:
        findings = []
        if isinstance(validation, Mapping):
            findings = validation.get("findings") or []
        refs = sorted(
            {
                str(ref)
                for finding in findings
                if isinstance(finding, Mapping)
                for ref in (finding.get("requirementRefs") or [])
            }
        )
        sink.add(
            FV_MODEL_NOT_VALID,
            tuple(refs) if refs else ("9.7",),
            (ASSESSMENT_JSON_ARTIFACT_NAME,),
            "embedded model validation state is not valid",
        )


# --------------------------------------------------------------------------- #
# 2. Table parity (capability matrix + gap register, JSON and CSV).            #
# --------------------------------------------------------------------------- #
def _canonical_ids(model: Mapping[str, Any]) -> frozenset[str]:
    ids: set[str] = set()
    revision = model.get("revision")
    if isinstance(revision, Mapping) and revision.get("repositoryRootId"):
        ids.add(str(revision["repositoryRootId"]))
    for key in (
        "sourceInventory",
        "evidenceRecords",
        "conflicts",
        "domains",
        "gaps",
        "hardGates",
        "observedConclusions",
        "recommendations",
    ):
        for obj in model.get(key) or ():
            if isinstance(obj, Mapping) and obj.get("id") is not None:
                ids.add(str(obj["id"]))
    return frozenset(ids)


def _check_table_parity(
    model: Mapping[str, Any],
    matrix_json: Any,
    gaps_json: Any,
    raw: Mapping[str, bytes],
    sink: _FindingSink,
) -> None:
    canonical = _canonical_ids(model)
    domain_ids = frozenset(str(d["id"]) for d in model.get("domains") or () if isinstance(d, Mapping))
    gap_ids = frozenset(str(g["id"]) for g in model.get("gaps") or () if isinstance(g, Mapping))

    # -- capability matrix --------------------------------------------------- #
    _check_one_table(
        json_doc=matrix_json,
        json_name=CAPABILITY_MATRIX_JSON,
        csv_bytes=raw.get(CAPABILITY_MATRIX_CSV),
        csv_name=CAPABILITY_MATRIX_CSV,
        key_column="domainId",
        reference_columns=("evidenceRefs", "nextHardGate", "blockingDependencies"),
        expected_keys=domain_ids,
        canonical=canonical,
        sink=sink,
    )
    # -- gap register -------------------------------------------------------- #
    _check_one_table(
        json_doc=gaps_json,
        json_name=GAP_REGISTER_JSON,
        csv_bytes=raw.get(GAP_REGISTER_CSV),
        csv_name=GAP_REGISTER_CSV,
        key_column="gapId",
        # Only ``domainIds`` reference canonical objects; a gap's ``dependencies``
        # cite dependency-ordered Hard-Gate *candidates* from the roadmap, which
        # are intentionally broader than the materialized canonical Hard-Gate set,
        # so they are not part of the canonical foreign-reference check.
        reference_columns=("domainIds",),
        expected_keys=gap_ids,
        canonical=canonical,
        sink=sink,
    )


def _check_one_table(
    *,
    json_doc: Any,
    json_name: str,
    csv_bytes: bytes | None,
    csv_name: str,
    key_column: str,
    reference_columns: Sequence[str],
    expected_keys: frozenset[str],
    canonical: frozenset[str],
    sink: _FindingSink,
) -> None:
    rows = _table_rows(json_doc, json_name, sink)
    if rows is None:
        return

    keys = [str(row[key_column]) for row in rows if key_column in row]
    key_set = set(keys)

    # Count parity.
    if len(keys) != len(expected_keys):
        sink.add(
            FV_TABLE_COUNT,
            ("14.2",),
            (json_name,),
            f"{json_name}: {len(keys)} rows vs {len(expected_keys)} model objects",
        )

    # Forward: every canonical row-key object must have a row.
    missing = sorted(expected_keys - key_set)
    if missing:
        sink.add(FV_TABLE_MISSING_ROW, ("14.2",), (json_name, *missing))

    # Backward: no foreign rows, no duplicate rows.
    foreign = sorted(key_set - expected_keys)
    if foreign:
        sink.add(FV_TABLE_FOREIGN_ROW, ("14.2",), (json_name, *foreign))
    duplicates = sorted({k for k in keys if keys.count(k) > 1})
    if duplicates:
        sink.add(FV_TABLE_DUPLICATE_ROW, ("14.2",), (json_name, *duplicates))

    # Reference parity: no row may cite an identifier outside the model.
    references: list[str] = []
    for row in rows:
        for column in reference_columns:
            value = row.get(column)
            if isinstance(value, list):
                references.extend(str(item) for item in value)
            elif value not in (None, ""):
                references.append(str(value))
    foreign_refs = sorted({ref for ref in references if ref not in canonical})
    if foreign_refs:
        sink.add(FV_TABLE_FOREIGN_REFERENCE, ("14.2", "14.7"), (json_name, *foreign_refs))

    # CSV <-> JSON key parity: the two encodings must describe the same objects.
    if csv_bytes is not None:
        csv_keys = _csv_key_set(csv_bytes, csv_name, key_column, sink)
        if csv_keys is not None and csv_keys != key_set:
            only_csv = sorted(csv_keys - key_set)
            only_json = sorted(key_set - csv_keys)
            sink.add(
                FV_TABLE_CSV_JSON_MISMATCH,
                ("14.2", "14.3"),
                (csv_name, json_name, *only_csv, *only_json),
            )


def _table_rows(json_doc: Any, name: str, sink: _FindingSink) -> list[dict[str, Any]] | None:
    if not isinstance(json_doc, Mapping):
        sink.add(FV_ARTIFACT_CORRUPT, ("14.2",), (name,), "table is not an object")
        return None
    rows = json_doc.get("rows")
    if not isinstance(rows, list) or not all(isinstance(row, Mapping) for row in rows):
        sink.add(FV_ARTIFACT_CORRUPT, ("14.2",), (name,), "table has no rows array")
        return None
    return [dict(row) for row in rows]


def _csv_key_set(
    content: bytes, name: str, key_column: str, sink: _FindingSink
) -> set[str] | None:
    try:
        reader = csv.DictReader(io.StringIO(content.decode("utf-8")))
        keys = {str(row[key_column]) for row in reader if row.get(key_column) not in (None, "")}
    except (ValueError, UnicodeDecodeError, KeyError) as error:
        sink.add(FV_ARTIFACT_CORRUPT, ("14.2",), (name,), f"invalid CSV: {error}")
        return None
    return keys


# --------------------------------------------------------------------------- #
# 3. Markdown references and required sections.                                #
# --------------------------------------------------------------------------- #
def _check_markdown(model: Mapping[str, Any], markdown: str, sink: _FindingSink) -> None:
    # 3a. Required sections.
    for heading in _REQUIRED_SECTION_HEADINGS:
        if heading not in markdown:
            sink.add(FV_MD_SECTION_MISSING, ("14.1",), (heading,))

    # 3b. Every domain id and gap id must appear (the matrix/register cite them).
    for domain in model.get("domains") or ():
        if isinstance(domain, Mapping):
            did = str(domain.get("id"))
            if did not in markdown:
                sink.add(FV_MD_REFERENCE_UNRESOLVED, ("14.2", "14.4"), (did,), "domain id not referenced in markdown")
    for gap in model.get("gaps") or ():
        if isinstance(gap, Mapping):
            gid = str(gap.get("id"))
            if gid not in markdown:
                sink.add(FV_MD_REFERENCE_UNRESOLVED, ("14.3", "14.4"), (gid,), "gap id not referenced in markdown")

    # 3c. Repository-relative citation resolution (Requirement 14.4, 14.5).
    inventory_paths = {
        str(entry["path"])
        for entry in model.get("sourceInventory") or ()
        if isinstance(entry, Mapping) and entry.get("path") is not None
    }
    anchors_by_path: dict[str, set[str]] = {}
    for entry in model.get("sourceInventory") or ():
        if isinstance(entry, Mapping) and entry.get("path") is not None:
            anchors_by_path.setdefault(str(entry["path"]), set()).update(
                str(a) for a in (entry.get("stableAnchors") or [])
            )

    cited_paths = set(re.findall(r"`([^`\n]+)` \(", markdown))
    # Every cited path must resolve to a repository-relative inventory path.
    unresolved = sorted(cited_paths - inventory_paths)
    if unresolved:
        sink.add(
            FV_MD_REFERENCE_UNRESOLVED,
            ("14.4",),
            tuple(unresolved[:20]),
            "cited markdown path does not resolve to the source inventory",
        )

    # Every evidence source path must be cited, and non-line anchors present.
    for record in model.get("evidenceRecords") or ():
        if not isinstance(record, Mapping):
            continue
        rid = str(record.get("id"))
        source_path = str(record.get("sourcePath"))
        if source_path not in cited_paths:
            sink.add(
                FV_MD_REFERENCE_UNRESOLVED,
                ("14.4",),
                (rid, source_path),
                "evidence source path is not cited in the report",
            )
        location = record.get("location")
        if isinstance(location, Mapping) and location.get("kind") != "LineRange":
            anchors = anchors_by_path.get(source_path, set())
            value = str(location.get("value"))
            if anchors and value not in anchors:
                sink.add(
                    FV_MD_ANCHOR_MISSING,
                    ("14.4", "14.5"),
                    (rid, source_path, value),
                    "evidence anchor is not present in the source inventory",
                )


# --------------------------------------------------------------------------- #
# 6. Status / wording (never expand a prerequisite gate into an OS claim).     #
# --------------------------------------------------------------------------- #
def _check_status_and_wording(
    model: Mapping[str, Any], markdown: str, sink: _FindingSink
) -> None:
    # Non-additive statement present (Requirement 3.7, 13.7).
    lowered = markdown.lower()
    if NON_ADDITIVE_MATURITY_STATEMENT.lower() not in lowered and "non-additive" not in lowered:
        sink.add(
            FV_MD_STATEMENT_MISSING,
            ("3.7", "13.7"),
            (MARKDOWN_ARTIFACT_NAME,),
            "non-additive maturity statement is missing",
        )

    # Prerequisite-gate scope statement present (Requirement 13.7): a passing
    # prerequisite gate proves only its named scope; it is never expanded.
    if _PREREQUISITE_GATE_SCOPE_FRAGMENT not in lowered:
        sink.add(
            FV_MD_STATEMENT_MISSING,
            ("13.7",),
            (MARKDOWN_ARTIFACT_NAME,),
            "prerequisite-gate scope statement is missing",
        )

    # No prerequisite gate is expanded into an OS-substrate claim (Req 13.7).
    for pattern in _OS_CLAIM_EXPANSION_PATTERNS:
        match = re.search(pattern, lowered)
        if match:
            sink.add(
                FV_MD_OS_CLAIM_EXPANSION,
                ("13.7",),
                (MARKDOWN_ARTIFACT_NAME,),
                f"report expands a prerequisite gate into an OS claim: {match.group(0)!r}",
            )

    # Substrate domains without direct implementation evidence stay at maturity 0
    # (Requirement 3.6, 10.6, 15.5). Never expand a gate into a substrate score.
    domain_level = {
        str(d["id"]): str(d.get("targetLevel"))
        for d in model.get("domains") or ()
        if isinstance(d, Mapping) and d.get("id") is not None
    }
    for assessment in model.get("assessments") or ():
        if not isinstance(assessment, Mapping):
            continue
        did = str(assessment.get("domainId"))
        if domain_level.get(did) not in _SUBSTRATE_LEVELS:
            continue
        if assessment.get("evidenceIds"):
            continue
        raw_score = _as_int(assessment.get("rawScore"))
        eff_score = _as_int(assessment.get("effectiveScore"))
        if (raw_score or 0) > 0 or (eff_score or 0) > 0:
            sink.add(
                FV_SUBSTRATE_NONZERO,
                ("3.6", "10.6", "15.5"),
                (did,),
                "substrate domain without direct evidence has non-zero maturity",
            )


# --------------------------------------------------------------------------- #
# 7. Trust assumptions recorded.                                               #
# --------------------------------------------------------------------------- #
def _check_trust_assumptions(model: Mapping[str, Any], sink: _FindingSink) -> None:
    assumptions = model.get("assumptions") or ()
    if not assumptions:
        sink.add(
            FV_TRUST_ASSUMPTIONS_MISSING,
            ("9.5", "9.6"),
            (ASSESSMENT_JSON_ARTIFACT_NAME,),
            "no trust assumptions recorded",
        )


# --------------------------------------------------------------------------- #
# 8. All 15 requirements coverage represented.                                 #
# --------------------------------------------------------------------------- #
def _check_requirement_coverage(
    model: Mapping[str, Any], markdown: str, sink: _FindingSink
) -> None:
    domains = [d for d in (model.get("domains") or ()) if isinstance(d, Mapping)]
    gaps = [g for g in (model.get("gaps") or ()) if isinstance(g, Mapping)]
    evidence = [e for e in (model.get("evidenceRecords") or ()) if isinstance(e, Mapping)]
    assessments = [a for a in (model.get("assessments") or ()) if isinstance(a, Mapping)]
    conclusions = [c for c in (model.get("observedConclusions") or ()) if isinstance(c, Mapping)]

    domain_names = " ".join(str(d.get("name", "")).lower() for d in domains)
    domain_levels = {str(d.get("targetLevel")) for d in domains}
    evidence_statuses = {str(e.get("status")) for e in evidence}
    gap_categories = {str(g.get("primaryCategory")) for g in gaps}
    revision = model.get("revision") if isinstance(model.get("revision"), Mapping) else {}
    lowered = markdown.lower()

    def has_domain_keyword(*keywords: str) -> bool:
        return any(kw in domain_names for kw in keywords)

    def conclusions_cover_initial() -> bool:
        # Requirement 15: the seven initial evidence-backed conclusions.
        text = " ".join(str(c.get("text", "")).lower() for c in conclusions)
        fragments = (
            "t1_independent_language_platform",
            "unachieved",
            "maturity 0",
            "hosted adjacency",
            "shortest evidence-backed path",
        )
        return all(fragment in text for fragment in fragments)

    # Requirement -> structural predicate over the published artifact set.
    coverage: dict[str, bool] = {
        # R1: bound to repository evidence (revision + origins).
        "1": bool(str(revision.get("commitId", "")).strip())
        and bool(str(revision.get("worktreeFingerprint", "")).strip())
        and all(e.get("origin") for e in evidence)
        and len(evidence) > 0,
        # R2: six ordered target levels.
        "2": set(str(level) for level in (model.get("targetLevels") or ())) == {
            "T0_Hosted_Adjacency",
            "T1_Independent_Language_Platform",
            "T2_Freestanding_Substrate",
            "T3_Boot_And_Kernel_Foundation",
            "T4_Isolated_Userspace_Platform",
            "T5_Operable_Universe_OS",
        },
        # R3: maturity assessed with ordinal 0-5 scores and non-additive.
        "3": len(assessments) > 0
        and all(
            _in_range(a.get("rawScore")) and _in_range(a.get("effectiveScore"))
            for a in assessments
        )
        and "non-additive" in lowered,
        # R4: current baseline with multiple distinct evidence statuses.
        "4": len(evidence_statuses - {"None"}) >= 2,
        # R5: language semantics/type-system gaps.
        "5": "Language_Gap" in gap_categories,
        # R6: memory, ownership, concurrency, safety.
        "6": has_domain_keyword("memory", "safety", "concurren", "ownership", "unsafe"),
        # R7: FFI/ABI/compilation/linking/backend.
        "7": has_domain_keyword("abi", "backend", "linker", "compil"),
        # R8: runtime, standard library, package system.
        "8": has_domain_keyword("runtime", "library", "package", "std"),
        # R9: debugging, observability, security, reliability + trust assumptions.
        "9": has_domain_keyword("observab", "security", "diagnostic", "reliab")
        and bool(model.get("assumptions")),
        # R10: hardware, drivers, kernel, userspace substrate present at T3/T4.
        "10": has_domain_keyword("kernel", "driver", "userspace", "interrupt", "scheduler", "syscall")
        and {"T3_Boot_And_Kernel_Foundation", "T4_Isolated_Userspace_Platform"} <= domain_levels,
        # R11: application platform / ecosystem / release engineering.
        "11": "Ecosystem_Gap" in gap_categories,
        # R12: gap classification (all four primary categories represented).
        "12": {"Language_Gap", "Implementation_Gap", "Verification_Gap", "Ecosystem_Gap"}
        <= gap_categories,
        # R13: claims/uncertainty/conflicts (non-claims + prereq-gate scope).
        "13": bool(model.get("nonClaims"))
        and _PREREQUISITE_GATE_SCOPE_FRAGMENT in lowered,
        # R14: traceable outputs (all required sections present).
        "14": all(heading in markdown for heading in _REQUIRED_SECTION_HEADINGS),
        # R15: initial evidence-backed distance conclusion.
        "15": len(conclusions) > 0 and conclusions_cover_initial(),
    }

    for requirement in (str(index) for index in range(1, 16)):
        if not coverage.get(requirement, False):
            sink.add(
                FV_REQUIREMENT_COVERAGE,
                (requirement,),
                (f"requirement.{requirement}",),
                f"requirement {requirement} is not represented in the published report",
            )


# --------------------------------------------------------------------------- #
# 4. Manifest digests match on-disk bytes.                                     #
# --------------------------------------------------------------------------- #
def _check_manifest_digests(
    manifest: Any, raw: Mapping[str, bytes], sink: _FindingSink
) -> None:
    if not isinstance(manifest, Mapping):
        return  # already reported corrupt.
    entries = manifest.get("artifacts")
    if not isinstance(entries, list):
        sink.add(FV_ARTIFACT_CORRUPT, ("14.7",), (MANIFEST_ARTIFACT_NAME,), "manifest has no artifacts list")
        return

    for entry in entries:
        if not isinstance(entry, Mapping):
            continue
        name = str(entry.get("name"))
        if name == MANIFEST_ARTIFACT_NAME:
            sink.add(
                FV_MANIFEST_SELF_DIGEST,
                ("14.7",),
                (MANIFEST_ARTIFACT_NAME,),
                "manifest must never digest itself",
            )
            continue
        content = raw.get(name)
        if content is None:
            sink.add(
                FV_MANIFEST_UNKNOWN_ARTIFACT,
                ("14.7",),
                (name,),
                "manifest references an artifact that is not present",
            )
            continue
        expected_digest = str(entry.get("sha256"))
        actual_digest = artifact_digest(content)
        if expected_digest != actual_digest:
            sink.add(
                FV_DIGEST_MISMATCH,
                ("9.7", "14.7"),
                (name,),
                f"digest mismatch: manifest {expected_digest} vs on-disk {actual_digest}",
            )
        expected_size = entry.get("sizeBytes")
        if isinstance(expected_size, int) and expected_size != len(content):
            sink.add(
                FV_DIGEST_SIZE_MISMATCH,
                ("14.7",),
                (name,),
                f"size mismatch: manifest {expected_size} vs on-disk {len(content)}",
            )


# --------------------------------------------------------------------------- #
# 5. Revision / fingerprint binding.                                           #
# --------------------------------------------------------------------------- #
def _check_revision_binding(
    model: Mapping[str, Any], manifest: Any, sink: _FindingSink
) -> None:
    if not isinstance(manifest, Mapping):
        return
    manifest_revision = manifest.get("revision")
    model_revision = model.get("revision")
    if not isinstance(manifest_revision, Mapping) or not isinstance(model_revision, Mapping):
        sink.add(
            FV_REVISION_MISMATCH,
            ("1.1", "14.7"),
            (MANIFEST_ARTIFACT_NAME,),
            "manifest or model revision is missing",
        )
        return
    for manifest_key, model_key in _REVISION_FIELD_PAIRS:
        manifest_value = str(manifest_revision.get(manifest_key))
        model_value = str(model_revision.get(model_key))
        if manifest_value != model_value:
            sink.add(
                FV_REVISION_MISMATCH,
                ("1.1", "14.7"),
                (MANIFEST_ARTIFACT_NAME, manifest_key),
                f"{manifest_key}: manifest {manifest_value!r} vs assessment {model_value!r}",
            )


# --------------------------------------------------------------------------- #
# Small helpers.                                                               #
# --------------------------------------------------------------------------- #
def _as_int(value: Any) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _in_range(value: Any) -> bool:
    number = _as_int(value)
    return number is not None and 0 <= number <= 5
