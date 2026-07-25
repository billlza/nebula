# Feature: nebula-universe-os-gap-analysis, Property 20: Gap register classification and ranking are deterministic
# **Validates: Requirements 12.1, 12.2, 12.3, 12.4**
"""Property 20 - Gap register classification and ranking are deterministic.

For all gap sets, each gap has exactly one primary category and all required
Requirement 12.3 fields; secondary categories are unique and exclude the primary
(Requirements 12.1, 12.2); and the priority ranking is a strict lexicographic
order over ``(dependency_criticality, safety_impact, claim_risk,
target_unblock_value, stable_id)`` - the four heterogeneous dimensions are
compared independently (never summed) and the stable identifier is the final
tie-break - so the ordering is a strict total order independent of input order
(Requirement 12.4).

These properties run against the real
:mod:`tools.universe_os_gap_analysis.gap_register` and
:mod:`tools.universe_os_gap_analysis.roadmap` modules with no mocks. The gap
generators deliberately produce varied categories, duplicated secondary labels,
and small priority-dimension ranges so priority ties (and stable-id tie-breaks)
are common.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.gap_register import (
    ALL_GAP_CATEGORIES,
    GapRegister,
    build_gap_register,
)
from tools.universe_os_gap_analysis.identifiers import StableId
from tools.universe_os_gap_analysis.models import (
    EvidenceStatus,
    GapCategory,
    GapEntry,
    Severity,
    TargetLevel,
)
from tools.universe_os_gap_analysis.roadmap import (
    GapRanking,
    gap_priority_key,
    rank_gap_register,
    rank_gaps,
)

# The full Requirement 12.3 field set every gap must record.
_REQUIRED_12_3_FIELDS: tuple[str, ...] = (
    "domain_ids",
    "current_status",
    "target_level",
    "severity",
    "dependencies",
    "acceptance_evidence",
    "recommended_owner_area",
)

_CATEGORIES: tuple[GapCategory, ...] = tuple(GapCategory)
_EVIDENCE_STATUSES: tuple[EvidenceStatus, ...] = tuple(EvidenceStatus)
_TARGET_LEVELS: tuple[TargetLevel, ...] = tuple(TargetLevel)
_SEVERITIES: tuple[Severity, ...] = tuple(Severity)

# Small priority-dimension range so ties across dimensions (and hence stable-id
# tie-breaks) occur frequently under Hypothesis.
_SMALL_INT = st.integers(min_value=0, max_value=3)


def _make_gap(
    gap_id: str,
    dims: tuple[int, int, int, int],
    *,
    primary: GapCategory = GapCategory.IMPLEMENTATION,
    secondary: tuple[GapCategory, ...] = (),
) -> GapEntry:
    """Build a valid :class:`GapEntry` with the four priority dimensions ``dims``."""

    dependency_criticality, safety_impact, claim_risk, target_unblock_value = dims
    return GapEntry(
        id=StableId(gap_id),
        title=f"Gap {gap_id}",
        primary_category=primary,
        secondary_categories=secondary,
        domain_ids=(f"domain-{gap_id}",),
        current_status=EvidenceStatus.UNSUPPORTED,
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        severity=Severity.HIGH,
        dependencies=(),
        acceptance_evidence=(f"Acceptance evidence closing {gap_id}.",),
        recommended_owner_area="Kernel",
        dependency_criticality=dependency_criticality,
        safety_impact=safety_impact,
        claim_risk=claim_risk,
        target_unblock_value=target_unblock_value,
        observed_fact=f"{gap_id} has no direct implementation evidence.",
        recommendation=f"Implement and verify {gap_id} before depending on it.",
    )


@st.composite
def _gap(draw: st.DrawFn, gap_id: str) -> GapEntry:
    """Draw a single gap with a varied category, duplicate-prone secondaries and ties."""

    primary = draw(st.sampled_from(_CATEGORIES))
    others = [category for category in _CATEGORIES if category is not primary]
    # A list (not a set) over the *other* three categories: duplicates are
    # expected and exercise the register's secondary-label deduplication, while
    # never introducing the primary category into the secondaries.
    secondary = tuple(draw(st.lists(st.sampled_from(others), max_size=6)))
    dims = (
        draw(_SMALL_INT),
        draw(_SMALL_INT),
        draw(_SMALL_INT),
        draw(_SMALL_INT),
    )
    return GapEntry(
        id=StableId(gap_id),
        title=f"Gap {gap_id}",
        primary_category=primary,
        secondary_categories=secondary,
        domain_ids=(f"domain-{draw(st.integers(min_value=0, max_value=3))}",),
        current_status=draw(st.sampled_from(_EVIDENCE_STATUSES)),
        target_level=draw(st.sampled_from(_TARGET_LEVELS)),
        severity=draw(st.sampled_from(_SEVERITIES)),
        dependencies=(),
        acceptance_evidence=(f"Acceptance evidence closing {gap_id}.",),
        recommended_owner_area="Kernel",
        dependency_criticality=dims[0],
        safety_impact=dims[1],
        claim_risk=dims[2],
        target_unblock_value=dims[3],
        observed_fact=f"{gap_id} has no direct implementation evidence.",
        recommendation=f"Implement and verify {gap_id} before depending on it.",
    )


@st.composite
def _gap_set(draw: st.DrawFn, min_size: int = 1, max_size: int = 8) -> list[GapEntry]:
    """Draw a list of gaps with unique stable identifiers."""

    size = draw(st.integers(min_value=min_size, max_value=max_size))
    return [draw(_gap(f"gap-prop20-{index:03d}")) for index in range(size)]


# --------------------------------------------------------------------------- #
# Classification (Requirements 12.1, 12.2, 12.3).                              #
# --------------------------------------------------------------------------- #


# **Validates: Requirements 12.1, 12.2, 12.3**
@given(gaps=_gap_set())
@settings(max_examples=200, deadline=None, print_blob=True)
def test_gap_register_classification_invariants(gaps: list[GapEntry]) -> None:
    register = build_gap_register(gaps)

    # The register keeps every (uniquely identified) gap.
    assert len(register) == len(gaps)

    for gap in register.gaps:
        # Requirement 12.1: exactly one primary category from the closed set.
        assert isinstance(gap.primary_category, GapCategory)
        assert gap.primary_category in ALL_GAP_CATEGORIES

        # Requirement 12.2: secondary labels are deduplicated and never repeat
        # the primary category.
        secondary = tuple(gap.secondary_categories)
        assert len(secondary) == len(set(secondary))
        assert gap.primary_category not in secondary
        assert all(isinstance(category, GapCategory) for category in secondary)

        # Requirement 12.3: the full required field set is present and populated.
        for field_name in _REQUIRED_12_3_FIELDS:
            assert hasattr(gap, field_name)
        assert len(gap.domain_ids) >= 1
        assert len(gap.acceptance_evidence) >= 1
        assert isinstance(gap.current_status, EvidenceStatus)
        assert isinstance(gap.target_level, TargetLevel)
        assert isinstance(gap.severity, Severity)
        assert gap.recommended_owner_area.strip()
        assert gap.observed_fact.strip()
        assert gap.recommendation.strip()

    # The per-primary-category counts partition the whole register.
    counts = register.primary_category_counts()
    assert set(counts) == set(ALL_GAP_CATEGORIES)
    assert sum(counts.values()) == len(register)


# **Validates: Requirements 12.1, 12.2, 12.3**
@given(
    primary=st.sampled_from(_CATEGORIES),
    data=st.data(),
)
@settings(max_examples=100, deadline=None, print_blob=True)
def test_secondary_labels_are_deduplicated(primary: GapCategory, data: st.DrawFn) -> None:
    others = [category for category in _CATEGORIES if category is not primary]
    # A raw secondary list that may repeat labels several times.
    raw_secondary = tuple(
        data.draw(st.lists(st.sampled_from(others), min_size=1, max_size=9))
    )
    gap = _make_gap("gap-prop20-dedup", (1, 1, 1, 1), primary=primary, secondary=raw_secondary)
    register = build_gap_register(gap)

    stored = register.gaps[0]
    assert set(stored.secondary_categories) == set(raw_secondary)
    assert len(stored.secondary_categories) == len(set(raw_secondary))
    assert primary not in stored.secondary_categories


# --------------------------------------------------------------------------- #
# Register order independence (Requirements 12.1-12.3).                        #
# --------------------------------------------------------------------------- #


# **Validates: Requirements 12.1, 12.2, 12.3**
@given(gaps=_gap_set(), data=st.data())
@settings(max_examples=300, deadline=None, print_blob=True)
def test_register_is_independent_of_input_permutation(
    gaps: list[GapEntry], data: st.DrawFn
) -> None:
    permutation = data.draw(st.permutations(gaps))

    baseline = build_gap_register(gaps)
    permuted = build_gap_register(list(permutation))

    assert [str(ref) for ref in baseline.gap_ids] == [
        str(ref) for ref in permuted.gap_ids
    ]
    assert baseline.gaps == permuted.gaps


# --------------------------------------------------------------------------- #
# Ranking is a strict deterministic total order (Requirement 12.4).           #
# --------------------------------------------------------------------------- #


# **Validates: Requirements 12.4**
@given(gaps=_gap_set(), data=st.data())
@settings(max_examples=300, deadline=None, print_blob=True)
def test_ranking_is_strict_total_order_independent_of_permutation(
    gaps: list[GapEntry], data: st.DrawFn
) -> None:
    ranked = rank_gaps(gaps)

    # Every gap appears exactly once.
    assert len(ranked) == len(gaps)
    assert {str(gap.id) for gap in ranked} == {str(gap.id) for gap in gaps}

    keys = [gap_priority_key(gap) for gap in ranked]

    # Strict total order: because the stable identifier is an always-distinct
    # final component, every key is unique and consecutive keys strictly ascend.
    assert len(set(keys)) == len(keys)
    for earlier, later in zip(keys, keys[1:]):
        assert earlier < later

    # Determinism: ranking is idempotent.
    assert [str(gap.id) for gap in rank_gaps(gaps)] == [str(gap.id) for gap in ranked]

    # Permutation independence: the same gaps in any order rank identically.
    permutation = data.draw(st.permutations(gaps))
    ranked_permuted = rank_gaps(list(permutation))
    assert [str(gap.id) for gap in ranked_permuted] == [str(gap.id) for gap in ranked]

    # The GapRanking dataclass and rank_gap_register agree with rank_gaps.
    register = build_gap_register(gaps)
    assert [str(gap.id) for gap in GapRanking(ranked_gaps=tuple(gaps)).ranked_gaps] == [
        str(gap.id) for gap in ranked
    ]
    assert [str(gap.id) for gap in rank_gap_register(register).ranked_gaps] == [
        str(gap.id) for gap in ranked
    ]


# --------------------------------------------------------------------------- #
# Ranking is lexicographic, never a heterogeneous sum (Requirement 12.4).     #
# --------------------------------------------------------------------------- #


# **Validates: Requirements 12.4**
@given(data=st.data())
@settings(max_examples=300, deadline=None, print_blob=True)
def test_earlier_dimension_outranks_larger_later_dimension_sum(data: st.DrawFn) -> None:
    # Choose which dimension is the deciding one. Restrict to 0..2 so at least
    # one strictly-less-significant dimension always remains for the loser to
    # inflate, making its (incorrect) sum strictly larger than the winner's.
    deciding = data.draw(st.integers(min_value=0, max_value=2))
    # Equal, arbitrary values for the strictly-more-significant dimensions so the
    # deciding dimension is what actually breaks the tie.
    prefix = [data.draw(_SMALL_INT) for _ in range(deciding)]
    loser_value = data.draw(st.integers(min_value=0, max_value=5))
    big = 1000  # dominates any small dimension when (incorrectly) summed.

    # Winner: equal prefix, deciding dimension one greater, later dimensions zero.
    winner_dims = tuple(prefix + [loser_value + 1] + [0] * (3 - deciding))
    # Loser: equal prefix, deciding dimension one less, later dimensions maximal.
    loser_dims = tuple(prefix + [loser_value] + [big] * (3 - deciding))

    # A summation-based scorer would rank the loser first; a lexicographic one
    # must not: the loser's total is strictly larger, yet it must rank second.
    assert sum(loser_dims) > sum(winner_dims)

    winner = _make_gap("gap-prop20-winner", winner_dims)
    loser = _make_gap("gap-prop20-loser", loser_dims)

    ranked = rank_gaps([loser, winner])
    assert str(ranked[0].id) == "gap-prop20-winner"
    assert str(ranked[1].id) == "gap-prop20-loser"


# --------------------------------------------------------------------------- #
# Stable identifier is the final tie-break (Requirement 12.4).                #
# --------------------------------------------------------------------------- #


# **Validates: Requirements 12.4**
@given(
    dims=st.tuples(_SMALL_INT, _SMALL_INT, _SMALL_INT, _SMALL_INT),
    count=st.integers(min_value=2, max_value=6),
)
@settings(max_examples=200, deadline=None, print_blob=True)
def test_stable_id_breaks_priority_ties(dims: tuple[int, int, int, int], count: int) -> None:
    # Every gap shares identical priority dimensions, so only the stable
    # identifier can order them.
    ids = [f"gap-prop20-tie-{index:03d}" for index in range(count)]
    gaps = [_make_gap(gap_id, dims) for gap_id in ids]

    ranked = rank_gaps(gaps)

    # Ties resolve by ascending stable identifier.
    assert [str(gap.id) for gap in ranked] == sorted(ids)
