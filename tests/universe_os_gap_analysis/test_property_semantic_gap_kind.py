from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis import (
    LANGUAGE_TYPE_SYSTEM_CHECKLIST,
    evaluate_language_type_system,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    GapCategory,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    VerificationState,
)

# ---------------------------------------------------------------------------
# Evidence-space fixtures derived from the REAL checklist under test.
#
# We do not invent capability keys or specification paths: we harvest exactly
# the authoritative specification files, the parser/typechecker implementation
# directory prefixes, and the compatibility-policy files that the production
# language evaluator declares. Randomly including/excluding each of these lets
# Hypothesis explore the documented / implemented / stabilized combinations that
# select a gap kind, while the assertions run against the real evaluator output.
# ---------------------------------------------------------------------------

_ITEMS_BY_KEY = {item.key: item for item in LANGUAGE_TYPE_SYSTEM_CHECKLIST.items}

# Every distinct authoritative specification path any checklist item cites.
_ALL_SPEC_PATHS: tuple[str, ...] = tuple(
    sorted({path for item in LANGUAGE_TYPE_SYSTEM_CHECKLIST.items for path in item.specification_paths})
)

# The parser/typechecker implementation directory prefixes the checklist uses.
_IMPL_DIRS: tuple[str, ...] = ("frontend/", "passes/", "nir/")
_IMPL_FILENAMES = ("parser.cpp", "sema.cpp", "lower.cpp", "typecheck.cpp")

# The compatibility/stability policy source the checklist governs features with.
_COMPAT_PATH = "docs/stability_policy.md"

_REVISION_REF = reference("revision-property-10")

# Implementation-strength evidence kinds (SOURCE counts as parser/typechecker
# evidence); specification kind never does. Mirrors the evaluator's contract.
_IMPL_KIND = EvidenceKind.SOURCE


def _record(
    *,
    source_path: str,
    evidence_kind: EvidenceKind,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
) -> EvidenceRecord:
    claim_key = f"source:{source_path}:{evidence_kind.value}"
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, source_path, evidence_kind.value, status.value),
        claim_key=claim_key,
        claim=f"{source_path} provides {evidence_kind.value} evidence.",
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=f"File:{source_path}"),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=evidence_kind,
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


@st.composite
def _evidence_scenarios(draw: st.DrawFn) -> list[EvidenceRecord]:
    """Generate a documented/implemented/stabilized evidence mix.

    Each authoritative spec file, each implementation directory, and the
    compatibility policy file is independently included, so Hypothesis covers
    the full cross product of "documented", "implemented", and "stabilized"
    layers that Property 10 discriminates between.
    """

    records: list[EvidenceRecord] = []

    # Specification (documentation) evidence: a random subset of real spec files.
    spec_flags = draw(
        st.lists(st.booleans(), min_size=len(_ALL_SPEC_PATHS), max_size=len(_ALL_SPEC_PATHS))
    )
    for path, include in zip(_ALL_SPEC_PATHS, spec_flags):
        if include:
            records.append(_record(source_path=path, evidence_kind=EvidenceKind.SPECIFICATION))

    # Parser/typechecker implementation evidence under real directory prefixes.
    for impl_dir in _IMPL_DIRS:
        if draw(st.booleans()):
            filename = draw(st.sampled_from(_IMPL_FILENAMES))
            records.append(
                _record(source_path=impl_dir + filename, evidence_kind=_IMPL_KIND)
            )

    # Compatibility/stability policy evidence.
    if draw(st.booleans()):
        records.append(
            _record(source_path=_COMPAT_PATH, evidence_kind=EvidenceKind.SPECIFICATION)
        )

    return records


def _language_gap_for(draft, item_key: str):
    expected_id = str(stable_id("gap", "language", item_key))
    for gap in draft.gaps:
        if str(gap.id) == expected_id:
            return gap
    return None


def _verification_gap_for(draft, item_key: str):
    expected_id = str(stable_id("gap", "verification", item_key))
    for gap in draft.gaps:
        if str(gap.id) == expected_id:
            return gap
    return None


# Feature: nebula-universe-os-gap-analysis, Property 10: Semantic evidence creates
# the correct gap kind - a documented feature always yields a Language_Gap that
# references its authoritative source (and direct implementation evidence when
# present), and parser/typechecker support without a compatibility policy yields
# a semantic-stability Verification_Gap, each gap keeping exactly one primary
# category (the one-primary-category rule).
# **Validates: Requirements 5.3, 5.4**
@given(records=_evidence_scenarios())
@settings(max_examples=200, deadline=None, print_blob=True)
def test_semantic_evidence_creates_the_correct_gap_kind(
    records: list[EvidenceRecord],
) -> None:
    draft = evaluate_language_type_system(_bundle(records))
    domain_ref = reference(draft.domain.id)

    # One-primary-category rule: every gap has a single primary category drawn
    # from the two kinds this evaluator may emit, and secondary labels never
    # duplicate it.
    for gap in draft.gaps:
        assert gap.primary_category in {GapCategory.LANGUAGE, GapCategory.VERIFICATION}
        assert gap.primary_category not in gap.secondary_categories

    # Exactly one Language_Gap per documented checklist feature.
    language_gaps = draft.gaps_by_category(GapCategory.LANGUAGE)
    assert len(language_gaps) == len(LANGUAGE_TYPE_SYSTEM_CHECKLIST.items)

    for finding in draft.findings:
        item = _ITEMS_BY_KEY[finding.item_key]

        # Requirement 5.3: the documented feature yields a Language_Gap that
        # references its authoritative specification source(s).
        language_gap = _language_gap_for(draft, item.key)
        assert language_gap is not None
        assert language_gap.primary_category is GapCategory.LANGUAGE
        assert domain_ref in language_gap.domain_ids
        assert language_gap.acceptance_evidence  # authoritative acceptance target
        for spec_path in item.specification_paths:
            assert spec_path in language_gap.observed_fact

        # Requirement 5.3: direct implementation evidence is referenced when the
        # parser/typechecker layer actually matched, and explicitly noted absent
        # otherwise (the real evaluator branch is exercised both ways).
        if finding.has_parser_typechecker:
            assert "direct implementation evidence" in language_gap.observed_fact
            assert "no direct implementation evidence" not in language_gap.observed_fact
            for impl_ref in finding.evidence_by_layer.get(
                _parser_layer(), ()
            ):
                assert str(impl_ref) in language_gap.observed_fact
        else:
            assert "no direct implementation evidence" in language_gap.observed_fact

        # Requirement 5.4: parser/typechecker support without a compatibility
        # policy produces a semantic-stability Verification_Gap; anything else
        # produces none for this feature.
        verification_gap = _verification_gap_for(draft, item.key)
        if finding.has_parser_typechecker and not finding.has_compatibility_policy:
            assert verification_gap is not None
            assert verification_gap.primary_category is GapCategory.VERIFICATION
            assert "no compatibility policy" in verification_gap.observed_fact
            assert "parser/typechecker implementation evidence" in verification_gap.observed_fact
            assert domain_ref in verification_gap.domain_ids
        else:
            assert verification_gap is None


def _parser_layer():
    # Imported lazily to keep the evidence-layer enum close to its use site.
    from tools.universe_os_gap_analysis import EvidenceLayer

    return EvidenceLayer.PARSER_TYPECHECKER


# Feature: nebula-universe-os-gap-analysis, Property 10: Semantic evidence creates
# the correct gap kind - supplying a compatibility policy for a feature that has
# parser/typechecker evidence suppresses its semantic-stability Verification_Gap,
# confirming the gap kind tracks the compatibility-policy layer rather than input
# noise.
# **Validates: Requirements 5.3, 5.4**
@given(records=_evidence_scenarios())
@settings(max_examples=100, deadline=None, print_blob=True)
def test_compatibility_policy_layer_governs_verification_gap_presence(
    records: list[EvidenceRecord],
) -> None:
    draft = evaluate_language_type_system(_bundle(records))
    verification_ids = {str(gap.id) for gap in draft.gaps_by_category(GapCategory.VERIFICATION)}

    for finding in draft.findings:
        expected_id = str(stable_id("gap", "verification", finding.item_key))
        should_exist = finding.has_parser_typechecker and not finding.has_compatibility_policy
        assert (expected_id in verification_ids) == should_exist
