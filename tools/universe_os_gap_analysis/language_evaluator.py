"""Language-semantics and type-system declarative evaluator (Task 5.1).

This evaluator encodes Requirements 5.1-5.5 as a reusable
:class:`~tools.universe_os_gap_analysis.evaluator.DeclarativeChecklist` and runs
it through the shared :class:`~tools.universe_os_gap_analysis.evaluator.ChecklistEvaluator`.
It builds on the evidence (Task 4.1) and Claim Guard (Task 4.3) layers, consumes
:class:`~tools.universe_os_gap_analysis.models.EvidenceRecord` values via an
:class:`~tools.universe_os_gap_analysis.evidence.EvidenceBundle`, and emits a
:class:`~tools.universe_os_gap_analysis.evaluator.DomainDraft` whose
:class:`~tools.universe_os_gap_analysis.models.CapabilityDomain` and
:class:`~tools.universe_os_gap_analysis.models.GapEntry` values feed the later
Maturity Assessor and gap register.

The checklist keeps three semantic layers strictly separate for every feature:

* **Specification** -- the authoritative spec prose that documents the feature.
* **Parser/typechecker implementation** -- the ``frontend/``, ``passes/``, and
  ``nir/`` entries that implement it.
* **Compatibility policy** -- the stability/compatibility contract that governs
  it across revisions.

Coverage:

* Requirement 5.1: lexical rules, control flow, functions, methods, modules,
  visibility, generics, traits/protocols, closures, patterns, error effects,
  reflection, macros, and metaprogramming.
* Requirement 5.2: primitive widths, pointers, references, slices, arrays,
  collections, nullable values, aggregates, enums, callable types, variance,
  lifetimes, constrained generics, and dynamic dispatch.
* Requirement 5.3: every documented feature produces a ``Language_Gap`` that
  references the authoritative source path and any direct implementation
  evidence.
* Requirement 5.4: a feature with parser/typechecker support but no
  compatibility policy produces a semantic-stability ``Verification_Gap``.
* Requirement 5.5: low-level semantic prerequisites -- target layout,
  initialization, destruction, aliasing, and system-call boundaries.
"""

from __future__ import annotations

from .claim_guard import GuardedEvidence
from .evaluator import (
    ChecklistEvaluator,
    ChecklistItem,
    DeclarativeChecklist,
    DomainDraft,
)
from .evidence import EvidenceBundle
from .models import EvidenceKind, TargetLevel

# Semantic aspects the checklist is partitioned into.
ASPECT_LANGUAGE_SEMANTICS = "language-semantics"
ASPECT_TYPE_SYSTEM = "type-system"
ASPECT_LOW_LEVEL_SEMANTICS = "low-level-semantics"

# Authoritative specification sources (all real repository paths).
_SPEC_GRAMMAR = "spec/grammar.ebnf"
_SPEC_CORE = "spec/language_core.md"
_SPEC_REFERENCE = "spec/language_reference.md"
_SPEC_TYPE_SYSTEM = "spec/type_system.md"
_SPEC_GENERICS = "spec/generics_policy.md"
_SPEC_ABI_LAYOUT = "spec/abi_layout.md"
_SPEC_INTEROP = "spec/interop_c_abi.md"
_SPEC_OWNERSHIP = "spec/ownership_model.md"
_SPEC_REGION = "spec/region_semantics.md"
_SPEC_REP_OWNER = "spec/rep_owner_model.md"
_SPEC_SAFETY = "spec/safety_contract.md"

# Parser/typechecker implementation entries (directory prefixes).
_IMPL_FRONTEND = "frontend/"
_IMPL_PASSES = "passes/"
_IMPL_NIR = "nir/"

# Compatibility / stability governance source.
_COMPAT_STABILITY = "docs/stability_policy.md"

# Default allowed evidence kinds for a documented, implementable language feature.
_LANGUAGE_EVIDENCE_KINDS = (
    EvidenceKind.SPECIFICATION,
    EvidenceKind.SOURCE,
    EvidenceKind.TEST_DEFINITION,
    EvidenceKind.TEST_EXECUTION,
)


def _item(
    key: str,
    title: str,
    aspect: str,
    *,
    specification_paths: tuple[str, ...],
    implementation_entries: tuple[str, ...] = (),
    compatibility_policy_paths: tuple[str, ...] = (),
    test_gate_refs: tuple[str, ...] = (),
    known_non_claims: tuple[str, ...] = (),
    acceptance_evidence: tuple[str, ...] | None = None,
    dependency_criticality: int = 1,
    safety_impact: int = 0,
    claim_risk: int = 1,
    target_unblock_value: int = 1,
) -> ChecklistItem:
    if acceptance_evidence is None:
        acceptance_evidence = (
            f"A normative specification for {title} with cross-revision "
            "compatibility guarantees and matching parser/typechecker evidence.",
        )
    return ChecklistItem(
        key=key,
        title=title,
        aspect=aspect,
        specification_paths=specification_paths,
        implementation_entries=implementation_entries,
        compatibility_policy_paths=compatibility_policy_paths,
        test_gate_refs=test_gate_refs,
        allowed_evidence_kinds=_LANGUAGE_EVIDENCE_KINDS,
        known_non_claims=known_non_claims,
        acceptance_evidence=acceptance_evidence,
        recommended_owner_area="Language & type system",
        dependency_criticality=dependency_criticality,
        safety_impact=safety_impact,
        claim_risk=claim_risk,
        target_unblock_value=target_unblock_value,
    )


# --------------------------------------------------------------------------- #
# Requirement 5.1: language semantics                                         #
# --------------------------------------------------------------------------- #

_LANGUAGE_SEMANTICS_ITEMS: tuple[ChecklistItem, ...] = (
    _item(
        "lexical-rules", "Lexical rules", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_GRAMMAR, _SPEC_CORE),
        implementation_entries=(_IMPL_FRONTEND,),
        compatibility_policy_paths=(_COMPAT_STABILITY,),
        claim_risk=0,
    ),
    _item(
        "control-flow", "Control flow", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_CORE, _SPEC_GRAMMAR),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_PASSES),
    ),
    _item(
        "functions", "Functions", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_CORE,),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_PASSES),
    ),
    _item(
        "methods", "Methods", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_CORE, _SPEC_TYPE_SYSTEM),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_PASSES),
    ),
    _item(
        "modules", "Modules", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_CORE, _SPEC_REFERENCE),
        implementation_entries=(_IMPL_FRONTEND,),
    ),
    _item(
        "visibility", "Visibility", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_CORE, _SPEC_REFERENCE),
        implementation_entries=(_IMPL_FRONTEND,),
    ),
    _item(
        "generics", "Generics", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_GENERICS, _SPEC_TYPE_SYSTEM),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_PASSES),
        dependency_criticality=2,
    ),
    _item(
        "traits-protocols", "Traits or protocols", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_TYPE_SYSTEM, _SPEC_REFERENCE),
        implementation_entries=(_IMPL_PASSES,),
        dependency_criticality=2,
    ),
    _item(
        "closures", "Closures", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_CORE, _SPEC_TYPE_SYSTEM),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_PASSES),
    ),
    _item(
        "patterns", "Patterns", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_CORE, _SPEC_GRAMMAR),
        implementation_entries=(_IMPL_FRONTEND,),
    ),
    _item(
        "error-effects", "Error effects", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_CORE,),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_PASSES),
        safety_impact=1,
    ),
    _item(
        "reflection", "Reflection", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_REFERENCE,),
        known_non_claims=("Runtime reflection is not implemented.",),
        claim_risk=2,
        target_unblock_value=0,
    ),
    _item(
        "macros", "Macros", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_REFERENCE,),
        known_non_claims=("A macro system is not implemented.",),
        claim_risk=2,
        target_unblock_value=0,
    ),
    _item(
        "metaprogramming", "Metaprogramming", ASPECT_LANGUAGE_SEMANTICS,
        specification_paths=(_SPEC_REFERENCE,),
        known_non_claims=("Compile-time metaprogramming is not implemented.",),
        claim_risk=2,
        target_unblock_value=0,
    ),
)


# --------------------------------------------------------------------------- #
# Requirement 5.2: type system                                                #
# --------------------------------------------------------------------------- #

_TYPE_SYSTEM_ITEMS: tuple[ChecklistItem, ...] = (
    _item(
        "primitive-widths", "Primitive widths", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM, _SPEC_ABI_LAYOUT),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_NIR),
        compatibility_policy_paths=(_COMPAT_STABILITY,),
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=0,
    ),
    _item(
        "pointers", "Pointers", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM, _SPEC_SAFETY),
        implementation_entries=(_IMPL_FRONTEND,),
        safety_impact=2,
    ),
    _item(
        "references", "References", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM, _SPEC_OWNERSHIP),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_PASSES),
        safety_impact=1,
    ),
    _item(
        "slices", "Slices", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM,),
        implementation_entries=(_IMPL_FRONTEND,),
    ),
    _item(
        "arrays", "Arrays", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM, _SPEC_ABI_LAYOUT),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_NIR),
    ),
    _item(
        "collections", "Collections", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM,),
        known_non_claims=("A complete standard collection hierarchy is not specified.",),
    ),
    _item(
        "nullable", "Nullable values", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM, _SPEC_CORE),
        implementation_entries=(_IMPL_FRONTEND,),
        safety_impact=1,
    ),
    _item(
        "aggregates", "Aggregates", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_ABI_LAYOUT, _SPEC_TYPE_SYSTEM),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_NIR),
        dependency_criticality=2,
    ),
    _item(
        "enums", "Enums", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_ABI_LAYOUT, _SPEC_TYPE_SYSTEM),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_NIR),
    ),
    _item(
        "callable-types", "Callable types", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM,),
        implementation_entries=(_IMPL_FRONTEND, _IMPL_PASSES),
    ),
    _item(
        "variance", "Variance", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM, _SPEC_GENERICS),
        known_non_claims=("A normative variance model is not specified.",),
        claim_risk=2,
    ),
    _item(
        "lifetimes", "Lifetimes", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_OWNERSHIP, _SPEC_TYPE_SYSTEM),
        implementation_entries=(_IMPL_PASSES,),
        safety_impact=2,
    ),
    _item(
        "constrained-generics", "Constrained generics", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_GENERICS,),
        implementation_entries=(_IMPL_PASSES,),
        dependency_criticality=2,
    ),
    _item(
        "dynamic-dispatch", "Dynamic dispatch", ASPECT_TYPE_SYSTEM,
        specification_paths=(_SPEC_TYPE_SYSTEM, _SPEC_REFERENCE),
        known_non_claims=("Dynamic dispatch is not part of the current type system.",),
        claim_risk=2,
    ),
)


# --------------------------------------------------------------------------- #
# Requirement 5.5: low-level semantic prerequisites                           #
# --------------------------------------------------------------------------- #

_LOW_LEVEL_ITEMS: tuple[ChecklistItem, ...] = (
    _item(
        "target-layout", "Target layout", ASPECT_LOW_LEVEL_SEMANTICS,
        specification_paths=(_SPEC_ABI_LAYOUT,),
        implementation_entries=(_IMPL_NIR,),
        dependency_criticality=3,
        safety_impact=1,
        target_unblock_value=2,
    ),
    _item(
        "initialization", "Initialization", ASPECT_LOW_LEVEL_SEMANTICS,
        specification_paths=(_SPEC_REGION, _SPEC_OWNERSHIP),
        implementation_entries=(_IMPL_PASSES,),
        safety_impact=1,
        dependency_criticality=2,
    ),
    _item(
        "destruction", "Destruction", ASPECT_LOW_LEVEL_SEMANTICS,
        specification_paths=(_SPEC_REGION, _SPEC_OWNERSHIP),
        implementation_entries=(_IMPL_PASSES,),
        safety_impact=1,
        dependency_criticality=2,
    ),
    _item(
        "aliasing", "Aliasing", ASPECT_LOW_LEVEL_SEMANTICS,
        specification_paths=(_SPEC_REP_OWNER, _SPEC_SAFETY),
        implementation_entries=(_IMPL_PASSES,),
        safety_impact=2,
        dependency_criticality=2,
    ),
    _item(
        "syscall-boundaries", "System-call boundaries", ASPECT_LOW_LEVEL_SEMANTICS,
        specification_paths=(_SPEC_INTEROP, _SPEC_ABI_LAYOUT),
        known_non_claims=(
            "No Nebula-owned system-call boundary exists; only a hosted C ABI is specified.",
        ),
        safety_impact=2,
        dependency_criticality=3,
        target_unblock_value=2,
    ),
)


LANGUAGE_TYPE_SYSTEM_CHECKLIST = DeclarativeChecklist(
    domain_key="language-type-system",
    name="Language semantics and type system",
    target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
    description=(
        "Nebula language semantics, type-system rules, and low-level semantic "
        "prerequisites required for sustained non-host-language and system-level "
        "development, assessed with specification, parser/typechecker, and "
        "compatibility-policy evidence kept separate."
    ),
    mandatory_for_target=True,
    items=_LANGUAGE_SEMANTICS_ITEMS + _TYPE_SYSTEM_ITEMS + _LOW_LEVEL_ITEMS,
)


def evaluate_language_type_system(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> DomainDraft:
    """Evaluate language semantics and the type system (Requirements 5.1-5.5)."""

    return ChecklistEvaluator().evaluate(LANGUAGE_TYPE_SYSTEM_CHECKLIST, bundle, guarded)
