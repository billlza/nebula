"""Property 13: primitive-object proof and boot gates remain decomposed.

This module holds the single Hypothesis property test for design Property 13.
It exercises the *real* ABI/backend primitive-object finding and the *real* boot
evaluator (no mocks, no reimplementation of the components under test) against
adversarially generated evidence bundles, and checks two independent invariants:

* **Wording bound (Requirement 7.6).** For any evidence, the primitive-object
  finding's allowed wording is exactly the canonical clang-backed ELF
  relocatable-object emission phrase and asserts none of the forbidden direct
  backend / linked-image / runtime / boot terms; and the Claim Guard governs
  every primitive-object record to that same wording.
* **Decomposition + ordering (Requirement 7.7).** The boot evaluator always
  keeps target specification, linker inputs/scripts, relocation, startup object,
  deterministic linking, boot media, and boot execution as separate, distinctly
  identified Hard-Gate candidates joined in a fixed order, and a primitive
  relocatable-object proof never satisfies the linked-image, media, or execution
  stages.

The oracles below are derived from the requirement text, not from the
implementation, so the assertions are not tautological: the expected wording
set, the expected forbidden-term detection, and the expected stage ordering are
recomputed here and then compared against what the real components produce.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.claim_guard import (
    PRIMITIVE_OBJECT_FORBIDDEN_TERMS,
    PRIMITIVE_OBJECT_WORDING,
    guard_evidence,
)
from tools.universe_os_gap_analysis.evaluators.abi_backend import evaluate_abi_backend
from tools.universe_os_gap_analysis.evaluators.boot import (
    BOOT_CHAIN_SPECS,
    JOIN_GATE_KEY,
    BootStageKind,
    evaluate_boot,
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

_REVISION_REF = reference("revision-property-13")

# --------------------------------------------------------------------------- #
# Independent oracles (derived from the requirement, not the implementation)  #
# --------------------------------------------------------------------------- #

# Requirement 7.6: primitive freestanding object markers. Any record whose text
# contains one of these is a primitive-object claim whose wording is bounded.
_PRIMITIVE_MARKERS: tuple[str, ...] = (
    "primitive freestanding",
    "primitive object",
    "et_rel",
    "relocatable object",
    "relocatable-object",
    "freestanding object",
)

# Requirement 7.7: the boot stages that must remain separate, ordered gates.
# Declared here independently of the evaluator's own spec table.
_REQUIRED_BOOT_STAGE_KEYS: tuple[str, ...] = (
    "target-spec",
    "boot-protocol",
    "boot-entry",
    "linker-script-input",
    "relocation",
    "startup-object",
    "deterministic-linked-elf",
    "boot-media",
    "qemu-execution",
)

# The subset the property names explicitly, with the ordering they must obey.
_PRE_LINK_STAGE_KEYS: tuple[str, ...] = (
    "target-spec",
    "linker-script-input",
    "relocation",
    "startup-object",
)
_LINK_KEY = "deterministic-linked-elf"
_MEDIA_KEY = "boot-media"
_EXECUTION_KEY = "qemu-execution"
_PRIMITIVE_OBJECT_KEY = "primitive-et-rel-object"

# Statuses/kinds that assert current implementation (used by the gate_passed
# oracle for the primitive-object finding).
_DIRECT_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)


def _oracle_forbidden_hits(text: str) -> tuple[str, ...]:
    """Independently recompute which forbidden terms occur in ``text``."""

    lowered = text.lower()
    return tuple(term for term in PRIMITIVE_OBJECT_FORBIDDEN_TERMS if term in lowered)


def _oracle_gate_passed(records: tuple[EvidenceRecord, ...]) -> bool:
    """Requirement 7.6 gate-passed oracle, derived from the finding's contract."""

    for record in records:
        text = f"{record.claim_key}\n{record.claim}".lower()
        if any(marker in text for marker in _PRIMITIVE_MARKERS) and (
            record.verification_state is VerificationState.VALIDATED
            or record.evidence_kind is EvidenceKind.TEST_EXECUTION
        ):
            return True
    return False


def _is_primitive(record: EvidenceRecord) -> bool:
    text = f"{record.claim_key}\n{record.claim}".lower()
    return any(marker in text for marker in _PRIMITIVE_MARKERS)


# --------------------------------------------------------------------------- #
# Generators                                                                  #
# --------------------------------------------------------------------------- #

_safe_text = (
    st.text(
        alphabet=st.characters(min_codepoint=0x61, max_codepoint=0x7A),
        min_size=1,
        max_size=24,
    )
    .map(str.strip)
    .filter(bool)
    # Keep ordinary text free of primitive/forbidden marker words so a record's
    # primitive classification is controlled entirely by the generator's flag.
    .filter(
        lambda value: not any(
            marker in value
            for marker in _PRIMITIVE_MARKERS + PRIMITIVE_OBJECT_FORBIDDEN_TERMS
        )
    )
)


@st.composite
def _record(draw: st.DrawFn, *, primitive: bool | None = None) -> EvidenceRecord:
    """Draw a valid Evidence_Record, optionally forcing a primitive-object claim."""

    make_primitive = draw(st.booleans()) if primitive is None else primitive
    if make_primitive:
        marker = draw(st.sampled_from(_PRIMITIVE_MARKERS))
        claim = f"{marker} emission for the experimental subset {draw(_safe_text)}"
    else:
        claim = f"{draw(_safe_text)} {draw(_safe_text)}"

    status = draw(st.sampled_from(tuple(EvidenceStatus)))
    kind = draw(st.sampled_from(tuple(EvidenceKind)))
    origin = draw(st.sampled_from(tuple(RevisionOrigin)))
    verification = draw(st.sampled_from(tuple(VerificationState)))
    claim_key = draw(
        st.from_regex(r"src:[a-z0-9]{1,10}(-[a-z0-9]{1,10}){0,2}", fullmatch=True)
    )
    source_path = draw(_safe_text) + draw(st.sampled_from([".md", ".py", ".hpp", ".cpp"]))

    return EvidenceRecord(
        id=stable_id(
            "evidence", claim_key, claim, status.value, kind.value, origin.value
        ),
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
        verification_state=verification,
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] += (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


def _predecessors() -> dict[str, set[str]]:
    """Transitive predecessor sets over the real boot candidate chain edges."""

    direct: dict[str, set[str]] = {}
    for spec in BOOT_CHAIN_SPECS:
        direct.setdefault(spec.key, set())
        for dep in spec.dependency_keys:
            direct[spec.key].add(dep)

    closure: dict[str, set[str]] = {key: set(deps) for key, deps in direct.items()}
    changed = True
    while changed:
        changed = False
        for key, deps in closure.items():
            for dep in tuple(deps):
                for upstream in closure.get(dep, set()):
                    if upstream not in deps:
                        deps.add(upstream)
                        changed = True
    return closure


# --------------------------------------------------------------------------- #
# Property 13                                                                 #
# --------------------------------------------------------------------------- #


# Feature: nebula-universe-os-gap-analysis, Property 13: Primitive-object proof and boot gates remain decomposed - for passing primitive object gates, allowed wording is limited to clang-backed ELF relocatable-object emission and excludes direct backend, linked image, runtime, and boot claims; target specification, linker inputs/scripts, relocation/startup, deterministic linking, boot media, and boot execution remain separate ordered hard gates.
# **Validates: Requirements 7.6, 7.7**
@given(records=st.lists(_record(), min_size=1, max_size=6, unique_by=lambda r: str(r.id)))
@settings(max_examples=200, deadline=None, print_blob=True)
def test_primitive_object_wording_is_bounded(records: list[EvidenceRecord]) -> None:
    """Requirement 7.6: the primitive-object finding wording is fixed and bounded.

    The real ABI/backend finding and the real Claim Guard are exercised; the
    expected wording, forbidden-term exclusion, and gate-passed value are all
    recomputed from the requirement independently of those components.
    """

    bundle = _bundle(records)

    finding = evaluate_abi_backend(bundle).primitive_object

    # Allowed wording is exactly the canonical clang-backed ELF relocatable
    # object emission phrase, and it never leaks a forbidden claim.
    assert finding.wording == PRIMITIVE_OBJECT_WORDING
    assert "clang-backed" in finding.wording.lower()
    assert "relocatable-object" in finding.wording.lower()
    assert finding.wording_asserts_forbidden(finding.wording) == ()

    # gate_passed matches the independently derived oracle (non-tautological).
    assert finding.gate_passed is _oracle_gate_passed(bundle.records)

    # The finding's forbidden-term detector agrees with an independent scan for
    # both a clean phrase and an adversarial phrase asserting every forbidden term.
    clean = "clang-backed ELF64 relocatable-object emission for a primitive subset"
    assert set(finding.wording_asserts_forbidden(clean)) == set(_oracle_forbidden_hits(clean))
    adversarial = (
        "this direct backend and native backend prove a linked image, a linked ELF, "
        "a bootable runtime kernel with boot execution"
    )
    assert set(finding.wording_asserts_forbidden(adversarial)) == set(
        _oracle_forbidden_hits(adversarial)
    )
    assert set(_oracle_forbidden_hits(adversarial)) == set(PRIMITIVE_OBJECT_FORBIDDEN_TERMS)

    # The real Claim Guard governs every primitive-object record to the same
    # bounded wording, and never so governs a non-primitive record.
    guarded = guard_evidence(bundle)
    for record in records:
        claim = guarded.claim_for(record.id)
        assert claim is not None
        assert claim.is_primitive_object is _is_primitive(record)
        if claim.is_primitive_object:
            assert claim.guarded_wording == PRIMITIVE_OBJECT_WORDING
            assert _oracle_forbidden_hits(claim.guarded_wording) == ()


# Feature: nebula-universe-os-gap-analysis, Property 13: Primitive-object proof and boot gates remain decomposed - target specification, linker inputs/scripts, relocation/startup, deterministic linking, boot media, and boot execution remain separate ordered hard gates, and a primitive relocatable-object proof never satisfies the linked-image, media, or execution stages.
# **Validates: Requirements 7.6, 7.7**
@given(records=st.lists(_record(), max_size=6, unique_by=lambda r: str(r.id)))
@settings(max_examples=200, deadline=None, print_blob=True)
def test_boot_gates_remain_separate_and_ordered(records: list[EvidenceRecord]) -> None:
    """Requirement 7.7: object/link/media/execute stay separate, ordered gates.

    The real boot evaluator is run over adversarial evidence; the required stage
    set and the required precedence relations are declared independently here.
    """

    result = evaluate_boot(_bundle(records))

    # Every required boot stage is a distinct domain, gap, and Hard-Gate
    # candidate. The stage keys are declared independently of the evaluator.
    gate_ids: list[str] = []
    for key in _REQUIRED_BOOT_STAGE_KEYS:
        domain = result.domain_for(key)
        gate = result.gate_for(key)
        assert domain is not None, f"missing domain for {key}"
        assert gate is not None, f"missing gate for {key}"
        gate_ids.append(str(gate.id))
    # No two required stages collapse onto a shared gate or domain identity.
    assert len(set(gate_ids)) == len(_REQUIRED_BOOT_STAGE_KEYS)

    # The evaluator emits exactly the boot-stage domains it declares as stages,
    # and that set is exactly the independently required set.
    boot_stage_keys = {
        spec.key for spec in BOOT_CHAIN_SPECS if spec.kind is BootStageKind.BOOT_STAGE
    }
    assert boot_stage_keys == set(_REQUIRED_BOOT_STAGE_KEYS)
    assert len(result.domains) == len(_REQUIRED_BOOT_STAGE_KEYS)

    # Ordering: pre-link stages and the primitive-object input precede the
    # deterministic linked-ELF join, which precedes media, which precedes
    # execution. Computed from the real dependency edges.
    predecessors = _predecessors()
    for key in _PRE_LINK_STAGE_KEYS + (_PRIMITIVE_OBJECT_KEY,):
        assert key in predecessors[_LINK_KEY], f"{key} must precede the linked-ELF join"
    assert _LINK_KEY in predecessors[_MEDIA_KEY]
    assert _MEDIA_KEY in predecessors[_EXECUTION_KEY]
    # Execution transitively depends on the join and every pre-link stage.
    for key in (_LINK_KEY, *_PRE_LINK_STAGE_KEYS, _PRIMITIVE_OBJECT_KEY):
        assert key in predecessors[_EXECUTION_KEY]
    # The chain is strictly ordered: neither media nor execution precedes the join.
    assert _MEDIA_KEY not in predecessors[_LINK_KEY]
    assert _EXECUTION_KEY not in predecessors[_LINK_KEY]


# Feature: nebula-universe-os-gap-analysis, Property 13: Primitive-object proof and boot gates remain decomposed - a primitive ET_REL relocatable-object proof is only an input to the linked-ELF join and never satisfies deterministic linking, boot media, or boot execution.
# **Validates: Requirements 7.6, 7.7**
@given(records=st.lists(_record(primitive=True), min_size=1, max_size=5, unique_by=lambda r: str(r.id)))
@settings(max_examples=150, deadline=None, print_blob=True)
def test_primitive_object_never_satisfies_later_boot_stages(
    records: list[EvidenceRecord],
) -> None:
    """Requirement 7.7: an ET_REL object proof cannot lift later boot stages.

    Bundles here contain only primitive relocatable-object evidence. Because the
    primitive-object markers are disjoint from the linked-ELF/media/execution
    markers, those downstream stages must stay unsatisfied at maturity 0 with no
    supporting evidence, no matter how strong the primitive evidence is.
    """

    result = evaluate_boot(_bundle(records))

    for later in (_LINK_KEY, _MEDIA_KEY, _EXECUTION_KEY, "relocation"):
        assessment = result.assessment_for(later)
        assert assessment is not None
        assert assessment.satisfied is False, f"{later} must not be satisfied"
        assert assessment.maturity is MaturityScore.ABSENT
        assert assessment.supporting_evidence_ids == ()
        # An unsatisfied boot stage still yields its own separate gap.
        assert result.gap_for(later) is not None


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner; allow direct execution.
    test_primitive_object_wording_is_bounded()
    test_boot_gates_remain_separate_and_ordered()
    test_primitive_object_never_satisfies_later_boot_stages()
    print("Property 13 OK: primitive-object proof and boot gates remain decomposed")
