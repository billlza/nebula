"""Property-based test for hosted / scoped-release isolation (design Property 9).

This module owns the Hypothesis property test for design Property 9:

    Hosted and scoped-release evidence cannot propagate into OS substrate.

It exercises the *real* Task 7.3 observability/security/reliability evaluator
(:func:`evaluate_observability_security_reliability`), the *real* Task 7.4
application/ecosystem/release evaluator
(:func:`evaluate_application_ecosystem_release`), and the *real* Task 4.3 Claim
Guard (:func:`guard_evidence`). No product code is edited and no mocks are used.

The four hosted / scoped-release evidence sources named by the property are all
generated adversarially -- their claim text deliberately mentions OS-substrate
observability topics (kernel log, boot diagnostics, driver observability,
userspace observability, kernel/user correlation) so that the evaluator *does*
match them onto OS-substrate domains and must actively refuse to promote them:

* hosted examples (``Example`` evidence kind);
* compiler / hosted-service observability records (hosted GA tiers);
* compiler/tooling GA releases;
* Linux backend SDK GA releases.

The property is proven as a monotonicity/delta invariant against a freely
generated baseline: adding any of those records to the baseline leaves every
OS-substrate domain's ``satisfied`` flag and maturity score unchanged and can
never newly promote the OS-substrate strength headline. To keep the test from
being a tautology it (a) grounds every generated hosted record against the real
Claim Guard's independent ``substrate_promotion_blocked`` decision, and (b)
proves the evaluators are *live* -- the same hosted evidence is still observed
in its declared hosted scope, and a genuine OS-substrate implementation is still
recognised.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.claim_guard import guard_evidence
from tools.universe_os_gap_analysis.evaluators.application_ecosystem_release import (
    evaluate_application_ecosystem_release,
)
from tools.universe_os_gap_analysis.evaluators.observability_security_reliability import (
    OPERATIONS_CHECKLIST,
    AssessmentScope,
    OperationsScopeStrength,
    evaluate_observability_security_reliability,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import stable_id
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
    TargetLevel,
    VerificationState,
)

_REVISION_REF = stable_id("revision", "property-9")

# The OS-substrate observability topics named by the property (boot, kernel,
# driver, userspace, OS-operations). Each is a literal marker recognised by the
# real observability evaluator, so hosted evidence quoting them is genuinely
# matched onto an OS-substrate domain rather than being silently dropped.
_OS_SUBSTRATE_MARKERS: tuple[str, ...] = (
    "boot diagnostics",
    "kernel log",
    "driver observability",
    "userspace observability",
    "kernel/user correlation",
)

# Requirement 4.6 / 9.2 / 11.6 grounding: hosted / scoped-release statuses that
# the Claim Guard treats as unable to raise OS-substrate maturity. Re-derived
# here from the requirement text, not imported from the guard.
_SCOPED_GA_STATUSES: tuple[EvidenceStatus, ...] = (
    EvidenceStatus.COMPILER_TOOLING_GA,
    EvidenceStatus.BACKEND_SDK_GA,
)

# Evidence kinds usable for hosted-service observability / compiler tooling that
# are not the Example kind (Example is handled as its own category).
_HOSTED_TOOLING_KINDS: tuple[EvidenceKind, ...] = (
    EvidenceKind.SOURCE,
    EvidenceKind.ARTIFACT,
    EvidenceKind.TEST_EXECUTION,
    EvidenceKind.WORKFLOW,
)


# --------------------------------------------------------------------------- #
# Shared record building                                                      #
# --------------------------------------------------------------------------- #


def _make_record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus,
    kind: EvidenceKind,
    origin: RevisionOrigin,
    source_path: str,
    unique: str,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value, unique),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] += (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


# --------------------------------------------------------------------------- #
# Baseline generator (freely varied; may contain genuine OS-substrate impls)  #
# --------------------------------------------------------------------------- #

# A pool of baseline claim texts. Some quote OS-substrate markers so that a
# genuine OS-substrate implementation (implemented status + direct evidence
# kind + current origin) can legitimately satisfy an OS-substrate domain, which
# keeps the "cannot *increase*" invariant non-trivial.
_BASELINE_CLAIMS: tuple[str, ...] = (
    "Kernel observability with kernel log tracing and kernel metrics.",
    "Boot diagnostics emitted by the boot chain during early boot.",
    "Driver observability owned by a driver model with driver log output.",
    "Userspace observability with process tracing and userspace metrics.",
    "Kernel/user correlation across kernel and userspace log streams.",
    "Source diagnostics, LSP language server, and formatter tooling.",
    "The parser and typechecker lower source into a typed representation.",
    "Hosted service observability provides profiling, tracing, and metrics.",
)


@st.composite
def _baseline_record(draw: st.DrawFn) -> EvidenceRecord:
    claim = draw(st.sampled_from(_BASELINE_CLAIMS))
    status = draw(st.sampled_from(tuple(EvidenceStatus)))
    kind = draw(st.sampled_from(tuple(EvidenceKind)))
    origin = draw(st.sampled_from(tuple(RevisionOrigin)))
    unique = draw(st.from_regex(r"[a-z0-9]{6}", fullmatch=True))
    return _make_record(
        claim_key=f"baseline:{unique}",
        claim=claim,
        status=status,
        kind=kind,
        origin=origin,
        source_path="docs/universeos/architecture.md",
        unique=unique,
    )


# --------------------------------------------------------------------------- #
# Hosted / scoped-release generator (the four sources named by Property 9)    #
# --------------------------------------------------------------------------- #

_CAT_HOSTED_EXAMPLE = "hosted_example"
_CAT_HOSTED_OBSERVABILITY = "hosted_observability"
_CAT_COMPILER_TOOLING_RELEASE = "compiler_tooling_release"
_CAT_BACKEND_SDK_RELEASE = "backend_sdk_release"


@st.composite
def _hosted_scoped_record(draw: st.DrawFn) -> EvidenceRecord:
    """One hosted / scoped-release record from the four Property-9 sources.

    Every branch quotes an OS-substrate observability marker so the record is
    actively matched onto an OS-substrate domain, yet each branch is, by the
    requirement text, unable to raise OS-substrate maturity: a hosted example,
    a hosted GA observability record, a compiler/tooling GA release, or a Linux
    backend SDK GA release.
    """

    category = draw(
        st.sampled_from(
            (
                _CAT_HOSTED_EXAMPLE,
                _CAT_HOSTED_OBSERVABILITY,
                _CAT_COMPILER_TOOLING_RELEASE,
                _CAT_BACKEND_SDK_RELEASE,
            )
        )
    )
    marker = draw(st.sampled_from(_OS_SUBSTRATE_MARKERS))
    origin = draw(st.sampled_from(tuple(RevisionOrigin)))
    unique = draw(st.from_regex(r"[a-z0-9]{6}", fullmatch=True))

    if category == _CAT_HOSTED_EXAMPLE:
        # Hosted example: any status, but the Example kind alone is enough for
        # the Claim Guard to block substrate promotion.
        status = draw(st.sampled_from(tuple(EvidenceStatus)))
        kind = EvidenceKind.EXAMPLE
        claim = f"Hosted example project shows {marker} dashboards on the host OS."
        source_path = "examples/observe/main.nb"
    elif category == _CAT_HOSTED_OBSERVABILITY:
        # Compiler / hosted-service observability, shipped as a hosted GA tier.
        status = draw(st.sampled_from(_SCOPED_GA_STATUSES))
        kind = draw(st.sampled_from(_HOSTED_TOOLING_KINDS))
        claim = f"Hosted service observability tooling ships {marker} views as a GA service."
        source_path = "services/observe/main.nb"
    elif category == _CAT_COMPILER_TOOLING_RELEASE:
        status = EvidenceStatus.COMPILER_TOOLING_GA
        kind = EvidenceKind.RELEASE
        claim = f"The compiler/tooling GA release notes describe {marker} in the hosted toolchain."
        source_path = "RELEASE_NOTES_v1.0.0.md"
    else:  # _CAT_BACKEND_SDK_RELEASE
        status = EvidenceStatus.BACKEND_SDK_GA
        kind = EvidenceKind.RELEASE
        claim = f"The Linux backend SDK GA release adds {marker} to the hosted backend service."
        source_path = "RELEASE_NOTES_v1.0.0.md"

    return _make_record(
        claim_key=f"{category}:{unique}",
        claim=claim,
        status=status,
        kind=kind,
        origin=origin,
        source_path=source_path,
        unique=unique,
    )


def _guard_blocks_all(records: list[EvidenceRecord]) -> bool:
    """Independent grounding: the real Claim Guard flags every record blocked.

    This is re-derived through the real guard rather than asserted structurally,
    so the property's premise ("these records are hosted / scoped-release") is
    anchored in the actual Task 4.3 governance, not merely assumed.
    """

    guarded = guard_evidence(_bundle(records))
    blocked = {
        str(claim.evidence_id): claim.substrate_promotion_blocked
        for claim in guarded.claims
    }
    return all(blocked.get(str(record.id), False) for record in records)


# Feature: nebula-universe-os-gap-analysis, Property 9: Hosted and scoped-release evidence cannot propagate into OS substrate - adding hosted examples, compiler/service observability, compiler/tooling releases, or Linux backend SDK releases cannot increase any boot/kernel/driver/userspace/OS-operations maturity.
# **Validates: Requirements 4.6, 9.2, 11.6**
@given(
    baseline=st.lists(
        _baseline_record(), max_size=6, unique_by=lambda r: str(r.id)
    ),
    hosted=st.lists(
        _hosted_scoped_record(), min_size=1, max_size=6, unique_by=lambda r: str(r.id)
    ),
)
@settings(max_examples=200, deadline=None, print_blob=True)
def test_hosted_scoped_evidence_cannot_raise_os_substrate_maturity(
    baseline: list[EvidenceRecord],
    hosted: list[EvidenceRecord],
) -> None:
    """Requirement 4.6/9.2/11.6: hosted / scoped-release additions never promote substrate."""

    # Grounding: the real Claim Guard independently agrees these hosted /
    # scoped-release records cannot raise OS-substrate maturity.
    assert _guard_blocks_all(hosted)

    before = evaluate_observability_security_reliability(_bundle(baseline))
    after = evaluate_observability_security_reliability(_bundle(baseline + hosted))

    # Every OS-substrate observability/security/reliability domain is unchanged:
    # adding hosted / scoped-release evidence cannot flip it satisfied or raise
    # its maturity score.
    for item in OPERATIONS_CHECKLIST:
        if item.scope is not AssessmentScope.OS_SUBSTRATE:
            continue
        capability_id = str(item.capability_id)
        before_draft = before.draft_for(capability_id)
        after_draft = after.draft_for(capability_id)
        assert before_draft is not None, capability_id
        assert after_draft is not None, capability_id
        assert after_draft.satisfied == before_draft.satisfied, capability_id
        assert after_draft.maturity_score == before_draft.maturity_score, capability_id
        # A hosted-only OS-substrate domain never earns any maturity credit.
        if not after_draft.satisfied:
            assert after_draft.maturity_score is MaturityScore.ABSENT, capability_id

    # The OS-substrate headline can never be *newly* promoted to OS_SUBSTRATE by
    # hosted / scoped-release evidence: it is OS_SUBSTRATE after the addition iff
    # it already was before.
    assert (
        after.os_substrate_strength is OperationsScopeStrength.OS_SUBSTRATE
    ) == (before.os_substrate_strength is OperationsScopeStrength.OS_SUBSTRATE)


# Feature: nebula-universe-os-gap-analysis, Property 9: Hosted and scoped-release evidence cannot propagate into OS substrate - compiler/tooling and Linux backend SDK release evidence is flagged unable to raise OS-substrate maturity by the application/ecosystem/release evaluator.
# **Validates: Requirements 4.6, 9.2, 11.6**
@st.composite
def _scoped_release_record(draw: st.DrawFn) -> EvidenceRecord:
    """A compiler/tooling or Linux backend SDK GA release/ecosystem record."""

    marker = draw(
        st.sampled_from(
            ("sbom", "provenance", "attestation", "installer", "package breadth", "documentation")
        )
    )
    status = draw(st.sampled_from(_SCOPED_GA_STATUSES))
    kind = draw(st.sampled_from(tuple(EvidenceKind)))
    origin = draw(st.sampled_from(tuple(RevisionOrigin)))
    unique = draw(st.from_regex(r"[a-z0-9]{6}", fullmatch=True))
    claim = f"The Linux backend SDK / compiler tooling GA release provides {marker}."
    return _make_record(
        claim_key=f"release:{unique}",
        claim=claim,
        status=status,
        kind=kind,
        origin=origin,
        source_path=".github/workflows/release.yml",
        unique=unique,
    )


@given(
    records=st.lists(
        _scoped_release_record(), min_size=1, max_size=8, unique_by=lambda r: str(r.id)
    )
)
@settings(max_examples=150, deadline=None, print_blob=True)
def test_scoped_release_evidence_is_flagged_and_scope_isolated(
    records: list[EvidenceRecord],
) -> None:
    """Requirement 11.6 / Property 9: scoped-release evidence is quarantined."""

    result = evaluate_application_ecosystem_release(_bundle(records))

    # Every ecosystem/release domain that matched scoped-release evidence is
    # flagged as unable to raise OS-substrate maturity, is marked scoped-release
    # only, and records the corresponding limitation. This is the only effect
    # such evidence may have: it stays confined to its declared hosted scope.
    touched = 0
    for draft in result.maturity_drafts():
        if not draft.supporting_evidence_ids:
            continue
        touched += 1
        assert draft.substrate_promotion_blocked is True, str(draft.domain.id)
        assert draft.scoped_release_only is True, str(draft.domain.id)
        limitation_text = "\n".join(draft.limitations).lower()
        assert "cannot raise os-substrate maturity" in limitation_text, str(draft.domain.id)

    # The generated evidence always matches at least one release/ecosystem
    # capability, so the evaluator genuinely reacted to it (non-tautology).
    assert touched >= 1


# Feature: nebula-universe-os-gap-analysis, Property 9: Hosted and scoped-release evidence cannot propagate into OS substrate - hosted evidence is still observed within its declared hosted scope while a genuine OS-substrate implementation is still recognised (liveness / non-tautology).
# **Validates: Requirements 4.6, 9.2, 11.6**
def test_hosted_evidence_is_observed_but_quarantined() -> None:
    """A deterministic liveness check proving the evaluator is not vacuous."""

    kernel_obs = "capability-observability-kernel"

    # A hosted-service GA observability record that explicitly quotes an
    # OS-substrate topic is observed (strength becomes hosted-only), but never
    # satisfies the OS-substrate kernel-observability domain.
    hosted = _make_record(
        claim_key="hosted:kernel-observe",
        claim="Hosted service observability ships kernel log dashboards as a GA service.",
        status=EvidenceStatus.BACKEND_SDK_GA,
        kind=EvidenceKind.SOURCE,
        origin=RevisionOrigin.COMMITTED_REVISION,
        source_path="services/observe/main.nb",
        unique="a1b2c3",
    )
    hosted_result = evaluate_observability_security_reliability(_bundle([hosted]))
    assert (
        hosted_result.os_substrate_strength
        is OperationsScopeStrength.COMPILER_HOSTED_ONLY
    )
    kernel_draft = hosted_result.draft_for(kernel_obs)
    assert kernel_draft is not None
    assert kernel_draft.satisfied is False
    assert kernel_draft.maturity_score is MaturityScore.ABSENT

    # A genuine, non-hosted OS-substrate implementation of the same capability
    # IS recognised, confirming the domain can be satisfied at all.
    genuine = _make_record(
        claim_key="source:kernel/observe.nb",
        claim="Kernel observability with kernel log tracing and kernel metrics.",
        status=EvidenceStatus.EXPERIMENTAL,
        kind=EvidenceKind.SOURCE,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        source_path="kernel/observe.nb",
        unique="d4e5f6",
    )
    genuine_result = evaluate_observability_security_reliability(_bundle([genuine]))
    assert (
        genuine_result.os_substrate_strength is OperationsScopeStrength.OS_SUBSTRATE
    )
    genuine_draft = genuine_result.draft_for(kernel_obs)
    assert genuine_draft is not None
    assert genuine_draft.satisfied is True

    # Adding the hosted record on top of the genuine one does not change the
    # genuine domain's satisfaction (it was already satisfied, and hosted
    # evidence adds no substrate credit).
    combined = evaluate_observability_security_reliability(_bundle([genuine, hosted]))
    combined_draft = combined.draft_for(kernel_obs)
    assert combined_draft is not None
    assert combined_draft.satisfied is True
    assert combined_draft.maturity_score == genuine_draft.maturity_score


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner, so allow the property
    # tests to be executed directly as a fallback.
    test_hosted_scoped_evidence_cannot_raise_os_substrate_maturity()
    test_scoped_release_evidence_is_flagged_and_scope_isolated()
    test_hosted_evidence_is_observed_but_quarantined()
    print("Property 9 OK: hosted / scoped-release evidence stays out of OS substrate")
