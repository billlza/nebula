from __future__ import annotations

from hypothesis import assume, given, settings, strategies as st

from tools.universe_os_gap_analysis.evidence import (
    ClaimInput,
    EvidenceCollector,
    decide_status,
)
from tools.universe_os_gap_analysis.identifiers import reference
from tools.universe_os_gap_analysis.models import (
    EvidenceKind,
    EvidenceStatus,
    RevisionOrigin,
    VerificationState,
)

# The set of present-tense/current statuses that assert a capability exists now
# and therefore may carry maturity credit. Plan-only evidence must never land in
# this set: a plan grants no implemented status and no maturity (maturity 0).
IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)

# Only these two statuses grant no maturity credit and carry no present-tense
# implementation assertion; a compliant classifier for plan-only prose must
# always resolve to one of them (Planned with a path, Unknown when pathless).
NON_IMPLEMENTATION_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {EvidenceStatus.PLANNED, EvidenceStatus.UNKNOWN}
)

# Future-work evidence kinds that only ever prove a plan exists: roadmap prose
# and other future specification text, RFC proposals, proposed/not-yet-run test
# definitions, and planned-gate source mappings.
FUTURE_PROSE_KINDS = st.sampled_from(
    [
        EvidenceKind.SPECIFICATION,  # roadmap / future prose
        EvidenceKind.RFC,  # RFC proposal
        EvidenceKind.TEST_DEFINITION,  # proposed / future test
        EvidenceKind.SOURCE,  # planned-gate source mapping / future prose
    ]
)

# Adversarial caller-proposed statuses. Crucially this includes the implemented
# tiers: a caller (or a buggy upstream adapter) may *propose* GA/preview/
# experimental, and the classifier must still refuse to credit plan-only prose
# as implementation. Unsupported is excluded because it is illegal without an
# explicit negative/audited signal and is a separate polarity from "planned".
ADVERSARIAL_PROPOSED_STATUS = st.sampled_from(
    [
        None,
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
        EvidenceStatus.PLANNED,
        EvidenceStatus.UNKNOWN,
    ]
)

# Future-tense wording fragments: plan evidence describes work that *will* or
# *is proposed to* happen, never work that exists now.
FUTURE_WORDING = st.sampled_from(
    [
        "This capability is planned for a future revision.",
        "Proposed in an RFC; no implementation exists yet.",
        "The roadmap intends to deliver this later.",
        "A future gate will validate this once implemented.",
        "This test is proposed and has not been run.",
    ]
)

_PATH_SEGMENT = st.text(
    alphabet="abcdefghijklmnopqrstuvwxyz0123456789_", min_size=1, max_size=10
)


@st.composite
def _repository_paths(draw: st.DrawFn) -> str:
    segments = draw(st.lists(_PATH_SEGMENT, min_size=1, max_size=3))
    suffix = draw(st.sampled_from([".md", ".toml", ".nb", ".py", ".yml"]))
    return "/".join(segments) + suffix


@st.composite
def _claim_keys(draw: st.DrawFn) -> str:
    prefix = draw(st.sampled_from(["plan", "gate", "rfc", "test", "roadmap"]))
    body = draw(st.text(alphabet="abcdefghijklmnopqrstuvwxyz0123456789-.", min_size=1, max_size=16))
    assume(body.strip("-.") != "")
    return f"{prefix}:{body}"


# Feature: nebula-universe-os-gap-analysis, Property 4: Plans never become
# implementation - plan-only prose classifies as Planned (or Unknown when
# pathless), never an implemented status, and grants no maturity credit.
# **Validates: Requirements 1.6, 13.2**
@given(
    has_path=st.booleans(),
    source_path=_repository_paths(),
    proposed_status=ADVERSARIAL_PROPOSED_STATUS,
)
@settings(max_examples=200, deadline=None, print_blob=True)
def test_decide_status_refuses_to_credit_plan_only_prose_as_implementation(
    has_path: bool,
    source_path: str,
    proposed_status: EvidenceStatus | None,
) -> None:
    """Exercise the real decide_status classifier for future-work evidence.

    Even when a caller adversarially proposes an implemented status (GA, preview,
    experimental), plan-only prose must resolve to Planned (with a path) or
    Unknown (pathless). This is non-tautological: the proposed status is varied
    across the implemented tiers and the classifier actively overrides it.
    """

    resolved = decide_status(
        source_path=source_path if has_path else None,
        plan_only=True,
        negative_claim=False,
        audited_absence=False,
        proposed_status=proposed_status,
    )

    # Classification never becomes an implemented status regardless of what the
    # caller proposed, so no maturity credit can be derived from a plan.
    assert resolved not in IMPLEMENTED_STATUSES
    assert resolved in NON_IMPLEMENTATION_STATUSES

    if has_path:
        # Future prose with a verifiable path is a plan, not implementation.
        assert resolved is EvidenceStatus.PLANNED
    else:
        # A pathless claim cannot be verified and is Unknown, never Planned-as-fact.
        assert resolved is EvidenceStatus.UNKNOWN


# Feature: nebula-universe-os-gap-analysis, Property 4: Plans never become
# implementation - the collector builds Planned, future-worded records from
# plan-only evidence and preserves the future wording without upgrading it.
# **Validates: Requirements 1.6, 13.2**
@given(
    inputs=st.lists(
        st.builds(
            lambda claim_key, claim, kind, origin, path, proposed, verification: ClaimInput(
                claim_key=claim_key,
                claim=claim,
                evidence_kind=kind,
                origin=origin,
                source_path=path,
                proposed_status=proposed,
                plan_only=True,
                verification_state=verification,
            ),
            claim_key=_claim_keys(),
            claim=FUTURE_WORDING,
            kind=FUTURE_PROSE_KINDS,
            origin=st.sampled_from(tuple(RevisionOrigin)),
            path=_repository_paths(),
            proposed=ADVERSARIAL_PROPOSED_STATUS,
            verification=st.sampled_from(tuple(VerificationState)),
        ),
        min_size=1,
        max_size=8,
    ),
)
@settings(max_examples=100, deadline=None, print_blob=True)
def test_collector_emits_only_planned_future_records_for_plan_only_evidence(
    inputs: list[ClaimInput],
) -> None:
    """Exercise the real EvidenceCollector normalization for plan-only inputs.

    Every produced record must be classified Planned, must never carry an
    implemented status, and must keep the caller's future-tense wording verbatim
    (the collector never rewrites a plan into a present-tense implementation
    claim). The proposed status is adversarial per input.
    """

    # Each ClaimInput's own status property is driven by the real decision order.
    for candidate in inputs:
        assert candidate.status is EvidenceStatus.PLANNED

    collector = EvidenceCollector()
    bundle = collector._normalize(inputs, reference("revision-property-4"))

    assert bundle.records  # plan-only, path-bearing inputs always serialize
    wordings_in = {candidate.claim for candidate in inputs}
    for record in bundle.records:
        # No plan is ever credited as implementation, so no maturity credit.
        assert record.status is EvidenceStatus.PLANNED
        assert record.status not in IMPLEMENTED_STATUSES
        # Future wording is preserved losslessly; the collector does not upgrade
        # plan prose into a present-tense implementation assertion.
        assert record.claim in wordings_in
