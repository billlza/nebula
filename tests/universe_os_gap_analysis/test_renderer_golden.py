"""Golden-snapshot and fail-closed unit tests for the renderers (Task 11.5).

These are deterministic unit/golden tests (not Hypothesis property tests --
Property 22 is covered separately by Task 11.4). They exercise the real
:mod:`tools.universe_os_gap_analysis.json_renderer`,
:mod:`tools.universe_os_gap_analysis.table_renderer`,
:mod:`tools.universe_os_gap_analysis.markdown_renderer`, and the fail-closed
publish transaction in
:mod:`tools.universe_os_gap_analysis.model_builder` against the same fully
populated canonical model the validator tests build (no mocks).

They pin, and thereby protect against silent drift:

* the byte-for-byte narrative Markdown report -- executive conclusion, every
  required section heading, the ordered ``T0``-``T5`` target model, the ``0``-``5``
  maturity rubric rows, the Mermaid Hard-Gate block, stable path/anchor
  citations, and the non-additive / non-progress / non-schedule statement
  (Requirements 2.1-2.8, 3.7, 13.7, 14.1, 14.4, 14.5, 15.1-15.7);
* the ``Unknown`` evidence-status rendering and the lossless, winner-free
  conflict rendering (Requirements 1.5, 13.4, 14.7); and
* the invalid-model rejection path through
  :func:`~tools.universe_os_gap_analysis.model_builder.publish_assessment`,
  confirming that a model that fails validation causes *no* artifact to be
  written to the output directory (Requirements 9.7, 14.1).

The Markdown golden is a full deterministic byte pin stored under
``golden/assessment.md``. Because the renderer is deterministic and the model is
fixed, the golden can be regenerated at will (see ``_regenerate_golden`` below);
any intentional renderer change must be accompanied by a conscious regeneration,
which is exactly the drift signal these tests provide.
"""

from __future__ import annotations

import dataclasses
import json
import tempfile
import unittest
from pathlib import Path

from tools.universe_os_gap_analysis.json_renderer import (
    ASSESSMENT_JSON_ARTIFACT_NAME,
    render_assessment_json,
)
from tools.universe_os_gap_analysis.markdown_renderer import (
    ARTIFACT_NAME as MARKDOWN_ARTIFACT_NAME,
    markdown_report,
    render_markdown,
)
from tools.universe_os_gap_analysis.model_builder import (
    RPT_PUBLISH_INVALID_MODEL,
    publish_assessment,
)
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceConflict,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    VerificationState,
)
from tools.universe_os_gap_analysis.identifiers import StableId
from tools.universe_os_gap_analysis.table_renderer import (
    CAPABILITY_MATRIX_JSON,
    GAP_REGISTER_JSON,
    render_capability_matrix_json,
    render_gap_register_json,
)

# Reuse the fully-populated, internally consistent model the validator tests
# build, so the renderers are exercised against exactly what the pipeline emits.
from .test_validator import build_valid_model

_GOLDEN_DIR = Path(__file__).parent / "golden"
_MARKDOWN_GOLDEN = _GOLDEN_DIR / "assessment.md"


def _regenerate_golden() -> None:
    """Rewrite the Markdown golden from the current renderer output.

    Intentionally not a test. Run it explicitly (``python -c "from tests...
    import _regenerate_golden; _regenerate_golden()"``) after a deliberate,
    reviewed renderer change; never to make a failing test pass without review.
    """

    _GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    _MARKDOWN_GOLDEN.write_bytes(render_markdown(build_valid_model()).content)


class MarkdownGoldenTests(unittest.TestCase):
    """Byte-for-byte golden pin of the narrative Markdown artifact."""

    def setUp(self) -> None:
        self.model = build_valid_model()
        self.artifact = render_markdown(self.model)

    def test_markdown_matches_golden_bytes(self) -> None:
        self.assertTrue(
            _MARKDOWN_GOLDEN.exists(),
            msg=f"missing golden fixture: {_MARKDOWN_GOLDEN}",
        )
        expected = _MARKDOWN_GOLDEN.read_bytes()
        self.assertEqual(
            self.artifact.content,
            expected,
            msg=(
                "Markdown renderer output drifted from the golden fixture. If "
                "this change is intentional, regenerate golden/assessment.md via "
                "_regenerate_golden() and review the diff."
            ),
        )

    def test_golden_is_valid_utf8_and_matches_string_report(self) -> None:
        # The RenderedArtifact bytes are exactly the string report, UTF-8 encoded.
        self.assertEqual(
            self.artifact.content.decode("utf-8"),
            markdown_report(self.model),
        )
        self.assertEqual(self.artifact.name, MARKDOWN_ARTIFACT_NAME)


class MarkdownStructureInvariantTests(unittest.TestCase):
    """Pin the material structural invariants inside the golden report."""

    def setUp(self) -> None:
        self.report = _MARKDOWN_GOLDEN.read_text(encoding="utf-8")

    def test_executive_conclusion_content_pinned(self) -> None:
        start = self.report.index("## 1. Executive Conclusion")
        end = self.report.index("## 2. Assessment Revision")
        section = self.report[start:end]
        self.assertIn("### Observed facts", section)
        self.assertIn("### Recommendations", section)
        self.assertIn(
            "Nebula is a hosted language and tooling foundation.", section
        )
        # The observed conclusion cites a repository-relative path + stable anchor.
        self.assertIn('[ev-hosted: `README.md` (heading "Current Boundary")]', section)
        # The recommendation is separated and linked to its gap.
        self.assertIn(
            "Publish a compatibility policy before depending on the CLI.", section
        )
        self.assertIn("(related gaps: gap-hosted)", section)

    def test_all_required_section_headings_present_in_order(self) -> None:
        headings = (
            "## 1. Executive Conclusion",
            "## 2. Assessment Revision",
            "## 3. Source Inventory",
            "## 4. Current Baseline",
            "## 5. Target Model (T0-T5)",
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
        positions = []
        for heading in headings:
            self.assertIn(heading, self.report, msg=f"missing section: {heading}")
            positions.append(self.report.index(heading))
        self.assertEqual(
            positions, sorted(positions), msg="sections are out of order"
        )

    def test_target_model_lists_all_six_levels_in_order(self) -> None:
        start = self.report.index("## 5. Target Model (T0-T5)")
        end = self.report.index("## 6. Maturity Rubric")
        section = self.report[start:end]
        ordered_levels = (
            "T0_Hosted_Adjacency",
            "T1_Independent_Language_Platform",
            "T2_Freestanding_Substrate",
            "T3_Boot_And_Kernel_Foundation",
            "T4_Isolated_Userspace_Platform",
            "T5_Operable_Universe_OS",
        )
        level_positions = []
        for level in ordered_levels:
            self.assertIn(level, section, msg=f"missing target level {level}")
            level_positions.append(section.index(level))
        self.assertEqual(level_positions, sorted(level_positions))

    def test_maturity_rubric_pins_rows_zero_through_five(self) -> None:
        start = self.report.index("## 6. Maturity Rubric")
        end = self.report.index("## 7. Capability Matrix")
        rubric = self.report[start:end]
        expected_rows = (
            "| 0 | No implementation evidence. |",
            "| 1 | Narrow experimental implementation. |",
            "| 2 | Repeatable repository-local implementation. |",
            "| 3 | Candidate contract verified across supported hosts, "
            "with migration and rollback evidence. |",
            "| 4 | Supported production capability. |",
            "| 5 | Mature independent ecosystem capability. |",
        )
        for row in expected_rows:
            self.assertIn(row, rubric, msg=f"missing rubric row: {row}")

    def test_mermaid_hard_gate_block_pinned(self) -> None:
        start = self.report.index("## 9. Hard-Gate Dependency Graph")
        end = self.report.index("## 10. Prioritized Parallel Roadmap")
        section = self.report[start:end]
        self.assertIn("```mermaid", section)
        self.assertIn("flowchart LR", section)
        self.assertIn('g0["gate-hosted: Hosted CLI gate"]', section)
        self.assertIn('g1["gate-kernel: Kernel scheduler gate"]', section)
        # Kernel gate depends on the hosted gate: exactly one directed edge.
        self.assertIn("g0 --> g1", section)

    def test_stable_anchor_citations_pinned(self) -> None:
        # Material conclusions cite repository-relative paths + smallest anchors.
        self.assertIn('`README.md` (heading "Current Boundary")', self.report)
        self.assertIn('`spec/language_core.md` (heading "Overview")', self.report)

    def test_non_additive_non_progress_non_schedule_statement_pinned(self) -> None:
        self.assertIn(
            "Capability maturity scores are non-additive ordinal values",
            self.report,
        )
        self.assertIn("**not** a progress indicator", self.report)
        self.assertIn("**not** a schedule or effort estimate", self.report)
        # The rubric section repeats the disclaimer; it appears at least twice.
        self.assertGreaterEqual(self.report.count("non-additive"), 2)

    def test_prerequisite_gate_scope_statement_pinned(self) -> None:
        # Requirement 13.7: a passing prerequisite gate proves only the named
        # gate scope. The statement accompanies the non-additive disclaimer and
        # therefore appears at least twice (header + rubric).
        self.assertIn(
            "A passing prerequisite gate proves only that named gate's scope.",
            self.report,
        )
        self.assertGreaterEqual(
            self.report.count(
                "A passing prerequisite gate proves only that named gate's scope."
            ),
            2,
        )

    def test_observed_facts_recommendations_separation_statement_pinned(self) -> None:
        # Requirement 14.7: observed current facts are kept separate from
        # recommendations, and the report says so up front.
        self.assertIn(
            "Observed current facts and recommendations are kept in separate, "
            "clearly labelled sections throughout this report.",
            self.report,
        )

    def test_target_boundaries_pin_hosted_adjacency_vs_os_substrate(self) -> None:
        # Requirements 2.3-2.8: T0 is bounded to Hosted_Adjacency (and never
        # counts as OS substrate), while T1-T5 are OS_Substrate levels. Pin the
        # target-model table rows so the boundary classification cannot drift.
        start = self.report.index("## 5. Target Model (T0-T5)")
        end = self.report.index("## 6. Maturity Rubric")
        section = self.report[start:end]
        self.assertIn(
            "| T0_Hosted_Adjacency | Hosted adjacency | Hosted_Adjacency |",
            section,
        )
        for os_substrate_level in (
            "T1_Independent_Language_Platform",
            "T2_Freestanding_Substrate",
            "T3_Boot_And_Kernel_Foundation",
            "T4_Isolated_Userspace_Platform",
            "T5_Operable_Universe_OS",
        ):
            self.assertRegex(
                section,
                rf"\| {os_substrate_level} \|[^\n|]*\| OS_Substrate \|",
                msg=f"{os_substrate_level} must be bounded to OS_Substrate",
            )
        # The narrative reinforces that hosted adjacency never completes OS
        # substrate work and the six levels are strictly ordered.
        self.assertIn("strictly ordered", section)
        self.assertIn("never counts as OS substrate completion", section)

    def test_unvalidated_execution_disclosure_pinned(self) -> None:
        # Requirement 14.6: evidence inspected but not validated by execution is
        # disclosed as unexecuted and never presented as a passing result.
        start = self.report.index("## 14. Unvalidated / Unexecuted Evidence")
        section = self.report[start:]
        self.assertIn(
            "Sources inspected but not validated by execution at the bound "
            "revision. An unexecuted source is disclosed here and never "
            "presented as a passing result.",
            section,
        )
        # The disclosure table carries an execution-state column and lists the
        # NotRun sources rather than implying they passed.
        self.assertIn("| Entry | Path | Execution state | Detail |", section)
        self.assertIn("NotRun", section)


class UnknownStatusRenderingTests(unittest.TestCase):
    """Unknown-status evidence renders under its own status group."""

    def _model_with_unknown_record(self):
        model = build_valid_model()
        unknown = EvidenceRecord(
            id=StableId("ev-unknown"),
            claim_key="ev-unknown",
            claim="No verifiable path establishes freestanding runtime behavior.",
            status=EvidenceStatus.UNKNOWN,
            source_path="README.md",
            location=SourceLocation(
                kind=LocationKind.HEADING, value="Current Boundary"
            ),
            revision_ref="revision-test",
            origin=RevisionOrigin.COMMITTED_REVISION,
            evidence_kind=EvidenceKind.SOURCE,
            confidence=ConfidenceRating.LOW,
            scope=EvidenceScope(),
            limitations=(),
            trust_assumptions=(),
            verification_state=VerificationState.NOT_RUN,
        )
        return dataclasses.replace(
            model, evidence_records=(*model.evidence_records, unknown)
        )

    def test_unknown_status_group_and_claim_rendered(self) -> None:
        report = markdown_report(self._model_with_unknown_record())
        start = report.index("## 4. Current Baseline")
        end = report.index("## 5. Target Model (T0-T5)")
        baseline = report[start:end]
        # The Unknown status gets its own labelled group in the baseline.
        self.assertIn("### Unknown", baseline)
        self.assertIn(
            "No verifiable path establishes freestanding runtime behavior.",
            baseline,
        )
        self.assertIn("**ev-unknown**", baseline)

    def test_absent_status_groups_are_not_emitted(self) -> None:
        # The base model has no Planned/Experimental evidence, so those status
        # groups must not appear as headings in the baseline section.
        report = markdown_report(build_valid_model())
        start = report.index("## 4. Current Baseline")
        end = report.index("## 5. Target Model (T0-T5)")
        baseline = report[start:end]
        self.assertNotIn("### Planned", baseline)
        self.assertNotIn("### Experimental", baseline)
        self.assertNotIn("### Unknown", baseline)


class ConflictRenderingTests(unittest.TestCase):
    """Conflicts render losslessly with no inferred winner."""

    def test_nonblocking_conflict_rendered_winner_free(self) -> None:
        report = markdown_report(build_valid_model())
        start = report.index("## 11. Evidence Conflicts")
        end = report.index("## 12. Trust Assumptions")
        section = report[start:end]
        self.assertIn("no inferred winner", section)
        # All member records are listed losslessly, with values and locations.
        self.assertIn("**conflict-1** (non-blocking", section)
        self.assertIn("records ev-hosted, ev-spec", section)
        self.assertIn("winner: none.", section)

    def test_blocking_conflict_labelled(self) -> None:
        model = build_valid_model()
        blocking = dataclasses.replace(model.conflicts[0], blocking=True)
        report = markdown_report(dataclasses.replace(model, conflicts=(blocking,)))
        start = report.index("## 11. Evidence Conflicts")
        end = report.index("## 12. Trust Assumptions")
        section = report[start:end]
        self.assertIn("**conflict-1** (blocking", section)
        self.assertIn("winner: none.", section)

    def test_no_conflicts_message(self) -> None:
        model = build_valid_model()
        report = markdown_report(dataclasses.replace(model, conflicts=()))
        start = report.index("## 11. Evidence Conflicts")
        end = report.index("## 12. Trust Assumptions")
        section = report[start:end]
        self.assertIn("No evidence conflicts were recorded.", section)


class StructuredArtifactGoldenTests(unittest.TestCase):
    """Deterministic structural pins for the JSON and table renderers."""

    def test_assessment_json_is_deterministic_and_structured(self) -> None:
        model = build_valid_model()
        first = render_assessment_json(model)
        second = render_assessment_json(model)
        self.assertEqual(first.content, second.content)
        self.assertEqual(first.name, ASSESSMENT_JSON_ARTIFACT_NAME)

        document = json.loads(first.content.decode("utf-8"))
        # The structured output carries the full model, its reference graph, and
        # its fail-closed validation state.
        self.assertIn("assessment", document)
        self.assertIn("referenceGraph", document)
        self.assertIn("nodes", document["referenceGraph"])
        self.assertIn("edges", document["referenceGraph"])
        self.assertIn("valid", document["validation"])
        self.assertIsInstance(document["validation"]["findings"], list)

    def test_capability_matrix_has_exactly_one_row_per_domain(self) -> None:
        model = build_valid_model()
        artifact = render_capability_matrix_json(model)
        self.assertEqual(artifact.name, CAPABILITY_MATRIX_JSON)
        document = json.loads(artifact.content.decode("utf-8"))
        row_ids = [row["domainId"] for row in document["rows"]]
        self.assertEqual(sorted(row_ids), ["domain-hosted", "domain-kernel"])
        self.assertEqual(len(row_ids), len(set(row_ids)))

    def test_gap_register_has_exactly_one_row_per_gap(self) -> None:
        model = build_valid_model()
        artifact = render_gap_register_json(model)
        self.assertEqual(artifact.name, GAP_REGISTER_JSON)
        document = json.loads(artifact.content.decode("utf-8"))
        row_ids = [row["gapId"] for row in document["rows"]]
        self.assertEqual(row_ids, ["gap-hosted"])


class InvalidModelFailClosedTests(unittest.TestCase):
    """publish_assessment must write nothing when the model is invalid."""

    def _invalid_model(self):
        # Dropping the mandatory non-claims section makes the model invalid
        # (RPT-MISSING-SECTION) without disturbing anything a renderer needs.
        return dataclasses.replace(build_valid_model(), non_claims=())

    def test_invalid_model_rejected_before_any_write(self) -> None:
        invalid = self._invalid_model()
        with tempfile.TemporaryDirectory() as output_dir:
            result = publish_assessment(
                invalid, (render_markdown,), output_dir=output_dir
            )
            self.assertFalse(result.published)
            self.assertFalse(result.validation.valid)
            codes = {finding.code for finding in result.findings}
            self.assertIn(RPT_PUBLISH_INVALID_MODEL, codes)
            # No artifact was rendered or written; the output directory is empty.
            self.assertEqual(result.written_paths, ())
            self.assertEqual(list(Path(output_dir).iterdir()), [])

    def test_invalid_model_rejected_across_all_renderers(self) -> None:
        invalid = self._invalid_model()
        renderers = (
            render_assessment_json,
            render_capability_matrix_json,
            render_gap_register_json,
            render_markdown,
        )
        with tempfile.TemporaryDirectory() as output_dir:
            result = publish_assessment(invalid, renderers, output_dir=output_dir)
            self.assertFalse(result.published)
            self.assertEqual(result.artifacts, ())
            self.assertEqual(list(Path(output_dir).iterdir()), [])

    def test_valid_model_publishes_markdown_matching_golden(self) -> None:
        model = build_valid_model()
        renderers = (
            render_assessment_json,
            render_capability_matrix_json,
            render_gap_register_json,
            render_markdown,
        )
        with tempfile.TemporaryDirectory() as output_dir:
            result = publish_assessment(model, renderers, output_dir=output_dir)
            self.assertTrue(result.published, msg=f"findings: {result.findings}")
            written = {Path(path).name for path in result.written_paths}
            self.assertIn(MARKDOWN_ARTIFACT_NAME, written)
            self.assertIn(ASSESSMENT_JSON_ARTIFACT_NAME, written)
            published_md = (Path(output_dir) / MARKDOWN_ARTIFACT_NAME).read_bytes()
            self.assertEqual(published_md, _MARKDOWN_GOLDEN.read_bytes())


if __name__ == "__main__":
    unittest.main()
