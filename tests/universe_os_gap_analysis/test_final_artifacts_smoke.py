"""Final-deliverable smoke / parity regression test (Task 15.3).

This is a deterministic regression guard over the *committed, versioned*
assessment deliverable at ``tools/universe_os_gap_analysis/artifacts/`` -- the
artifact set produced by Task 15.1 (``assessment.json``,
``assessment.schema.json``, ``capability_matrix.{csv,json}``,
``gap_register.{csv,json}``, ``assessment.md``, ``assessment.manifest.json``) and
gated by the Task 15.2 final validator.

Unlike ``test_final_validation.py`` (which drives the validator against tampered
*copies*), this module reads only the static committed bytes and asserts the
whole deliverable is internally consistent, so silent drift of the published
report is caught. It is fully deterministic (no Hypothesis, no repo binding, no
network, no regeneration): it loads the frozen files on disk and cross-checks
them. It never mutates the committed artifacts.

The checks (Requirements 13.6, 13.7, 14.1-14.7, 15.1-15.7):

1. The committed deliverable passes the Task 15.2 final validator
   (:func:`validate_final_report` returns ``valid`` with no findings).
2. Bidirectional reference parity: every capability-matrix row <-> a domain in
   the embedded canonical model, every gap-register row <-> a gap; CSV <-> JSON
   key parity for both tables; the JSON reference-graph domain/gap nodes match
   the tables and are cited in ``assessment.md``.
3. Manifest digests match the on-disk bytes for all seven non-manifest
   artifacts, and the manifest binds the revision fingerprint embedded in
   ``assessment.json``.
4. Initial-conclusion / non-claim boundary: the narrative states the T1 / T2-T5
   unachieved conclusions, the language/tooling <= 2 cap, the substrate = 0
   conclusion, and the Hosted-Adjacency isolation conclusion; the non-claims
   section lists the kernel / driver / freestanding-runtime / bootable /
   userspace non-claims; every OS-substrate domain is 0 / Unsupported in both the
   JSON model and the capability matrix; and no prerequisite gate is expanded
   into an OS claim.
"""

from __future__ import annotations

import csv
import hashlib
import io
import json
import re
import unittest
from pathlib import Path

from tools.universe_os_gap_analysis.final_validation import (
    REQUIRED_ARTIFACTS,
    validate_final_report,
)
from tools.universe_os_gap_analysis.json_renderer import (
    ASSESSMENT_JSON_ARTIFACT_NAME,
)
from tools.universe_os_gap_analysis.manifest import MANIFEST_ARTIFACT_NAME
from tools.universe_os_gap_analysis.markdown_renderer import (
    ARTIFACT_NAME as MARKDOWN_ARTIFACT_NAME,
)
from tools.universe_os_gap_analysis.table_renderer import (
    CAPABILITY_MATRIX_CSV,
    CAPABILITY_MATRIX_JSON,
    GAP_REGISTER_CSV,
    GAP_REGISTER_JSON,
)

# The four OS-substrate target levels whose domains must remain at maturity 0 /
# Unsupported without direct implementation evidence (Requirements 3.6, 10.6,
# 15.3, 15.5).
_SUBSTRATE_LEVELS = frozenset(
    {
        "T2_Freestanding_Substrate",
        "T3_Boot_And_Kernel_Foundation",
        "T4_Isolated_Userspace_Platform",
        "T5_Operable_Universe_OS",
    }
)

# The fingerprint fields the manifest must reproduce from the embedded revision
# (Requirements 1.1, 14.7).
_REVISION_FINGERPRINT_FIELDS = (
    "repositoryRootId",
    "commitId",
    "fingerprintAlgorithm",
    "worktreeFingerprint",
    "trackedDiffHash",
    "untrackedPathSetHash",
)

# Initial evidence-backed conclusion fragments that must appear verbatim in the
# narrative's observed-facts section (Requirements 15.1-15.7).
_INITIAL_CONCLUSION_FRAGMENTS = (
    # 15.1 promising hosted foundation.
    "Nebula is a promising hosted language, compiler/tooling, backend-service, "
    "and thin-host application-core foundation.",
    # 15.2 T1 materially unachieved.
    "T1_Independent_Language_Platform is materially unachieved because "
    "production compilation still depends on generated C++ and external host "
    "tooling.",
    # 15.3 T2-T5 unachieved.
    "T2_Freestanding_Substrate through T5_Operable_Universe_OS are unachieved "
    "under current evidence.",
    # 15.4 language/tooling <= 2.
    "The strongest repository-local language/tooling capabilities have maturity "
    "no higher than 2 without cross-supported-host candidate evidence.",
    # 15.5 substrate == 0.
    "Freestanding runtime, linked or bootable chain, kernel subsystems, and "
    "Universe OS userspace have maturity 0 without direct implementation "
    "evidence.",
    # 15.6 hosted adjacency isolated.
    "Hosted Adjacency can reduce future application-porting effort but remains "
    "separate from every OS Substrate critical-path dependency and hard gate.",
)

# Non-claim fragments that must persist in the Non-Claims section (Requirement
# 13.6): kernel, driver, freestanding runtime, bootable image, userspace.
_NON_CLAIM_FRAGMENTS = (
    "No Nebula-owned kernel entry, panic path, or kernel synchronization exists.",
    "No UniverseOS userspace, system services, or product shell exists.",
    "No device drivers, driver model, DMA/IOMMU safety, or hardware "
    "qualification exists.",
    "No freestanding runtime (startup, allocation, panic runtime) exists",
    "No linked or bootable image, boot media, or QEMU execution proof exists",
)

# Phrases that would appear only if a prerequisite gate were expanded into an
# OS-substrate claim (Requirement 13.7). None must match a correct report.
_OS_CLAIM_EXPANSION_PATTERNS = (
    r"proves (?:a |an )?(?:bootable|linked image|linked elf|boot execution|"
    r"kernel|freestanding runtime)",
    r"t[2-5]_[a-z_]+ (?:is|are) achieved",
)


def _artifacts_dir() -> Path:
    """The committed, versioned deliverable directory (read-only)."""

    return (
        Path(__file__).resolve().parents[2]
        / "tools"
        / "universe_os_gap_analysis"
        / "artifacts"
    )


class FinalArtifactsSmokeTests(unittest.TestCase):
    """Deterministic regression guard over the committed deliverable."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.directory = _artifacts_dir()
        cls.raw = {
            name: (cls.directory / name).read_bytes() for name in REQUIRED_ARTIFACTS
        }
        cls.document = json.loads(cls.raw[ASSESSMENT_JSON_ARTIFACT_NAME])
        cls.model = cls.document["assessment"]
        cls.matrix = json.loads(cls.raw[CAPABILITY_MATRIX_JSON])
        cls.gaps = json.loads(cls.raw[GAP_REGISTER_JSON])
        cls.manifest = json.loads(cls.raw[MANIFEST_ARTIFACT_NAME])
        cls.markdown = cls.raw[MARKDOWN_ARTIFACT_NAME].decode("utf-8")

    # -- helpers ------------------------------------------------------------- #
    def _csv_keys(self, name: str, key_column: str) -> set[str]:
        reader = csv.DictReader(io.StringIO(self.raw[name].decode("utf-8")))
        return {row[key_column] for row in reader if row.get(key_column)}

    def _model_ids(self, key: str) -> set[str]:
        return {str(obj["id"]) for obj in self.model[key]}

    def _table_keys(self, table: dict, key_column: str) -> list[str]:
        return [str(row[key_column]) for row in table["rows"]]

    # -- 1. the committed deliverable passes the Task 15.2 final validator ---- #
    def test_committed_deliverable_passes_final_validator(self) -> None:
        result = validate_final_report(self.directory)
        self.assertTrue(
            result.valid,
            msg=f"unexpected findings: {[(f.code, f.detail) for f in result.findings]}",
        )
        self.assertEqual(result.findings, ())

    # -- 2a. capability matrix <-> model domain parity ----------------------- #
    def test_capability_matrix_row_per_domain(self) -> None:
        domain_ids = self._model_ids("domains")
        row_keys = self._table_keys(self.matrix, "domainId")

        self.assertEqual(len(row_keys), len(set(row_keys)), "duplicate matrix rows")
        self.assertEqual(
            set(row_keys),
            domain_ids,
            "capability matrix rows are not one-to-one with model domains",
        )
        self.assertEqual(len(row_keys), len(domain_ids))

    # -- 2b. gap register <-> model gap parity ------------------------------- #
    def test_gap_register_row_per_gap(self) -> None:
        gap_ids = self._model_ids("gaps")
        row_keys = self._table_keys(self.gaps, "gapId")

        self.assertEqual(len(row_keys), len(set(row_keys)), "duplicate gap rows")
        self.assertEqual(
            set(row_keys),
            gap_ids,
            "gap register rows are not one-to-one with model gaps",
        )
        self.assertEqual(len(row_keys), len(gap_ids))

    # -- 2c. CSV <-> JSON key parity for both tables ------------------------- #
    def test_csv_json_key_parity(self) -> None:
        self.assertEqual(
            self._csv_keys(CAPABILITY_MATRIX_CSV, "domainId"),
            set(self._table_keys(self.matrix, "domainId")),
            "capability matrix CSV/JSON key sets diverge",
        )
        self.assertEqual(
            self._csv_keys(GAP_REGISTER_CSV, "gapId"),
            set(self._table_keys(self.gaps, "gapId")),
            "gap register CSV/JSON key sets diverge",
        )

    # -- 2d. matrix evidence refs resolve to model evidence records ---------- #
    def test_matrix_evidence_refs_resolve_to_model(self) -> None:
        evidence_ids = self._model_ids("evidenceRecords")
        gate_ids = self._model_ids("hardGates")
        for row in self.matrix["rows"]:
            for ref in row["evidenceRefs"]:
                self.assertIn(
                    ref, evidence_ids, f"matrix row cites unknown evidence {ref}"
                )
            next_gate = row.get("nextHardGate")
            if next_gate:
                self.assertIn(
                    next_gate,
                    gate_ids,
                    f"matrix row cites unknown hard gate {next_gate}",
                )

    # -- 2e. gap register affected domains resolve to model domains ---------- #
    def test_gap_register_domains_resolve_to_model(self) -> None:
        domain_ids = self._model_ids("domains")
        for row in self.gaps["rows"]:
            for did in row["domainIds"]:
                self.assertIn(
                    did, domain_ids, f"gap row cites unknown domain {did}"
                )

    # -- 2f. reference graph nodes match the tables and cite the model ------- #
    def test_reference_graph_matches_tables(self) -> None:
        graph = self.document["referenceGraph"]
        domain_nodes = {
            n["id"] for n in graph["nodes"] if n["kind"] == "CapabilityDomain"
        }
        gap_nodes = {n["id"] for n in graph["nodes"] if n["kind"] == "GapEntry"}

        self.assertEqual(
            domain_nodes,
            set(self._table_keys(self.matrix, "domainId")),
            "reference-graph domain nodes diverge from the capability matrix",
        )
        self.assertEqual(
            gap_nodes,
            set(self._table_keys(self.gaps, "gapId")),
            "reference-graph gap nodes diverge from the gap register",
        )
        # And the model's own domain/gap ids agree with the graph nodes.
        self.assertEqual(domain_nodes, self._model_ids("domains"))
        self.assertEqual(gap_nodes, self._model_ids("gaps"))

    # -- 2g. every domain/gap id is cited in the narrative ------------------- #
    def test_domain_and_gap_ids_cited_in_markdown(self) -> None:
        for did in self._model_ids("domains"):
            self.assertIn(did, self.markdown, f"domain {did} not cited in markdown")
        for gid in self._model_ids("gaps"):
            self.assertIn(gid, self.markdown, f"gap {gid} not cited in markdown")

    # -- 3a. manifest digests match the on-disk bytes ------------------------ #
    def test_manifest_digests_match_disk_bytes(self) -> None:
        entries = {entry["name"]: entry for entry in self.manifest["artifacts"]}
        expected = {name for name in REQUIRED_ARTIFACTS if name != MANIFEST_ARTIFACT_NAME}

        self.assertEqual(
            set(entries),
            expected,
            "manifest does not describe exactly the seven non-manifest artifacts",
        )
        self.assertNotIn(
            MANIFEST_ARTIFACT_NAME, entries, "manifest must never digest itself"
        )
        for name, entry in entries.items():
            content = self.raw[name]
            self.assertEqual(
                entry["sha256"],
                hashlib.sha256(content).hexdigest(),
                f"manifest digest mismatch for {name}",
            )
            self.assertEqual(
                entry["sizeBytes"], len(content), f"manifest size mismatch for {name}"
            )

    # -- 3b. manifest binds the revision embedded in assessment.json --------- #
    def test_manifest_binds_embedded_revision(self) -> None:
        embedded = self.model["revision"]
        manifest_rev = self.manifest["revision"]
        for field in _REVISION_FINGERPRINT_FIELDS:
            self.assertEqual(
                manifest_rev[field],
                embedded[field],
                f"manifest revision field {field} does not match assessment.json",
            )

    # -- 4a. initial evidence-backed conclusions present --------------------- #
    def test_initial_conclusions_present(self) -> None:
        for fragment in _INITIAL_CONCLUSION_FRAGMENTS:
            self.assertIn(
                fragment,
                self.markdown,
                f"initial conclusion missing from narrative: {fragment!r}",
            )

    # -- 4b. non-claims boundary persists ------------------------------------ #
    def test_non_claims_boundary_present(self) -> None:
        self.assertIn("## 13. Non-Claims", self.markdown)
        non_claims_section = self.markdown.split("## 13. Non-Claims", 1)[1].split(
            "## 14.", 1
        )[0]
        for fragment in _NON_CLAIM_FRAGMENTS:
            self.assertIn(
                fragment,
                non_claims_section,
                f"non-claim missing from Non-Claims section: {fragment!r}",
            )

    # -- 4c. substrate domains are 0 / Unsupported in the model -------------- #
    def test_substrate_domains_zero_unsupported_in_model(self) -> None:
        level_by_domain = {
            str(d["id"]): str(d["targetLevel"]) for d in self.model["domains"]
        }
        substrate = {
            did for did, level in level_by_domain.items() if level in _SUBSTRATE_LEVELS
        }
        self.assertTrue(substrate, "expected at least one substrate domain")

        for assessment in self.model["assessments"]:
            did = str(assessment["domainId"])
            if did not in substrate:
                continue
            self.assertEqual(
                assessment["rawScore"], 0, f"substrate domain {did} raw score != 0"
            )
            self.assertEqual(
                assessment["effectiveScore"],
                0,
                f"substrate domain {did} effective score != 0",
            )
            self.assertEqual(
                assessment["evidenceStatus"],
                "Unsupported",
                f"substrate domain {did} is not Unsupported",
            )
            self.assertEqual(
                assessment["evidenceIds"],
                [],
                f"substrate domain {did} cites direct evidence",
            )

    # -- 4d. substrate domains are 0 / Unsupported in the matrix ------------- #
    def test_substrate_domains_zero_unsupported_in_matrix(self) -> None:
        substrate_rows = [
            row
            for row in self.matrix["rows"]
            if row["targetLevel"] in _SUBSTRATE_LEVELS
        ]
        self.assertTrue(substrate_rows, "expected substrate rows in the matrix")
        for row in substrate_rows:
            self.assertEqual(row["rawScore"], 0, row["domainId"])
            self.assertEqual(row["effectiveScore"], 0, row["domainId"])
            self.assertEqual(row["evidenceStatus"], "Unsupported", row["domainId"])
            self.assertEqual(row["evidenceRefs"], [], row["domainId"])

    # -- 4e. language/tooling capabilities never exceed maturity 2 ----------- #
    def test_language_tooling_cap_at_two(self) -> None:
        max_effective = max(
            row["effectiveScore"]
            for row in self.matrix["rows"]
            if row["targetLevel"] != "T0_Hosted_Adjacency"
        )
        self.assertLessEqual(
            max_effective, 2, "a non-adjacency capability exceeds maturity 2"
        )

    # -- 4f. no prerequisite gate is expanded into an OS claim --------------- #
    def test_no_os_claim_expansion(self) -> None:
        lowered = self.markdown.lower()
        for pattern in _OS_CLAIM_EXPANSION_PATTERNS:
            match = re.search(pattern, lowered)
            self.assertIsNone(
                match,
                f"narrative expands a prerequisite gate into an OS claim: "
                f"{match.group(0)!r}" if match else "",
            )


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
