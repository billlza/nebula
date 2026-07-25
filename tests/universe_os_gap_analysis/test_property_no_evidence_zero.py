"""Property-based test for the no-implementation-evidence floor (design Property 8).

This module owns the Hypothesis property test for design Property 8:

    No direct implementation evidence means zero.

It exercises the *real* Task 8.2 maturity assessor -- both
:func:`~tools.universe_os_gap_analysis.maturity.compute_raw_score` and the full
:func:`~tools.universe_os_gap_analysis.maturity.assess_domains` topological
capping over a *real* Task 8.1 validated Hard-Gate graph
(:func:`~tools.universe_os_gap_analysis.hard_gate_graph.build_hard_gate_graph`).
No product code is edited and no mocks are used.

The property is that whenever a capability domain -- including every OS-substrate
domain (freestanding runtime, boot, kernel, drivers, userspace, operations) --
has *no direct implementation evidence*, both its raw and effective maturity are
fixed at ``0`` regardless of:

* plans (roadmap / RFC / specification / planned-status evidence);
* prerequisites and adjacent capabilities (blocking Hard-Gate dependencies whose
  own effective scores are freely high, 1..5);
* examples (hosted ``Example`` evidence, even quoting GA status); and
* every score-3+ signal (cross-host candidate contract, migration/rollback,
  release review, supported production, mature ecosystem).

To keep the test from being a tautology it does three things:

1. it *re-derives* the "direct implementation evidence" predicate from the
   requirement text (direct kind AND implemented status) rather than importing
   the assessor's private sets, and asserts every generated domain genuinely
   lacks such evidence -- so the premise is real, not assumed;
2. it drives the *real* ``compute_raw_score`` and ``assess_domains`` (with
   genuine adjacent/prerequisite gate scores) and confirms both stay ``0``; and
3. it proves *liveness*: injecting a single genuine implementation record into
   the very same domain lifts the real raw score above ``0``, and a
   deterministic OS-substrate domain with real evidence is recognised. So the
   ``0`` result is caused by the missing evidence, not by an inert assessor.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.hard_gate_graph import build_hard_gate_graph
from tools.universe_os_gap_analysis.identifiers import stable_id
from tools.universe_os_gap_analysis.maturity import (
    DomainClass,
    DomainMaturityInput,
    assess_domains,
    compute_raw_score,
)
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    HardGate,
    LocationKind,
    MaturityScore,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = stable_id("revision", "property-8")

# The four OS-substrate target levels named by the property (freestanding
# substrate, boot/kernel foundation, isolated userspace, operable OS). The
# property must hold for these just as for hosted/language levels, so the
# generator samples across all six and the deterministic checks pin these four.
_OS_SUBSTRATE_LEVELS: tuple[TargetLevel, ...] = (
    TargetLevel.T2_FREESTANDING_SUBSTRATE,
    TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
    TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
    TargetLevel.T5_OPERABLE_UNIVERSE_OS,
)

# Requirement 3.6 / 10.6 / 15.5 grounding, re-derived from the requirement text
# rather than imported from the assessor: a record is *direct implementation*
# evidence only if it is a direct implementation kind carrying an implemented
# (present-tense) status. Anything else -- a plan, spec, RFC, example, non-claim,
# release, workflow, or a direct kind that is only Planned/Unsupported/Unknown --
# is NOT direct implementation evidence and can never lift maturity above 0.
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)
_IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)


def _is_direct_implementation(record: EvidenceRecord) -> bool:
    """Independent re-derivation of the direct-implementation predicate."""

    return (
        record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
        and record.status in _IMPLEMENTED_STATUSES
    )


# --------------------------------------------------------------------------- #
# Record / gate builders                                                      #
# --------------------------------------------------------------------------- #


def _record(
    *,
    key: str,
    kind: EvidenceKind,
    status: EvidenceStatus,
    source_path: str,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", key, kind.value, status.value),
        claim_key=f"claim.{key}",
        claim=f"Claim for {key} ({kind.value}/{status.value}).",
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=key),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=kind,
        confidence=ConfidenceRating.LOW,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _gate(
    key: str,
    *,
    dependencies: tuple[str, ...] = (),
    score: MaturityScore = MaturityScore.ABSENT,
    target_level: TargetLevel = TargetLevel.T2_FREESTANDING_SUBSTRATE,
) -> HardGate:
    return HardGate(
        id=str(stable_id("gate", "prop8", key)),
        title=f"Gate {key}",
        target_level=target_level,
        status=EvidenceStatus.UNSUPPORTED,
        maturity_score=score,
        dependency_ids=tuple(str(stable_id("gate", "prop8", dep)) for dep in dependencies),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=(f"Acceptance evidence for {key}.",),
        non_claims=(),
        owner_area="Test Owner",
    )


def _gid(key: str) -> str:
    return str(stable_id("gate", "prop8", key))


def _did(key: str) -> str:
    return str(stable_id("domain", "prop8", key))


# --------------------------------------------------------------------------- #
# Generators: non-implementation evidence in every flavour named by Prop 8    #
# --------------------------------------------------------------------------- #

# Each branch is guaranteed by construction to NOT be direct implementation
# evidence (verified independently in the test via _is_direct_implementation),
# yet several are adversarial: an example or release quoting a GA status, or a
# direct kind whose status is only Planned/Unsupported/Unknown.
_NON_IMPL_CATEGORIES: tuple[str, ...] = (
    "plan_rfc",
    "specification",
    "roadmap_release",
    "hosted_example",
    "non_claim",
    "workflow",
    "test_definition",
    "direct_kind_not_implemented",
)

# Statuses that are NOT implemented (safe for any evidence kind).
_NON_IMPLEMENTED_STATUSES: tuple[EvidenceStatus, ...] = (
    EvidenceStatus.PLANNED,
    EvidenceStatus.UNSUPPORTED,
    EvidenceStatus.UNKNOWN,
)


@st.composite
def _non_implementation_record(draw: st.DrawFn) -> EvidenceRecord:
    category = draw(st.sampled_from(_NON_IMPL_CATEGORIES))
    unique = draw(st.from_regex(r"[a-z0-9]{6}", fullmatch=True))

    if category == "plan_rfc":
        kind, status = EvidenceKind.RFC, EvidenceStatus.PLANNED
        source_path = "docs/rfcs/future.md"
    elif category == "specification":
        kind = EvidenceKind.SPECIFICATION
        status = draw(st.sampled_from((EvidenceStatus.PLANNED, EvidenceStatus.UNKNOWN)))
        source_path = "spec/future_semantics.md"
    elif category == "roadmap_release":
        # A release/roadmap record: RELEASE is not a direct implementation kind,
        # so even a GA status cannot make it implementation evidence.
        kind = EvidenceKind.RELEASE
        status = draw(st.sampled_from(tuple(EvidenceStatus)))
        source_path = "ROADMAP.md"
    elif category == "hosted_example":
        # Adversarial: an Example that even claims a GA status is still not
        # implementation evidence for the domain.
        kind = EvidenceKind.EXAMPLE
        status = draw(st.sampled_from(tuple(EvidenceStatus)))
        source_path = "examples/demo/main.nb"
    elif category == "non_claim":
        kind = EvidenceKind.NON_CLAIM
        status = draw(st.sampled_from((EvidenceStatus.UNSUPPORTED, EvidenceStatus.UNKNOWN)))
        source_path = "docs/universeos/kernel_boundary.md"
    elif category == "workflow":
        kind = EvidenceKind.WORKFLOW
        status = draw(st.sampled_from(tuple(EvidenceStatus)))
        source_path = ".github/workflows/contract-tests.yml"
    elif category == "test_definition":
        kind = EvidenceKind.TEST_DEFINITION
        status = draw(st.sampled_from(tuple(EvidenceStatus)))
        source_path = "tests/README.md"
    else:  # direct_kind_not_implemented
        # A direct implementation *kind*, but only a non-implemented status, so
        # it is still not direct implementation evidence.
        kind = draw(st.sampled_from(tuple(_DIRECT_IMPLEMENTATION_KINDS)))
        status = draw(st.sampled_from(_NON_IMPLEMENTED_STATUSES))
        source_path = "src/planned_feature.nb"

    return _record(key=f"{category}:{unique}", kind=kind, status=status, source_path=source_path)


def _genuine_implementation_record(unique: str) -> EvidenceRecord:
    """A record that IS direct implementation evidence (for liveness checks)."""

    return _record(
        key=f"impl:{unique}",
        kind=EvidenceKind.SOURCE,
        status=EvidenceStatus.EXPERIMENTAL,
        source_path="src/real_feature.nb",
    )


# Feature: nebula-universe-os-gap-analysis, Property 8: No direct implementation evidence means zero - for every capability domain (including every OS-substrate domain), absent direct implementation evidence forces raw and effective maturity to 0 regardless of plans, prerequisites, examples, or adjacent capabilities.
# **Validates: Requirements 3.6, 10.6, 15.5**
@given(
    evidence=st.lists(
        _non_implementation_record(), max_size=6, unique_by=lambda r: str(r.id)
    ),
    target_level=st.sampled_from(tuple(TargetLevel)),
    domain_class=st.sampled_from(tuple(DomainClass)),
    dep_scores=st.lists(st.integers(min_value=1, max_value=5), max_size=6),
    cross_host_candidate_contract=st.booleans(),
    migration_rollback=st.booleans(),
    release_review=st.booleans(),
    supported_production=st.booleans(),
    mature_ecosystem=st.booleans(),
)
@settings(max_examples=200, deadline=None, print_blob=True)
def test_no_direct_evidence_forces_zero(
    evidence: list[EvidenceRecord],
    target_level: TargetLevel,
    domain_class: DomainClass,
    dep_scores: list[int],
    cross_host_candidate_contract: bool,
    migration_rollback: bool,
    release_review: bool,
    supported_production: bool,
    mature_ecosystem: bool,
) -> None:
    """Requirement 3.6/10.6/15.5: no implementation evidence => raw = effective = 0."""

    # (1) Premise grounding: every generated record genuinely lacks direct
    # implementation status, re-derived independently of the assessor.
    assert not any(_is_direct_implementation(record) for record in evidence)

    # Adjacent capabilities / prerequisites: blocking dependency gates whose own
    # effective scores are freely high (1..5). These represent satisfied
    # neighbours that must NOT be able to lift a zero-evidence domain.
    dep_gates = tuple(
        _gate(f"dep{i}", score=MaturityScore(score), target_level=target_level)
        for i, score in enumerate(dep_scores)
    )
    domain_gate = _gate(
        "g",
        dependencies=tuple(f"dep{i}" for i in range(len(dep_scores))),
        target_level=target_level,
    )
    graph = build_hard_gate_graph(dep_gates + (domain_gate,))

    inp = DomainMaturityInput(
        domain_id=_did("d"),
        gate_id=_gid("g"),
        direct_evidence=tuple(evidence),
        domain_class=domain_class,
        cross_host_candidate_contract=cross_host_candidate_contract,
        migration_rollback=migration_rollback,
        release_review=release_review,
        supported_production=supported_production,
        mature_ecosystem=mature_ecosystem,
    )

    # (2) The real raw-score computation returns 0 despite plans/examples/flags.
    assert compute_raw_score(inp) is MaturityScore.ABSENT

    # (2) The real topological assessor returns 0 for both raw and effective,
    # even with high-scoring blocking prerequisites in the validated graph.
    assessment = assess_domains((inp,), graph)
    result = assessment.result_for(_did("d"))
    assert result is not None
    assert result.raw_score is MaturityScore.ABSENT
    assert result.effective_score is MaturityScore.ABSENT

    # (3) Liveness / non-tautology: injecting a single genuine implementation
    # record into the SAME domain lifts the real raw score above 0. So the 0
    # result above is caused by the missing evidence, not an inert assessor.
    live_inp = DomainMaturityInput(
        domain_id=_did("d"),
        gate_id=_gid("g"),
        direct_evidence=tuple(evidence) + (_genuine_implementation_record("live"),),
        domain_class=domain_class,
        cross_host_candidate_contract=cross_host_candidate_contract,
        migration_rollback=migration_rollback,
        release_review=release_review,
        supported_production=supported_production,
        mature_ecosystem=mature_ecosystem,
    )
    assert int(compute_raw_score(live_inp)) >= int(MaturityScore.NARROW_EXPERIMENT)


# Feature: nebula-universe-os-gap-analysis, Property 8: No direct implementation evidence means zero - deterministic OS-substrate coverage: each OS-substrate target level is 0 without direct evidence, yet still recognised when genuine implementation evidence is present (liveness).
# **Validates: Requirements 3.6, 10.6, 15.5**
def test_os_substrate_domains_are_zero_without_evidence_but_live_with_it() -> None:
    """Each OS-substrate level stays at 0 with plans/examples/prereqs, but is live."""

    plans_and_examples = (
        _record(
            key="rfc",
            kind=EvidenceKind.RFC,
            status=EvidenceStatus.PLANNED,
            source_path="docs/rfcs/kernel.md",
        ),
        _record(
            key="example",
            kind=EvidenceKind.EXAMPLE,
            status=EvidenceStatus.COMPILER_TOOLING_GA,
            source_path="examples/boot/main.nb",
        ),
        _record(
            key="nonclaim",
            kind=EvidenceKind.NON_CLAIM,
            status=EvidenceStatus.UNSUPPORTED,
            source_path="docs/universeos/kernel_boundary.md",
        ),
    )

    for level in _OS_SUBSTRATE_LEVELS:
        # A satisfied, mature prerequisite gate (score 5) that the OS-substrate
        # domain depends on -- an adjacent capability that must not lift it.
        prereq = _gate(
            f"prereq-{level.value}",
            score=MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM,
            target_level=level,
        )
        substrate = _gate(
            f"substrate-{level.value}",
            dependencies=(f"prereq-{level.value}",),
            target_level=level,
        )
        graph = build_hard_gate_graph((prereq, substrate))

        empty_inp = DomainMaturityInput(
            domain_id=_did(f"os-{level.value}"),
            gate_id=_gid(f"substrate-{level.value}"),
            direct_evidence=plans_and_examples,
            # Every score-3+ signal set: still must not create maturity.
            cross_host_candidate_contract=True,
            migration_rollback=True,
            release_review=True,
            supported_production=True,
            mature_ecosystem=True,
        )
        result = assess_domains((empty_inp,), graph).result_for(_did(f"os-{level.value}"))
        assert result is not None, level
        assert result.raw_score is MaturityScore.ABSENT, level
        assert result.effective_score is MaturityScore.ABSENT, level

        # Liveness: a genuine implementation record for the same OS-substrate
        # domain is recognised (raw > 0), proving the domain can score at all.
        live_inp = DomainMaturityInput(
            domain_id=_did(f"os-{level.value}"),
            gate_id=_gid(f"substrate-{level.value}"),
            direct_evidence=plans_and_examples
            + (_genuine_implementation_record(f"os-{level.value}"),),
        )
        assert int(compute_raw_score(live_inp)) >= int(MaturityScore.NARROW_EXPERIMENT), level


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner, so allow the property
    # test to be executed directly as a fallback.
    test_no_direct_evidence_forces_zero()
    test_os_substrate_domains_are_zero_without_evidence_but_live_with_it()
    print("Property 8 OK: no direct implementation evidence means zero")
