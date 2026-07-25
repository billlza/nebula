"""Deterministic ``assessment.json`` renderer and its JSON Schema (Task 11.1).

This module turns the single canonical
:class:`~tools.universe_os_gap_analysis.models.AssessmentModel` into the
structured ``assessment.json`` artifact. It is a *lossless projection* of the
canonical model and nothing more: every value emitted is derived from the model
(or from the model's own validation state), so the renderer never introduces a
fact that is not already in the model (Requirement 14.7). Concretely the
artifact carries three model-derived views:

* the **full serialized model** -- revision, source inventory, evidence records,
  conflicts, target levels, domains, capability assessments, gaps, Hard-Gates,
  assumptions, non-claims, observed conclusions, and recommendations -- so the
  structured output contains the same executive/inventory/matrix/gap/graph
  content the report must expose (Requirement 14.1, 14.3, 14.4);
* the **complete reference graph** -- every canonical object as a node and every
  reference between objects as an edge -- so automation can traverse the exact
  cross-reference structure the validator checked; and
* the **validation state** -- the fail-closed :class:`ValidationResult` -- so a
  consumer can see whether the model was publishable and why.

Serialization is fully deterministic: it reuses the shared
:func:`~tools.universe_os_gap_analysis.serialization.stable_json_bytes` helper
(canonical key ordering, no timestamps beyond the model's own bound
``assessedAtUtc``, no randomness), so the same model always renders byte-for-byte
identical output.

The renderer conforms to the
:data:`~tools.universe_os_gap_analysis.model_builder.Renderer` interface
(``Callable[[AssessmentModel], RenderedArtifact]``) and declares
``projected_ids`` as *exactly* the canonical object identifiers it references
(:func:`~tools.universe_os_gap_analysis.model_builder.canonical_reference_ids`),
so the publish gate's parity check confirms the artifact projects the whole model
and no foreign facts (Requirement 14.7).

A JSON Schema for the artifact is exposed via :func:`assessment_json_schema` and
can itself be published with :func:`render_schema`; both carry the schema version
so the structured output and its contract stay versioned together.
"""

from __future__ import annotations

from typing import Any, Iterable

from .models import (
    AssessmentModel,
    CapabilityAssessment,
    CapabilityDomain,
    EvidenceConflict,
    EvidenceRecord,
    GapEntry,
    HardGate,
    ObservedConclusion,
    Recommendation,
    SourceInventoryEntry,
)
from .model_builder import RenderedArtifact, canonical_reference_ids
from .serialization import stable_json_bytes, to_primitive

# The versioned contract of the structured artifact this module emits. This is
# the *output document* schema version and is independent of the model's own
# ``revision.schemaVersion`` (which versions the canonical model). Bump this when
# the top-level document shape changes.
ASSESSMENT_JSON_SCHEMA_VERSION = "1.0.0"

# Stable artifact names for the structured output and its schema.
ASSESSMENT_JSON_ARTIFACT_NAME = "assessment.json"
ASSESSMENT_SCHEMA_ARTIFACT_NAME = "assessment.schema.json"

# The `$id` of the emitted JSON Schema, carrying the schema version so consumers
# can pin the contract they validate against.
ASSESSMENT_JSON_SCHEMA_ID = (
    f"https://nebula.dev/schemas/universe-os-gap-analysis/"
    f"assessment-{ASSESSMENT_JSON_SCHEMA_VERSION}.json"
)


def render_assessment_json(model: AssessmentModel) -> RenderedArtifact:
    """Render the deterministic ``assessment.json`` artifact from ``model``.

    The returned :class:`RenderedArtifact` contains the canonical model, its full
    reference graph, and its validation state, serialized with stable key
    ordering. ``projected_ids`` is exactly
    :func:`canonical_reference_ids(model) <canonical_reference_ids>`, so the
    publish gate can confirm the artifact is a lossless, foreign-fact-free
    projection of the model (Requirement 14.7).
    """

    if not isinstance(model, AssessmentModel):
        raise TypeError("model must be an AssessmentModel")

    document = build_assessment_document(model)
    content = stable_json_bytes(document, indent=2)
    return RenderedArtifact(
        name=ASSESSMENT_JSON_ARTIFACT_NAME,
        content=content,
        projected_ids=canonical_reference_ids(model),
    )


def render_schema(_model: AssessmentModel) -> RenderedArtifact:
    """Render the JSON Schema for ``assessment.json`` as a publishable artifact.

    The schema is a pure structural contract; it references no canonical object
    identifiers, so ``projected_ids`` is empty and the parity check treats it as
    introducing no facts. It conforms to the ``Renderer`` interface so it can be
    handed to :func:`~tools.universe_os_gap_analysis.model_builder.publish_assessment`
    alongside :func:`render_assessment_json`.
    """

    content = stable_json_bytes(assessment_json_schema(), indent=2)
    return RenderedArtifact(
        name=ASSESSMENT_SCHEMA_ARTIFACT_NAME,
        content=content,
        projected_ids=frozenset(),
    )


def build_assessment_document(model: AssessmentModel) -> dict[str, Any]:
    """Build the JSON-primitive ``assessment.json`` document for ``model``.

    Every value is derived from ``model`` (or its validation state); the renderer
    adds only the structural framing (schema version, and the reference graph
    computed from the model's own references). Returned as JSON primitives so
    callers can serialize deterministically or validate against the schema.
    """

    if not isinstance(model, AssessmentModel):
        raise TypeError("model must be an AssessmentModel")

    return {
        "schemaVersion": ASSESSMENT_JSON_SCHEMA_VERSION,
        "modelSchemaVersion": str(model.revision.schema_version),
        "assessment": to_primitive(model),
        "referenceGraph": build_reference_graph(model),
        "validation": to_primitive(model.validation),
    }


def build_reference_graph(model: AssessmentModel) -> dict[str, Any]:
    """Compute the complete, deterministic reference graph of ``model``.

    ``nodes`` is every canonical object the model defines (id + kind), sorted by
    id. ``edges`` is every reference one object makes to another (source id,
    relation name, target reference), sorted for stable output. A ``resolved``
    flag marks whether each edge's target is a defined canonical node, so a
    consumer can distinguish an in-model cross-reference from a bare reference
    token (for example an evidence record's ``revisionRef``). The graph is a
    projection of the model's own references and introduces no external facts.
    """

    if not isinstance(model, AssessmentModel):
        raise TypeError("model must be an AssessmentModel")

    canonical = canonical_reference_ids(model)
    nodes = _reference_nodes(model)
    edges = _reference_edges(model, canonical)
    return {"nodes": nodes, "edges": edges}


# --------------------------------------------------------------------------- #
# Reference-graph construction (internal).                                     #
# --------------------------------------------------------------------------- #


def _reference_nodes(model: AssessmentModel) -> list[dict[str, str]]:
    """Every canonical object as a ``{"id", "kind"}`` node, sorted by id."""

    nodes: list[dict[str, str]] = [
        {"id": str(model.revision.repository_root_id), "kind": "AssessmentRevision"}
    ]
    _collect_nodes(nodes, model.source_inventory, "SourceInventoryEntry")
    _collect_nodes(nodes, model.evidence_records, "EvidenceRecord")
    _collect_nodes(nodes, model.conflicts, "EvidenceConflict")
    _collect_nodes(nodes, model.domains, "CapabilityDomain")
    _collect_nodes(nodes, model.gaps, "GapEntry")
    _collect_nodes(nodes, model.hard_gates, "HardGate")
    _collect_nodes(nodes, model.observed_conclusions, "ObservedConclusion")
    _collect_nodes(nodes, model.recommendations, "Recommendation")
    return sorted(nodes, key=lambda node: node["id"])


def _collect_nodes(
    sink: list[dict[str, str]], objects: Iterable[Any], kind: str
) -> None:
    for obj in objects:
        sink.append({"id": str(obj.id), "kind": kind})


def _reference_edges(
    model: AssessmentModel, canonical: frozenset[str]
) -> list[dict[str, Any]]:
    """Every reference in the model as a deterministic edge."""

    edges: list[dict[str, Any]] = []

    for entry in model.source_inventory:
        _add_edges(edges, entry.id, "revisionOrigin", ())

    for record in model.evidence_records:
        _add_edges(edges, record.id, "revisionRef", (record.revision_ref,))
        _add_edges(edges, record.id, "relatedEvidence", record.related_evidence_ids)
        _add_edges(edges, record.id, "scopeCapability", record.scope.capability_ids)

    for conflict in model.conflicts:
        _add_edges(edges, conflict.id, "evidence", conflict.evidence_ids)

    for domain in model.domains:
        if domain.parent_id is not None:
            _add_edges(edges, domain.id, "parent", (domain.parent_id,))
        _add_edges(edges, domain.id, "checklist", domain.checklist_ids)
        _add_edges(edges, domain.id, "evidence", domain.evidence_ids)
        _add_edges(edges, domain.id, "gap", domain.gap_ids)
        _add_edges(edges, domain.id, "dependencyGate", domain.dependency_gate_ids)

    for assessment in model.assessments:
        source = assessment.domain_id
        _add_edges(edges, source, "assessmentDomain", (assessment.domain_id,))
        _add_edges(edges, source, "assessmentEvidence", assessment.evidence_ids)
        _add_edges(edges, source, "nextHardGate", (assessment.next_hard_gate_id,))
        _add_edges(
            edges, source, "blockingDependency", assessment.blocking_dependency_ids
        )

    for gap in model.gaps:
        _add_edges(edges, gap.id, "domain", gap.domain_ids)
        _add_edges(edges, gap.id, "dependency", gap.dependencies)

    for gate in model.hard_gates:
        _add_edges(edges, gate.id, "dependency", gate.dependency_ids)
        _add_edges(edges, gate.id, "blockingDomain", gate.blocking_domain_ids)
        _add_edges(edges, gate.id, "evidence", gate.evidence_ids)
        _add_edges(edges, gate.id, "joinGate", gate.join_gate_ids)

    for conclusion in model.observed_conclusions:
        _add_edges(edges, conclusion.id, "evidence", conclusion.evidence_ids)

    for recommendation in model.recommendations:
        _add_edges(
            edges, recommendation.id, "relatedGap", recommendation.related_gap_ids
        )

    for edge in edges:
        edge["resolved"] = edge["target"] in canonical

    return sorted(
        edges,
        key=lambda edge: (edge["source"], edge["relation"], edge["target"]),
    )


def _add_edges(
    sink: list[dict[str, Any]],
    source: Any,
    relation: str,
    targets: Iterable[Any],
) -> None:
    source_id = str(source)
    for target in targets:
        sink.append(
            {"source": source_id, "relation": relation, "target": str(target)}
        )


# --------------------------------------------------------------------------- #
# JSON Schema for the emitted document.                                        #
# --------------------------------------------------------------------------- #


def assessment_json_schema() -> dict[str, Any]:
    """Return the JSON Schema (2020-12) describing the ``assessment.json`` document.

    The schema pins the output ``schemaVersion``, requires the canonical model
    under ``assessment``, the fail-closed ``validation`` state, and the full
    ``referenceGraph`` (nodes + edges). It carries :data:`ASSESSMENT_JSON_SCHEMA_ID`
    (which embeds the schema version) so structured output and contract stay
    versioned together (Requirement 14.1, 14.3, 14.4, 14.7).
    """

    node_schema: dict[str, Any] = {
        "type": "object",
        "required": ["id", "kind"],
        "additionalProperties": False,
        "properties": {
            "id": {"type": "string", "minLength": 1},
            "kind": {"type": "string", "minLength": 1},
        },
    }
    edge_schema: dict[str, Any] = {
        "type": "object",
        "required": ["source", "relation", "target", "resolved"],
        "additionalProperties": False,
        "properties": {
            "source": {"type": "string", "minLength": 1},
            "relation": {"type": "string", "minLength": 1},
            "target": {"type": "string", "minLength": 1},
            "resolved": {"type": "boolean"},
        },
    }
    validation_schema: dict[str, Any] = {
        "type": "object",
        "required": ["valid", "findings"],
        "properties": {
            "valid": {"type": "boolean"},
            "findings": {
                "type": "array",
                "items": {
                    "type": "object",
                    "required": [
                        "severity",
                        "code",
                        "requirementRefs",
                        "objectRefs",
                    ],
                    "properties": {
                        "severity": {"type": "string"},
                        "code": {"type": "string"},
                        "requirementRefs": {
                            "type": "array",
                            "items": {"type": "string"},
                        },
                        "objectRefs": {
                            "type": "array",
                            "items": {"type": "string"},
                        },
                    },
                },
            },
        },
    }
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": ASSESSMENT_JSON_SCHEMA_ID,
        "title": "Universe OS Gap Analysis Assessment",
        "type": "object",
        "required": [
            "schemaVersion",
            "modelSchemaVersion",
            "assessment",
            "referenceGraph",
            "validation",
        ],
        "additionalProperties": False,
        "properties": {
            "schemaVersion": {
                "type": "string",
                "const": ASSESSMENT_JSON_SCHEMA_VERSION,
            },
            "modelSchemaVersion": {"type": "string", "minLength": 1},
            "assessment": {
                "type": "object",
                "required": [
                    "revision",
                    "sourceInventory",
                    "evidenceRecords",
                    "conflicts",
                    "targetLevels",
                    "domains",
                    "assessments",
                    "gaps",
                    "hardGates",
                    "assumptions",
                    "nonClaims",
                    "observedConclusions",
                    "recommendations",
                    "validation",
                ],
            },
            "referenceGraph": {
                "type": "object",
                "required": ["nodes", "edges"],
                "additionalProperties": False,
                "properties": {
                    "nodes": {"type": "array", "items": node_schema},
                    "edges": {"type": "array", "items": edge_schema},
                },
            },
            "validation": validation_schema,
        },
    }
