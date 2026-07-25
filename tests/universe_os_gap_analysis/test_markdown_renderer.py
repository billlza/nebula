"""Unit tests for the narrative Markdown renderer (Task 11.3).

These tests exercise Requirements 3.7, 13.7, 14.1, and 14.4-14.7 against the real
:mod:`tools.universe_os_gap_analysis.markdown_renderer` module and the real
canonical model the validator tests build (no mocks). They confirm the renderer:

* emits every required narrative section (Requirement 14.1);
* emits a Mermaid Hard-Gate dependency block;
* states the non-additive / non-progress / non-schedule disclaimer
  (Requirement 3.7, 13.7);
* keeps observed facts separate from recommendations (Requirement 14.7);
* cites repository-relative paths plus stable anchors for material conclusions
  (Requirement 14.4, 14.5);
* discloses inspected-but-unexecuted evidence (Requirement 14.6);
* renders deterministically (byte-for-byte identical across runs); and
* integrates with the fail-closed publish transaction as a ``Renderer`` in a
  dry run, introducing no foreign facts.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.markdown_renderer import (
    ARTIFACT_NAME,
    markdown_report,
    render_markdown,
)
from tools.universe_os_gap_analysis.model_builder import (
    RenderedArtifact,
    canonical_reference_ids,
    publish_assessment,
)

# Reuse the fully-populated, internally consistent model the validator tests
# build, so the renderer is exercised against exactly what the pipeline emits.
from .test_validator import build_valid_model


REQUIRED_SECTION_HEADINGS = (
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


class RequiredSectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.model = build_valid_model()
        self.report = markdown_report(self.model)

    def test_all_required_sections_present(self) -> None:
        for heading in REQUIRED_SECTION_HEADINGS:
            self.assertIn(heading, self.report, msg=f"missing section: {heading}")

    def test_target_model_lists_all_six_levels(self) -> None:
        for level in (
            "T0_Hosted_Adjacency",
            "T1_Independent_Language_Platform",
            "T2_Freestanding_Substrate",
            "T3_Boot_And_Kernel_Foundation",
            "T4_Isolated_Userspace_Platform",
            "T5_Operable_Universe_OS",
        ):
            self.assertIn(level, self.report)

    def test_maturity_rubric_covers_zero_through_five(self) -> None:
        rubric_index = self.report.index("## 6. Maturity Rubric")
        matrix_index = self.report.index("## 7. Capability Matrix")
        rubric = self.report[rubric_index:matrix_index]
        for score in range(6):
            self.assertIn(f"| {score} |", rubric)

    def test_mermaid_hard_gate_block_present(self) -> None:
        self.assertIn("```mermaid", self.report)
        self.assertIn("flowchart LR", self.report)
        # The Hard-Gate ids from the model appear as node labels.
        self.assertIn("gate-hosted", self.report)
        self.assertIn("gate-kernel", self.report)
        # The dependency edge (kernel depends on hosted) renders as an arrow.
        self.assertRegex(self.report, r"g\d+ --> g\d+")


class NonAdditiveDisclaimerTests(unittest.TestCase):
    def test_states_non_additive_non_progress_non_schedule(self) -> None:
        report = markdown_report(build_valid_model())
        self.assertIn("non-additive", report)
        self.assertIn("not** a progress", report)
        self.assertIn("schedule", report)


class FactsRecommendationsSeparationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.report = markdown_report(build_valid_model())

    def test_executive_conclusion_separates_facts_and_recommendations(self) -> None:
        start = self.report.index("## 1. Executive Conclusion")
        end = self.report.index("## 2. Assessment Revision")
        section = self.report[start:end]
        self.assertIn("### Observed facts", section)
        self.assertIn("### Recommendations", section)
        self.assertLess(
            section.index("### Observed facts"),
            section.index("### Recommendations"),
        )

    def test_gap_register_separates_facts_and_recommendations(self) -> None:
        start = self.report.index("## 8. Gap Register")
        end = self.report.index("## 9. Hard-Gate Dependency Graph")
        section = self.report[start:end]
        self.assertIn("### Observed facts", section)
        self.assertIn("### Recommendations", section)

    def test_roadmap_separates_facts_and_recommendations(self) -> None:
        start = self.report.index("## 10. Prioritized Parallel Roadmap")
        end = self.report.index("## 11. Evidence Conflicts")
        section = self.report[start:end]
        self.assertIn("### Observed facts", section)
        self.assertIn("### Recommendations", section)


class CitationTests(unittest.TestCase):
    def test_material_conclusions_cite_path_and_anchor(self) -> None:
        report = markdown_report(build_valid_model())
        # README evidence is cited with its repository-relative path and heading.
        self.assertIn("`README.md`", report)
        self.assertIn('heading "Current Boundary"', report)
        # Spec evidence is cited with its path and heading anchor too.
        self.assertIn("`spec/language_core.md`", report)

    def test_unvalidated_evidence_disclosed(self) -> None:
        report = markdown_report(build_valid_model())
        start = report.index("## 14. Unvalidated / Unexecuted Evidence")
        section = report[start:]
        # The validator model records NotRun inventory entries, so they appear.
        self.assertIn("NotRun", section)


class DeterminismTests(unittest.TestCase):
    def test_render_is_byte_for_byte_deterministic(self) -> None:
        model = build_valid_model()
        first = render_markdown(model)
        second = render_markdown(model)
        self.assertEqual(first.content, second.content)
        self.assertEqual(first.projected_ids, second.projected_ids)

    def test_artifact_name_and_type(self) -> None:
        artifact = render_markdown(build_valid_model())
        self.assertIsInstance(artifact, RenderedArtifact)
        self.assertEqual(artifact.name, ARTIFACT_NAME)
        self.assertTrue(artifact.content.endswith(b"\n"))

    def test_rejects_non_model_input(self) -> None:
        with self.assertRaises(TypeError):
            render_markdown(object())  # type: ignore[arg-type]


class ParityAndPublishTests(unittest.TestCase):
    def test_projected_ids_are_a_subset_of_canonical_ids(self) -> None:
        model = build_valid_model()
        artifact = render_markdown(model)
        canonical = canonical_reference_ids(model)
        foreign = artifact.projected_ids - canonical
        self.assertEqual(foreign, frozenset(), msg=f"foreign facts: {foreign}")

    def test_dry_run_publish_succeeds_with_markdown_renderer(self) -> None:
        model = build_valid_model()
        result = publish_assessment(model, (render_markdown,), output_dir=None)
        self.assertTrue(result.published, msg=f"findings: {result.findings}")
        self.assertEqual(result.findings, ())
        self.assertEqual(len(result.artifacts), 1)
        self.assertEqual(result.artifacts[0].name, ARTIFACT_NAME)
        self.assertEqual(result.written_paths, ())


if __name__ == "__main__":
    unittest.main()
