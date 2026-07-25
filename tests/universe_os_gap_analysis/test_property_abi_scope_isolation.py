"""Property-based test for ABI scope isolation and T1 independence blockers.

This module owns the Hypothesis property test for design Property 12. It
exercises the *real* :func:`evaluate_abi_backend` evaluator (Task 6.1) against
adversarially generated evidence bundles; no product code is edited and no
mocks are used. Two independent oracles, re-derived from the Requirement 7.2 /
7.4 / 7.5 text rather than from the implementation, keep the properties from
being tautologies.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.evaluators.abi_backend import (
    AbiScope,
    evaluate_abi_backend,
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
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

# --------------------------------------------------------------------------- #
# Independent oracles                                                         #
#                                                                             #
# Everything below is re-derived from the *requirement text* (7.2 / 7.4 /     #
# 7.5), not copied from the evaluator, so the comparison is meaningful.       #
# --------------------------------------------------------------------------- #

_REVISION_REF = stable_id("revision", "property-12")

# The six freestanding ABI capability domains that hosted C ABI evidence must
# never satisfy (Requirement 7.2).
_FREESTANDING_ABI_CAPABILITIES: tuple[str, ...] = (
    "capability-compiler-abi",
    "capability-runtime-abi",
    "capability-boot-abi",
    "capability-syscall-abi",
    "capability-driver-abi",
    "capability-package-abi",
)

_HOSTED_CAPABILITY = "capability-hosted-c-abi"
_T1_CAPABILITY = "capability-t1-language-platform"

# Requirement 7.4/7.5: a "current implementation" backing requires an
# implemented status, a direct implementation/executable evidence kind, and a
# current-revision origin. These mirror the glossary's "implementation
# evidence from the bound revision" and are derived here independently.
_IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)
_CURRENT_REVISION_ORIGINS: frozenset[RevisionOrigin] = frozenset(
    {
        RevisionOrigin.COMMITTED_REVISION,
        RevisionOrigin.CURRENT_WORKTREE,
        RevisionOrigin.EXECUTION_ARTIFACT,
    }
)


def _is_current_implementation(
    status: EvidenceStatus, kind: EvidenceKind, origin: RevisionOrigin
) -> bool:
    """Requirement 7.4/7.5 backing test, derived from the requirement text."""

    return (
        status in _IMPLEMENTED_STATUSES
        and kind in _DIRECT_IMPLEMENTATION_KINDS
        and origin in _CURRENT_REVISION_ORIGINS
    )


# --------------------------------------------------------------------------- #
# Shared record building                                                      #
# --------------------------------------------------------------------------- #

# Safe filler: a single space-free, hyphen-free token. Every ABI-scope and
# dependency marker is multi-word (contains a space) or hyphenated, so a lone
# alnum token can never accidentally build one.
_SAFE_FILLER = st.from_regex(r"[a-z0-9]{0,12}", fullmatch=True)


def _make_record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus,
    kind: EvidenceKind,
    origin: RevisionOrigin,
    unique: str,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, unique),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path="spec/abi_layout.md",
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
# Part 1 generators: hosted C ABI evidence only                              #
# --------------------------------------------------------------------------- #

# Distinct hosted C ABI phrasings. Each contains a hosted C ABI marker and,
# critically, none of the freestanding-scope markers ("compiler abi",
# "runtime abi", "boot abi", "syscall abi", "driver abi", "package abi").
_HOSTED_C_ABI_PHRASES: tuple[str, ...] = (
    'The hosted C abi exposes extern "C" imports and exported C abi types.',
    "The C-abi calling convention and symbol rules are documented here.",
    "Aggregate layout and enum layout for the hosted C abi are specified.",
    "Cross-language fixture coverage for the interop_c_abi surface exists.",
    "The abi_layout describes alignment and versioning for the hosted C abi.",
)


@st.composite
def _hosted_records(draw: st.DrawFn) -> EvidenceRecord:
    phrase = draw(st.sampled_from(_HOSTED_C_ABI_PHRASES))
    filler = draw(_SAFE_FILLER)
    claim = f"{phrase} {filler}".strip()
    status = draw(st.sampled_from(tuple(EvidenceStatus)))
    kind = draw(st.sampled_from(tuple(EvidenceKind)))
    origin = draw(st.sampled_from(tuple(RevisionOrigin)))
    unique = draw(st.from_regex(r"[a-z0-9]{6}", fullmatch=True))
    return _make_record(
        claim_key=f"source:spec/interop_c_abi.md#{unique}",
        claim=claim,
        status=status,
        kind=kind,
        origin=origin,
        unique=unique,
    )


# Feature: nebula-universe-os-gap-analysis, Property 12: ABI evidence is scope-isolated and production dependencies block T1 - hosted C ABI records can never satisfy the freestanding compiler/runtime/boot/syscall/driver/package ABI domains.
# **Validates: Requirements 7.2, 7.4, 7.5**
@given(
    records=st.lists(
        _hosted_records(), min_size=1, max_size=6, unique_by=lambda r: str(r.id)
    )
)
@settings(max_examples=150, deadline=None, print_blob=True)
def test_hosted_c_abi_never_satisfies_freestanding_domains(
    records: list[EvidenceRecord],
) -> None:
    """Requirement 7.2: hosted C ABI evidence is strictly scope-isolated.

    Whatever the status/kind/origin of the hosted C ABI evidence, none of it can
    leak into a freestanding ABI domain: every freestanding domain stays
    unsatisfied, collects no supporting evidence, and records an
    implementation gap. This drives the real ``evaluate_abi_backend``.
    """

    result = evaluate_abi_backend(_bundle(records))

    # Every freestanding ABI scope is present, unsatisfied, and evidence-free.
    for capability_id in _FREESTANDING_ABI_CAPABILITIES:
        draft = result.draft_for(capability_id)
        assert draft is not None, capability_id
        assert draft.satisfied is False, capability_id
        assert draft.supporting_evidence_ids == (), capability_id
        # An unsatisfied freestanding ABI domain always yields a gap.
        assert result.gap_for(capability_id) is not None, capability_id

    # All seven distinct ABI scopes remain modelled, so hosted evidence has not
    # collapsed the scope partition.
    scopes = {draft.scope for draft in result.domain_drafts if draft.scope is not None}
    assert scopes == set(AbiScope)

    # The hosted domain itself is the only ABI scope hosted evidence can touch.
    hosted = result.draft_for(_HOSTED_CAPABILITY)
    assert hosted is not None
    assert hosted.scope is AbiScope.HOSTED_C_ABI


# --------------------------------------------------------------------------- #
# Part 2 generators: production-dependency signal records                     #
# --------------------------------------------------------------------------- #

# Each "flavor" carries exactly one dependency signal marker and no others, so
# the oracle can reason about signals independently.
_FLAVOR_GENERATED_CPP = "generated_cpp"
_FLAVOR_EXTERNAL_CLANG = "external_clang"
_FLAVOR_INVENTORY_COMPLETE = "inventory_complete"
_FLAVOR_ACCEPTED_BOOTSTRAP = "accepted_bootstrap"
_FLAVOR_NEUTRAL = "neutral"

_FLAVOR_PHRASES: dict[str, str] = {
    # Contains a generated-C++ marker; no clang/inventory/bootstrap markers.
    _FLAVOR_GENERATED_CPP: "Production compilation lowers Nebula to generated cpp output.",
    # Contains an external-clang marker; no generated-cpp/inventory/bootstrap markers.
    _FLAVOR_EXTERNAL_CLANG: "The production build shells out to external clang for linking.",
    # Contains an inventory-complete marker; nothing else.
    _FLAVOR_INVENTORY_COMPLETE: "The complete dependency inventory enumerates every input.",
    # Contains an accepted-bootstrap marker; nothing else.
    _FLAVOR_ACCEPTED_BOOTSTRAP: "An accepted independent bootstrap now drives every release.",
    # Neutral: matches no dependency signal marker at all.
    _FLAVOR_NEUTRAL: "The parser and typechecker lower source into a typed representation.",
}


@st.composite
def _signal_records(draw: st.DrawFn) -> tuple[str, EvidenceRecord]:
    flavor = draw(st.sampled_from(tuple(_FLAVOR_PHRASES)))
    phrase = _FLAVOR_PHRASES[flavor]
    status = draw(st.sampled_from(tuple(EvidenceStatus)))
    kind = draw(st.sampled_from(tuple(EvidenceKind)))
    origin = draw(st.sampled_from(tuple(RevisionOrigin)))
    unique = draw(st.from_regex(r"[a-z0-9]{6}", fullmatch=True))
    record = _make_record(
        claim_key=f"source:spec/compiler_pipeline.md#{unique}",
        claim=phrase,
        status=status,
        kind=kind,
        origin=origin,
        unique=unique,
    )
    return flavor, record


# Feature: nebula-universe-os-gap-analysis, Property 12: ABI evidence is scope-isolated and production dependencies block T1 - while generated C++, external clang, an incomplete dependency inventory, or a missing accepted independent bootstrap persists, T1_Independent_Language_Platform stays unachieved.
# **Validates: Requirements 7.2, 7.4, 7.5**
@given(
    flavored=st.lists(
        _signal_records(), min_size=1, max_size=8, unique_by=lambda pair: str(pair[1].id)
    )
)
@settings(max_examples=200, deadline=None, print_blob=True)
def test_production_dependencies_block_t1_independence(
    flavored: list[tuple[str, EvidenceRecord]],
) -> None:
    """Requirements 7.4 + 7.5: any independence blocker keeps T1 unachieved.

    An independently derived oracle computes each dependency signal and the
    resulting T1 achievement from the requirement text, then compares against
    the real evaluator. T1 is achievable only when the dependency inventory is
    complete AND no generated-C++/external-clang production dependency survives
    without an accepted independent bootstrap.
    """

    records = [record for _flavor, record in flavored]
    result = evaluate_abi_backend(_bundle(records))
    t1 = result.t1_independence

    # -- oracle, derived from Requirement 7.4/7.5 ---------------------------- #
    generated_cpp = any(flavor == _FLAVOR_GENERATED_CPP for flavor, _ in flavored)
    external_clang = any(flavor == _FLAVOR_EXTERNAL_CLANG for flavor, _ in flavored)
    inventory_complete = any(
        flavor == _FLAVOR_INVENTORY_COMPLETE
        and _is_current_implementation(rec.status, rec.evidence_kind, rec.origin)
        for flavor, rec in flavored
    )
    accepted_bootstrap = any(
        flavor == _FLAVOR_ACCEPTED_BOOTSTRAP
        and _is_current_implementation(rec.status, rec.evidence_kind, rec.origin)
        for flavor, rec in flavored
    )

    blocked_74 = not inventory_complete
    blocked_75 = (generated_cpp or external_clang) and not accepted_bootstrap
    expected_achieved = not (blocked_74 or blocked_75)

    # -- the real evaluator agrees with the oracle on every signal ---------- #
    assert t1.generated_cpp_dependency is generated_cpp
    assert t1.external_clang_dependency is external_clang
    assert t1.dependency_inventory_complete is inventory_complete
    assert t1.accepted_independent_bootstrap is accepted_bootstrap
    assert t1.achieved is expected_achieved

    # An unachieved assessment always records at least one blocking reason and
    # surfaces a T1 gap; an achieved one carries neither.
    if expected_achieved:
        assert t1.blocking_reasons == ()
        assert result.gap_for(_T1_CAPABILITY) is None
    else:
        assert t1.blocking_reasons != ()
        assert result.gap_for(_T1_CAPABILITY) is not None


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner, so allow the property
    # tests to be executed directly as a fallback.
    test_hosted_c_abi_never_satisfies_freestanding_domains()
    test_production_dependencies_block_t1_independence()
    print("Property 12 OK: ABI scope isolation and T1 independence blockers hold")
