"""Property-based test for Property 14 (Task 6.6).

Feature: nebula-universe-os-gap-analysis, Property 14: Library layers and
preview statuses do not collapse - future `core`, hosted `std`, and future
`system` stay three separate capability domains; a `core::`/`system::` import
with missing resolver or implementation support classifies as `Planned`; and
`Installed_Preview`/`Repo_Preview` statuses survive summaries and target-level
calculations unchanged (never dropped, promoted, or collapsed).

These tests drive the *real* ``evaluate_runtime_library_package`` evaluator and
the *real* ``classify_core_system_import`` classifier over Hypothesis-generated
evidence bundles and import-support combinations. Nothing is mocked and no
result is asserted against a re-implementation of the rule, so the checks are
non-tautological: the evaluator/classifier compute the outcome and the property
constrains it.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.evaluators.runtime_library_package import (
    LibraryLayer,
    classify_core_system_import,
    evaluate_runtime_library_package,
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
    RevisionOrigin,
    SourceLocation,
    VerificationState,
)

_REVISION_REF = reference("revision-property-14")

# The two preview statuses that Requirement 8.6 says must survive unchanged.
_PREVIEW_STATUSES: tuple[EvidenceStatus, ...] = (
    EvidenceStatus.INSTALLED_PREVIEW,
    EvidenceStatus.REPO_PREVIEW,
)

# Statuses strictly weaker than a preview status in the evaluator's strength
# ranking. A preview record mixed with any of these must remain the strongest
# observed status (i.e. the preview is never collapsed down to a weaker one).
_WEAKER_THAN_PREVIEW: tuple[EvidenceStatus, ...] = (
    EvidenceStatus.EXPERIMENTAL,
    EvidenceStatus.PLANNED,
    EvidenceStatus.UNSUPPORTED,
    EvidenceStatus.UNKNOWN,
)

# GA statuses a preview must never be silently promoted into.
_GA_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {EvidenceStatus.COMPILER_TOOLING_GA, EvidenceStatus.BACKEND_SDK_GA}
)

# Package facets keyed by their capability id, each paired with a marker string
# that uniquely selects that facet during the evaluator's substring detection.
_FACET_MARKERS: tuple[tuple[str, str], ...] = (
    ("capability-package-manifest", "nebula.toml"),
    ("capability-package-workspace", "workspace"),
    ("capability-package-lock", "nebula.lock"),
    ("capability-package-local-registry", "local registry"),
    ("capability-package-hosted-registry", "remote registry"),
    ("capability-package-git-dependency", "git+"),
    ("capability-package-native-dependency", "native library"),
    ("capability-package-reproducibility", "reproducible build"),
    ("capability-package-signing", "signed package"),
    ("capability-package-vulnerability-response", "advisory"),
    ("capability-package-compatibility", "semver"),
    ("capability-package-offline", "air-gapped"),
)

_CORE_SYSTEM_PREFIX = st.sampled_from(["core::", "system::"])
_MODULE_TAIL = st.text(
    alphabet="abcdefghijklmnopqrstuvwxyz_", min_size=1, max_size=12
)


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "spec/library_layers.md",
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value, evidence_kind.value),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=evidence_kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(records: tuple[EvidenceRecord, ...]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key[record.claim_key] = by_claim_key.get(record.claim_key, ()) + (record,)
    return EvidenceBundle(records=records, by_claim_key=by_claim_key)


@st.composite
def _arbitrary_evidence(draw: st.DrawFn) -> tuple[EvidenceRecord, ...]:
    """Generate a small bundle of records mentioning the library layers.

    The layer keywords (``core``/``std``/``system``) are seeded so the evaluator
    has evidence to match; statuses are adversarial across the full status set.
    """

    templates = draw(
        st.lists(
            st.tuples(
                st.sampled_from(
                    [
                        "The future core:: layer is discussed.",
                        "The hosted std standard library is described.",
                        "The future system:: layer is planned.",
                        "core::, hosted std, and system:: layers are documented.",
                        "A bundled std library on the host OS.",
                    ]
                ),
                st.sampled_from(tuple(EvidenceStatus)),
                st.sampled_from(tuple(EvidenceKind)),
            ),
            min_size=0,
            max_size=6,
        )
    )
    records = tuple(
        _record(
            claim_key=f"claim:{index}",
            claim=claim,
            status=status,
            evidence_kind=kind,
        )
        for index, (claim, status, kind) in enumerate(templates)
    )
    return records


# Feature: nebula-universe-os-gap-analysis, Property 14: Library layers and
# preview statuses do not collapse - future core, hosted std, and future system
# always resolve to three distinct capability domains regardless of evidence.
# **Validates: Requirements 8.3, 8.4, 8.6**
@given(records=_arbitrary_evidence())
@settings(max_examples=100, deadline=None, print_blob=True)
def test_library_layers_remain_three_separate_domains(
    records: tuple[EvidenceRecord, ...],
) -> None:
    """The real evaluator keeps core/std/system as three distinct domains."""

    result = evaluate_runtime_library_package(_bundle(records))

    layer_ids = result.layer_domain_ids()
    core_id = layer_ids[LibraryLayer.FUTURE_CORE]
    std_id = layer_ids[LibraryLayer.HOSTED_STD]
    system_id = layer_ids[LibraryLayer.FUTURE_SYSTEM]

    # Three separate domains: their ids never collapse into one another.
    assert len({core_id, std_id, system_id}) == 3

    # Each layer is materialized as its own draft (a distinct CapabilityDomain).
    for capability_id in (core_id, std_id, system_id):
        draft = result.draft_for(capability_id)
        assert draft is not None
        assert str(draft.domain.id) == capability_id


# Feature: nebula-universe-os-gap-analysis, Property 14: Library layers and
# preview statuses do not collapse - a core::/system:: import with either
# resolver or implementation support absent is classified Planned; only when
# both are present is it non-Planned (Experimental).
# **Validates: Requirements 8.3, 8.4, 8.6**
@given(
    prefix=_CORE_SYSTEM_PREFIX,
    tail=_MODULE_TAIL,
    resolver_support=st.booleans(),
    implementation_support=st.booleans(),
)
@settings(max_examples=100, deadline=None, print_blob=True)
def test_core_system_import_planned_unless_both_supports_present(
    prefix: str,
    tail: str,
    resolver_support: bool,
    implementation_support: bool,
) -> None:
    """Exercise the real classifier across all resolver/implementation combos."""

    module_path = f"{prefix}{tail}"
    status = classify_core_system_import(
        module_path,
        resolver_support=resolver_support,
        implementation_support=implementation_support,
    )

    if resolver_support and implementation_support:
        # Both present: the import is a current (experimental) implementation,
        # never silently downgraded and never treated as GA.
        assert status is not EvidenceStatus.PLANNED
        assert status is EvidenceStatus.EXPERIMENTAL
    else:
        # Either support missing => Planned. Missing support cannot be papered
        # over into an implemented status.
        assert status is EvidenceStatus.PLANNED


@st.composite
def _preview_bundle(draw: st.DrawFn):
    """Generate a facet + preview status, plus optional weaker sibling records.

    The preview record and any weaker siblings all target the *same* facet so we
    can assert the preview is neither dropped nor collapsed to a weaker status.
    """

    capability_id, marker = draw(st.sampled_from(_FACET_MARKERS))
    preview_status = draw(st.sampled_from(_PREVIEW_STATUSES))
    preview_kind = draw(st.sampled_from(tuple(EvidenceKind)))
    weaker_statuses = draw(
        st.lists(st.sampled_from(_WEAKER_THAN_PREVIEW), min_size=0, max_size=3)
    )

    records = [
        _record(
            claim_key=f"preview:{capability_id}",
            claim=f"The {marker} facet is provided as a preview package.",
            status=preview_status,
            evidence_kind=preview_kind,
            source_path="docs/official_package_tiering.md",
        )
    ]
    for index, status in enumerate(weaker_statuses):
        records.append(
            _record(
                claim_key=f"weaker:{capability_id}:{index}",
                claim=f"Additional {marker} facet notes.",
                status=status,
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path="docs/official_package_tiering.md",
            )
        )
    return capability_id, preview_status, tuple(records)


# Feature: nebula-universe-os-gap-analysis, Property 14: Library layers and
# preview statuses do not collapse - Installed_Preview/Repo_Preview survive the
# evaluator's summary and per-domain calculations unchanged, never promoted to a
# GA status nor collapsed to a weaker sibling status.
# **Validates: Requirements 8.3, 8.4, 8.6**
@given(data=_preview_bundle())
@settings(max_examples=100, deadline=None, print_blob=True)
def test_preview_statuses_survive_summaries_unchanged(data) -> None:
    """The real evaluator preserves preview statuses in summaries and drafts."""

    capability_id, preview_status, records = data
    result = evaluate_runtime_library_package(_bundle(records))

    # The summary of preserved preview statuses carries the injected status.
    assert preview_status in result.preserved_preview_statuses
    # It is only ever a genuine preview status, never promoted to GA.
    for status in result.preserved_preview_statuses:
        assert status in _PREVIEW_STATUSES
        assert status not in _GA_STATUSES

    draft = result.draft_for(capability_id)
    assert draft is not None
    # The per-domain observed status stays exactly the preview status: the
    # preview is neither promoted to GA nor collapsed down to a weaker sibling.
    assert draft.observed_status is preview_status
    assert draft.observed_status not in _GA_STATUSES
    # The preview status is retained in the draft's preview-status list.
    assert preview_status in draft.preview_statuses
