"""Gap register generation and one-primary-category validation (Task 9.1).

Every domain evaluator (Tasks 5.x-7.x) emits
:class:`~tools.universe_os_gap_analysis.models.GapEntry` values as part of its
draft output, and the preview-security obligation generator (Task 7.5) emits
ecosystem obligation gaps. This module is the single place that *aggregates*
those heterogeneous evaluator outputs into one canonical gap register and
enforces, register-wide, the classification invariants mandated by
Requirements 12.1-12.3 (and the language/verification classification of
Requirements 5.3, 5.4 and the ecosystem obligations of Requirement 9.8):

* every gap carries **exactly one** primary ``Gap_Category`` (Requirement 12.1);
* secondary ``Gap_Category`` labels are **deduplicated** and never repeat the
  primary category (Requirement 12.2); and
* every gap records the full Requirement 12.3 field set -- affected capability
  domains, current ``Evidence_Status``, target ``Target_Level``, severity,
  dependencies, acceptance evidence, recommended owner area, observed fact, and
  recommendation.

The ``Gap_Category`` closed set (``Language_Gap``, ``Implementation_Gap``,
``Verification_Gap``, ``Ecosystem_Gap``) is the classification vocabulary; this
register keeps gaps of all four kinds side by side and exposes accessors that
group them by primary category so downstream stages can confirm coverage of
every evaluator-produced gap.

Scope boundaries (owned by later tasks): this module does **not** rank or sort
gaps by priority beyond a stable identifier ordering (Task 9.2 owns the
dependency-criticality/safety/claim-risk/target-unblock lexicographic ranking
and the roadmap/gate frontier), does not assign maturity (Task 8), and renders
nothing (Task 11). It is read-only: it never mutates a gap, upgrades a status,
or edits any evaluator or product module. It fails closed with a ``GAP-*`` code
on any structural problem so an invalid register can never be published.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

from .identifiers import ReferenceId, reference
from .models import GapCategory, GapEntry

# --------------------------------------------------------------------------- #
# GAP-* error codes (fail closed; evaluators already built each GapEntry).     #
# --------------------------------------------------------------------------- #
GAP_DUPLICATE_ID = "GAP-DUPLICATE-ID"
GAP_INVALID_PRIMARY = "GAP-INVALID-PRIMARY"
GAP_SECONDARY_REPEATS_PRIMARY = "GAP-SECONDARY-REPEATS-PRIMARY"
GAP_SECONDARY_DUPLICATE = "GAP-SECONDARY-DUPLICATE"
GAP_MISSING_FIELD = "GAP-MISSING-FIELD"

# The complete closed classification vocabulary (Requirement 12.1).
ALL_GAP_CATEGORIES: tuple[GapCategory, ...] = tuple(GapCategory)


class GapRegisterError(ValueError):
    """A fail-closed gap-register error carrying a ``GAP-*`` code and object refs."""

    def __init__(self, code: str, message: str, object_refs: Iterable[str] = ()) -> None:
        self.code = code
        self.object_refs = tuple(sorted({str(ref) for ref in object_refs}))
        detail = ", ".join(self.object_refs)
        suffix = f" [{detail}]" if detail else ""
        super().__init__(f"{code}: {message}{suffix}")


@dataclass(frozen=True, slots=True)
class GapRegister:
    """The canonical, validated, deterministically ordered gap register.

    ``gaps`` is sorted by stable identifier so the register is order-independent
    with respect to how evaluators were collected. Every gap has already been
    validated for the one-primary-category invariant and the Requirement 12.3
    field set before it enters the register (see :func:`build_gap_register`).
    """

    gaps: tuple[GapEntry, ...] = ()

    def __post_init__(self) -> None:
        gaps = tuple(self.gaps)
        if not all(isinstance(gap, GapEntry) for gap in gaps):
            raise TypeError("gaps must contain GapEntry values")
        object.__setattr__(self, "gaps", tuple(sorted(gaps, key=lambda gap: str(gap.id))))

    def gap_for(self, gap_id: str) -> GapEntry | None:
        """Return the gap with ``gap_id`` or ``None`` if it is not registered."""

        target = str(gap_id)
        for gap in self.gaps:
            if str(gap.id) == target:
                return gap
        return None

    def by_primary_category(self, category: GapCategory) -> tuple[GapEntry, ...]:
        """Return every gap whose *primary* category is ``category``."""

        if not isinstance(category, GapCategory):
            raise TypeError("category must be a GapCategory")
        return tuple(gap for gap in self.gaps if gap.primary_category is category)

    def for_domain(self, domain_id: str) -> tuple[GapEntry, ...]:
        """Return every gap that affects the capability domain ``domain_id``."""

        target = str(domain_id)
        return tuple(
            gap for gap in self.gaps if target in {str(ref) for ref in gap.domain_ids}
        )

    def primary_category_counts(self) -> dict[GapCategory, int]:
        """Return the number of gaps per primary category (all four keys present)."""

        counts = {category: 0 for category in ALL_GAP_CATEGORIES}
        for gap in self.gaps:
            counts[gap.primary_category] += 1
        return counts

    def categories_present(self) -> frozenset[GapCategory]:
        """Return the set of primary categories actually represented in the register."""

        return frozenset(gap.primary_category for gap in self.gaps)

    @property
    def gap_ids(self) -> tuple[ReferenceId, ...]:
        """Return the stable identifiers of every registered gap, in order."""

        return tuple(reference(gap.id) for gap in self.gaps)

    def __len__(self) -> int:
        return len(self.gaps)


# --------------------------------------------------------------------------- #
# Collection from heterogeneous evaluator outputs.                             #
# --------------------------------------------------------------------------- #


def gaps_from_sources(sources: Iterable[object]) -> tuple[GapEntry, ...]:
    """Extract :class:`GapEntry` values from heterogeneous evaluator outputs.

    Accepts, in any mix:

    * a bare :class:`GapEntry`;
    * any evaluator draft/evaluation exposing a ``gaps`` tuple
      (``DomainDraft`` and every ``*Evaluation`` result type); or the
      preview-security generator exposing ``obligation_gaps``;
    * any iterable of the above (including nested iterables).

    This lets the register cover *all* evaluator-produced gaps under the four
    classification rules without each evaluator sharing a common base class.
    A source that yields no gaps is skipped; a source that is neither a
    ``GapEntry``, a gap-bearing object, nor an iterable raises ``TypeError``.
    """

    collected: list[GapEntry] = []
    for source in sources:
        collected.extend(_extract_gaps(source))
    return tuple(collected)


def _extract_gaps(source: object) -> tuple[GapEntry, ...]:
    if isinstance(source, GapEntry):
        return (source,)
    # Evaluator drafts/evaluations expose their classified gaps as a tuple. The
    # preview-security generator names its collection ``obligation_gaps``.
    for attribute in ("gaps", "obligation_gaps"):
        candidate = getattr(source, attribute, None)
        if candidate is not None:
            values = tuple(candidate)
            if not all(isinstance(value, GapEntry) for value in values):
                raise TypeError(
                    f"{type(source).__name__}.{attribute} must contain GapEntry values"
                )
            return values
    if isinstance(source, (str, bytes)):
        raise TypeError(f"cannot extract gaps from {type(source).__name__}")
    if isinstance(source, Iterable):
        collected: list[GapEntry] = []
        for item in source:
            collected.extend(_extract_gaps(item))
        return tuple(collected)
    raise TypeError(f"cannot extract gaps from {type(source).__name__}")


# --------------------------------------------------------------------------- #
# Register generation and one-primary-category validation.                     #
# --------------------------------------------------------------------------- #


def _validate_one_primary_category(gap: GapEntry) -> None:
    """Re-affirm the one-primary-category invariant for a single gap.

    ``GapEntry`` enforces these rules at construction, but the register is the
    authoritative fail-closed classification gate (Requirements 12.1, 12.2), so
    it re-validates every gap independently and reports a ``GAP-*`` code with the
    offending gap identifier rather than assuming construction was well formed.
    """

    if not isinstance(gap.primary_category, GapCategory):
        raise GapRegisterError(
            GAP_INVALID_PRIMARY,
            "gap primary_category must be exactly one Gap_Category",
            (gap.id,),
        )
    secondary = tuple(gap.secondary_categories)
    if any(not isinstance(category, GapCategory) for category in secondary):
        raise GapRegisterError(
            GAP_INVALID_PRIMARY,
            "gap secondary_categories must contain Gap_Category values",
            (gap.id,),
        )
    if gap.primary_category in secondary:
        raise GapRegisterError(
            GAP_SECONDARY_REPEATS_PRIMARY,
            "gap secondary_categories must not repeat the primary category",
            (gap.id,),
        )
    if len(secondary) != len(set(secondary)):
        raise GapRegisterError(
            GAP_SECONDARY_DUPLICATE,
            "gap secondary_categories must be unique",
            (gap.id,),
        )


def _validate_required_fields(gap: GapEntry) -> None:
    """Re-affirm the Requirement 12.3 field set is present for a single gap."""

    if not gap.domain_ids:
        raise GapRegisterError(
            GAP_MISSING_FIELD,
            "gap must record at least one affected capability domain",
            (gap.id,),
        )
    if not gap.acceptance_evidence:
        raise GapRegisterError(
            GAP_MISSING_FIELD,
            "gap must record acceptance evidence",
            (gap.id,),
        )
    for name in ("recommended_owner_area", "observed_fact", "recommendation"):
        value = getattr(gap, name)
        if not isinstance(value, str) or not value.strip():
            raise GapRegisterError(
                GAP_MISSING_FIELD,
                f"gap must record a non-empty {name}",
                (gap.id,),
            )


def build_gap_register(*sources: object) -> GapRegister:
    """Aggregate evaluator gaps into a validated, deduplicated gap register.

    Each argument may be a :class:`GapEntry`, a gap-bearing evaluator output, or
    an iterable of those (see :func:`gaps_from_sources`). The register:

    * re-validates the one-primary-category invariant (Requirements 12.1, 12.2)
      and the Requirement 12.3 field set for every gap, failing closed with a
      ``GAP-*`` code that names the offending gap;
    * deduplicates gaps that are *identical* (same stable identifier and equal
      content), because independent evaluators may legitimately surface the same
      gap; and
    * fails closed with ``GAP-DUPLICATE-ID`` when two *different* gaps share a
      stable identifier, since a colliding identifier would corrupt every
      downstream reference.

    The returned :class:`GapRegister` is ordered by stable identifier, so the
    result is independent of the order in which evaluators were collected.
    """

    gaps = gaps_from_sources(sources)

    by_id: dict[str, GapEntry] = {}
    for gap in gaps:
        _validate_one_primary_category(gap)
        _validate_required_fields(gap)
        gap_id = str(gap.id)
        existing = by_id.get(gap_id)
        if existing is None:
            by_id[gap_id] = gap
            continue
        if existing != gap:
            raise GapRegisterError(
                GAP_DUPLICATE_ID,
                "two distinct gaps share a stable identifier",
                (gap_id,),
            )
        # Identical duplicate: keep the single canonical entry.

    return GapRegister(gaps=tuple(by_id.values()))
