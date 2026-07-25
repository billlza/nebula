"""Property 18: a boot hello does not imply an operating system.

This module holds the single Hypothesis property test for design Property 18
(Requirement 10.7). It exercises the *real* kernel/hardware/driver evaluator, the
*real* userspace/system-service/shell evaluator, the *real* application/ecosystem/
release (operations) evaluator, and the *real* boot evaluator -- no mocks and no
reimplementation of the components under test.

The property: for any baseline evidence bundle, adding *only* a passing QEMU
serial-hello record leaves the driver, interrupt, MMU, scheduler, syscall,
isolation, storage, networking, userspace, and operations gaps and maturity
scores completely unchanged (each remains absent / Maturity_Score 0).

The test is deliberately non-tautological in two ways:

* The baseline is adversarially generated (arbitrary statuses, kinds, origins,
  verification states) with benign, marker-free text, so the evaluators run
  their genuine marker-matching and claim-guard logic rather than a fixed input.
* The very same QEMU serial-hello record is shown to *genuinely* satisfy the boot
  evaluator's ``qemu-execution`` stage (it lifts that boot stage to a narrow
  experiment). That proves the record is a real, effective passing serial hello
  -- yet it moves none of the OS-substrate subsystems off Maturity_Score 0. A
  record that changed nothing anywhere would make the "does not imply an OS"
  invariant vacuous; this one measurably changes the boot proof and nothing else.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.evaluators.application_ecosystem_release import (
    evaluate_application_ecosystem_release,
)
from tools.universe_os_gap_analysis.evaluators.boot import evaluate_boot
from tools.universe_os_gap_analysis.evaluators.kernel_hardware_driver import (
    evaluate_kernel_hardware_driver,
)
from tools.universe_os_gap_analysis.evaluators.userspace_services_shell import (
    evaluate_userspace_services_shell,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    LocationKind,
    MaturityScore,
    RevisionOrigin,
    SourceLocation,
    VerificationState,
)

_REVISION_REF = reference("revision-property-18")

# The OS-substrate subsystems the property names explicitly. Each must remain
# absent / Maturity_Score 0 after a boot hello is added. Declared independently
# of the evaluator so the assertions are not derived from the implementation.
_NAMED_KERNEL_DRIVER_IDS: tuple[str, ...] = (
    "capability-driver-device-discovery",  # driver
    "capability-driver-storage-devices",  # storage
    "capability-driver-network-devices",  # networking
    "capability-hw-interrupts",  # interrupt
    "capability-hw-mmu",  # MMU
    "capability-kernel-scheduler",  # scheduler
    "capability-kernel-syscall-dispatch",  # syscall
    "capability-kernel-address-space-isolation",  # isolation
)

# A benign vocabulary that contains none of the kernel/driver/userspace/operations
# markers nor any boot/QEMU marker, so generated baseline records never match a
# named subsystem or the boot execution stage. This isolates the effect of the
# single QEMU record that the test later adds.
_BENIGN_WORDS: tuple[str, ...] = (
    "documentation",
    "roadmap",
    "future",
    "proposal",
    "review",
    "summary",
    "note",
    "context",
    "overview",
    "plan",
    "draft",
    "section",
    "paragraph",
    "background",
    "rationale",
    "appendix",
    "glossary",
)


@st.composite
def _record(draw: st.DrawFn) -> EvidenceRecord:
    """Draw an adversarial but marker-free baseline Evidence_Record."""

    words = draw(st.lists(st.sampled_from(_BENIGN_WORDS), min_size=1, max_size=5))
    claim = " ".join(words)
    key_words = draw(st.lists(st.sampled_from(_BENIGN_WORDS), min_size=1, max_size=3))
    claim_key = "note:" + "-".join(key_words)

    status = draw(st.sampled_from(tuple(EvidenceStatus)))
    kind = draw(st.sampled_from(tuple(EvidenceKind)))
    origin = draw(st.sampled_from(tuple(RevisionOrigin)))
    verification = draw(st.sampled_from(tuple(VerificationState)))
    salt = draw(st.integers(min_value=0, max_value=10_000))

    return EvidenceRecord(
        id=stable_id(
            "evidence", claim_key, claim, status.value, kind.value, origin.value, str(salt)
        ),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path="docs/universeos/notes.md",
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=verification,
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] += (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


def _qemu_serial_hello_record() -> EvidenceRecord:
    """A passing QEMU serial-hello record: a direct, present-permitted boot proof.

    Its wording matches the boot evaluator's ``qemu-execution`` markers ("qemu",
    "serial hello", "boot execution") but none of the kernel/driver/userspace/
    operations subsystem markers.
    """

    claim = "QEMU serial hello boot execution smoke passed and emitted expected output"
    return EvidenceRecord(
        id=stable_id("evidence", "qemu-serial-hello", claim),
        claim_key="execution:UOS-BOOT-005",
        claim=claim,
        status=EvidenceStatus.EXPERIMENTAL,
        source_path="docs/universeos/qemu_boot_hello.md",
        location=SourceLocation(kind=LocationKind.CASE_ID, value="UOS-BOOT-005"),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=EvidenceKind.SOURCE,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.VALIDATED,
    )


def _kernel_signature(result) -> tuple:
    """A stable (score, gap) signature over every kernel/hardware/driver subsystem."""

    drafts = tuple(
        (
            str(d.domain.id),
            d.has_direct_implementation,
            str(d.raw_maturity_score),
            str(d.gap_id),
        )
        for d in result.domain_drafts
    )
    gaps = tuple(sorted(str(g.id) for g in result.gaps))
    return (drafts, gaps)


def _userspace_signature(result) -> tuple:
    drafts = tuple(
        (
            str(d.domain.id),
            d.satisfied,
            str(d.maturity_score),
            str(d.gap_id) if d.gap_id is not None else None,
        )
        for d in result.domain_drafts
    )
    gaps = tuple(sorted(str(g.id) for g in result.gaps))
    return (drafts, gaps)


def _operations_signature(result) -> tuple:
    drafts = tuple(
        (
            str(d.domain.id),
            d.satisfied,
            str(d.observed_status),
            str(d.gap_id) if d.gap_id is not None else None,
        )
        for d in result.maturity_drafts()
    )
    gaps = tuple(sorted(str(g.id) for g in result.gaps))
    return (drafts, gaps)


# --------------------------------------------------------------------------- #
# Property 18                                                                 #
# --------------------------------------------------------------------------- #


# Feature: nebula-universe-os-gap-analysis, Property 18: A boot hello does not imply an operating system - for all assessment models, adding only a passing QEMU serial-hello record leaves driver, interrupt, MMU, scheduler, syscall, isolation, storage, networking, userspace, and operations gaps and maturity scores unchanged.
# **Validates: Requirements 10.7**
@given(records=st.lists(_record(), max_size=6, unique_by=lambda r: str(r.id)))
@settings(max_examples=200, deadline=None, print_blob=True)
def test_boot_hello_does_not_imply_an_operating_system(
    records: list[EvidenceRecord],
) -> None:
    """Requirement 10.7: a passing serial hello moves no OS-substrate subsystem.

    Baseline and baseline-plus-QEMU bundles are both fed to the real kernel,
    userspace, and operations evaluators; their subsystem/gap signatures must be
    identical, and every named subsystem must remain absent / Maturity_Score 0.
    The same QEMU record is separately shown to genuinely satisfy the boot
    execution stage (non-tautology check).
    """

    baseline = _bundle(records)
    augmented = _bundle(records + [_qemu_serial_hello_record()])

    # -- 1. kernel / hardware / driver: interrupt, MMU, scheduler, syscall,
    #       isolation, storage, networking, and every driver subsystem. -------- #
    kernel_before = evaluate_kernel_hardware_driver(baseline)
    kernel_after = evaluate_kernel_hardware_driver(augmented)
    assert _kernel_signature(kernel_before) == _kernel_signature(kernel_after)

    for capability_id in _NAMED_KERNEL_DRIVER_IDS:
        draft = kernel_after.draft_for(capability_id)
        assert draft is not None, f"missing kernel/driver subsystem {capability_id}"
        assert draft.has_direct_implementation is False
        assert draft.raw_maturity_score is MaturityScore.ABSENT
        assert int(draft.raw_maturity_score) == 0
        # The subsystem still has its own dedicated implementation gap.
        assert kernel_after.gap_for(capability_id) is not None

    # -- 2. userspace: every T4 userspace/service/shell capability stays 0. ---- #
    userspace_before = evaluate_userspace_services_shell(baseline)
    userspace_after = evaluate_userspace_services_shell(augmented)
    assert _userspace_signature(userspace_before) == _userspace_signature(userspace_after)
    for draft in userspace_after.domain_drafts:
        assert draft.satisfied is False
        assert draft.maturity_score is MaturityScore.ABSENT

    # -- 3. operations: adding the boot hello changes no ecosystem/release
    #       maturity domain or gap (hosted-doc/release scope is a separate
    #       concern governed by Property 9, not by a boot execution proof). ---- #
    ops_before = evaluate_application_ecosystem_release(baseline)
    ops_after = evaluate_application_ecosystem_release(augmented)
    assert _operations_signature(ops_before) == _operations_signature(ops_after)

    # -- 4. non-tautology: the SAME QEMU record genuinely lifts the boot
    #       execution stage, proving it is an effective passing serial hello. -- #
    boot_before = evaluate_boot(baseline)
    boot_after = evaluate_boot(augmented)

    qemu_before = boot_before.assessment_for("qemu-execution")
    qemu_after = boot_after.assessment_for("qemu-execution")
    assert qemu_before is not None and qemu_after is not None
    # Baseline (marker-free) leaves the boot execution stage unsatisfied at 0 ...
    assert qemu_before.satisfied is False
    assert qemu_before.maturity is MaturityScore.ABSENT
    # ... while adding the QEMU serial hello genuinely satisfies that boot stage.
    assert qemu_after.satisfied is True
    assert qemu_after.maturity is MaturityScore.NARROW_EXPERIMENT
    assert qemu_after.supporting_evidence_ids != ()


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner; allow direct execution.
    test_boot_hello_does_not_imply_an_operating_system()
    print("Property 18 OK: a boot hello does not imply an operating system")
