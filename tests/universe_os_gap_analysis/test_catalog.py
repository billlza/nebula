from __future__ import annotations

import itertools
import unittest

from tools.universe_os_gap_analysis.catalog import (
    CAPABILITY_DEFINITIONS,
    INITIAL_CONCLUSIONS,
    INITIAL_CONCLUSION_OVERTURN_RULE,
    MATURITY_RUBRIC,
    NON_ADDITIVE_MATURITY_STATEMENT,
    SHORTEST_EVIDENCE_PATH_TEMPLATE,
    TARGET_LEVEL_DEFINITIONS,
    UNIVERSE_OS_DEFINITION,
    CapabilityBoundary,
    ConclusionEvidence,
    applicable_initial_conclusions,
    validate_catalog,
)
from tools.universe_os_gap_analysis.identifiers import StableId
from tools.universe_os_gap_analysis.models import MaturityScore, TargetLevel


class CatalogTests(unittest.TestCase):
    def test_universe_os_definition_and_target_order_are_fixed(self) -> None:
        for required_scope in (
            "boot chain", "freestanding runtime", "system ABI", "kernel",
            "hardware", "driver", "isolated userspace", "system services",
            "application lifecycle", "security", "observability", "update", "recovery",
        ):
            self.assertIn(required_scope, UNIVERSE_OS_DEFINITION)

        self.assertEqual(
            [item.level for item in TARGET_LEVEL_DEFINITIONS],
            list(TargetLevel),
        )
        self.assertEqual([item.order for item in TARGET_LEVEL_DEFINITIONS], list(range(6)))
        self.assertIs(
            TARGET_LEVEL_DEFINITIONS[0].boundary,
            CapabilityBoundary.HOSTED_ADJACENCY,
        )
        self.assertTrue(
            all(
                item.boundary is CapabilityBoundary.OS_SUBSTRATE
                for item in TARGET_LEVEL_DEFINITIONS[1:]
            )
        )
        self.assertIn("does not complete OS substrate", TARGET_LEVEL_DEFINITIONS[0].definition)

    def test_maturity_rubric_is_ordinal_zero_through_five_and_non_additive(self) -> None:
        self.assertEqual([item.score for item in MATURITY_RUBRIC], list(MaturityScore))
        self.assertIn("No implementation evidence", MATURITY_RUBRIC[0].meaning)
        self.assertIn("repository-local", MATURITY_RUBRIC[2].meaning)
        self.assertIn("supported hosts", MATURITY_RUBRIC[3].meaning)
        for forbidden_interpretation in ("non-additive", "summed", "averaged", "percentages", "schedule"):
            self.assertIn(forbidden_interpretation, NON_ADDITIVE_MATURITY_STATEMENT)

    def test_every_target_has_mandatory_capability_and_checklist_metadata(self) -> None:
        expected_checklist_counts = {
            TargetLevel.T0_HOSTED_ADJACENCY: 4,
            TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM: 6,
            TargetLevel.T2_FREESTANDING_SUBSTRATE: 6,
            TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION: 8,
            TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM: 7,
            TargetLevel.T5_OPERABLE_UNIVERSE_OS: 7,
        }
        all_check_ids: list[StableId] = []
        for target_level, expected_count in expected_checklist_counts.items():
            capabilities = [
                item for item in CAPABILITY_DEFINITIONS
                if item.target_level is target_level
            ]
            self.assertTrue(capabilities)
            self.assertTrue(all(item.mandatory_for_target for item in capabilities))
            self.assertEqual(sum(len(item.checklist) for item in capabilities), expected_count)
            for capability in capabilities:
                expected_boundary = (
                    CapabilityBoundary.HOSTED_ADJACENCY
                    if target_level is TargetLevel.T0_HOSTED_ADJACENCY
                    else CapabilityBoundary.OS_SUBSTRATE
                )
                self.assertIs(capability.boundary, expected_boundary)
                self.assertTrue(all(check.mandatory for check in capability.checklist))
                self.assertTrue(
                    all(check.capability_id == capability.id for check in capability.checklist)
                )
                all_check_ids.extend(check.id for check in capability.checklist)
        self.assertEqual(len(all_check_ids), len(set(all_check_ids)))

    def test_initial_conclusion_contract_covers_all_requirement_15_clauses(self) -> None:
        self.assertEqual(
            {item.requirement_ref for item in INITIAL_CONCLUSIONS},
            {f"15.{number}" for number in range(1, 8)},
        )
        combined = " ".join(item.text for item in INITIAL_CONCLUSIONS)
        for required_claim in (
            "promising hosted language", "generated C++", "external host tooling",
            "T2_Freestanding_Substrate through T5_Operable_Universe_OS",
            "no higher than 2", "maturity 0", "Hosted Adjacency",
            "separate from every OS Substrate", "shortest evidence-backed path",
        ):
            self.assertIn(required_claim, combined)
        self.assertIn("newer evidence", INITIAL_CONCLUSION_OVERTURN_RULE)
        self.assertIn("direct and verified", INITIAL_CONCLUSION_OVERTURN_RULE)

    def test_only_newer_direct_verified_accepted_same_scope_repository_evidence_overturns(self) -> None:
        conclusion = INITIAL_CONCLUSIONS[0]
        for contradicts, direct, verified, newer in itertools.product((False, True), repeat=4):
            evidence = ConclusionEvidence(
                conclusion_id=conclusion.id,
                contradicts=contradicts,
                direct=direct,
                verified=verified,
                newer_than_snapshot=newer,
            )
            applicable_ids = {
                item.id for item in applicable_initial_conclusions((evidence,))
            }
            expected_overturn = contradicts and direct and verified and newer
            self.assertEqual(conclusion.id not in applicable_ids, expected_overturn)

        required_qualifiers = ("repository_evidence", "accepted", "same_scope")
        for qualifier in required_qualifiers:
            values = {
                "repository_evidence": True,
                "accepted": True,
                "same_scope": True,
            }
            values[qualifier] = False
            evidence = ConclusionEvidence(
                conclusion_id=conclusion.id,
                contradicts=True,
                direct=True,
                verified=True,
                newer_than_snapshot=True,
                **values,
            )
            self.assertIn(
                conclusion.id,
                {item.id for item in applicable_initial_conclusions((evidence,))},
                msg=f"{qualifier}=False must preserve the snapshot conclusion",
            )

        with self.assertRaisesRegex(TypeError, "same_scope must be a bool"):
            ConclusionEvidence(
                conclusion_id=conclusion.id,
                contradicts=True,
                direct=True,
                verified=True,
                newer_than_snapshot=True,
                same_scope=1,  # type: ignore[arg-type]
            )

        with self.assertRaisesRegex(ValueError, "unknown initial conclusion"):
            applicable_initial_conclusions((
                ConclusionEvidence(
                    conclusion_id=StableId("conclusion-unknown"),
                    contradicts=True,
                    direct=True,
                    verified=True,
                    newer_than_snapshot=True,
                ),
            ))

    def test_shortest_evidence_path_keeps_pre_kernel_and_post_boot_gates_separate(self) -> None:
        nodes = {item.id: item for item in SHORTEST_EVIDENCE_PATH_TEMPLATE}
        required_ids = {
            "path-language-soundness", "path-system-abi", "path-independent-backend",
            "path-freestanding-runtime", "path-boot-toolchain", "path-primitive-object",
            "path-linked-elf", "path-boot-media", "path-qemu-execution",
            "path-memory-management", "path-interrupts", "path-scheduler",
            "path-syscall-capabilities", "path-drivers-dma", "path-storage",
            "path-networking", "path-process-isolation", "path-userspace",
            "path-update-recovery", "path-product-shell", "path-operable-universe-os",
        }
        self.assertEqual({str(item) for item in nodes}, required_ids)
        self.assertEqual(
            set(nodes[StableId("path-linked-elf")].dependency_ids),
            {
                StableId("path-freestanding-runtime"),
                StableId("path-boot-toolchain"),
                StableId("path-primitive-object"),
            },
        )
        self.assertEqual(
            set(nodes[StableId("path-scheduler")].dependency_ids),
            {StableId("path-memory-management"), StableId("path-interrupts")},
        )
        self.assertNotEqual(
            nodes[StableId("path-storage")].id,
            nodes[StableId("path-networking")].id,
        )
        self.assertNotEqual(
            nodes[StableId("path-update-recovery")].id,
            nodes[StableId("path-product-shell")].id,
        )
        validate_catalog()


if __name__ == "__main__":
    unittest.main()
