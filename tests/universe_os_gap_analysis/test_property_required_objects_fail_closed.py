# Feature: nebula-universe-os-gap-analysis, Property 16: Required assessment objects fail closed
# **Validates: Requirements 9.7**
"""Property 16 -- required assessment objects fail closed.

Requirement 9.7: *IF any required Evidence_Record or Capability_Domain
assessment is missing or invalid, THEN THE Gap_Analysis SHALL fail assessment
validation with the affected requirement references.*

Design Property 16: *For all otherwise valid assessment models, removing or
corrupting any required ``Evidence_Record`` or ``CapabilityDomain`` makes
validation fail with the affected object IDs and requirement references, rather
than producing a partial valid report.*

This test starts from the fully valid canonical model produced by
``build_valid_model`` (reused from ``test_validator``) and, for each generated
example, randomly drops or corrupts one of its required ``Evidence_Record`` or
``CapabilityDomain`` objects. It then drives the *real* publish gate
(``validate_assessment_model`` and ``publish_assessment``) and asserts the
fail-closed contract:

* (a) ``validation.valid`` is ``False``;
* (b) the findings carry the affected object ID *and* non-empty requirement
  references; and
* (c) ``publish_assessment`` emits no artifact and writes nothing to disk (no
  partial valid output), leaving any prior assessment untouched.
"""

from __future__ import annotations

import dataclasses
import tempfile
from pathlib import Path

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.model_builder import (
    RenderedArtifact,
    publish_assessment,
)
from tools.universe_os_gap_analysis.models import (
    AssessmentModel,
    CapabilityDomain,
    EvidenceRecord,
    FindingSeverity,
    ValidationFinding,
)
from tools.universe_os_gap_analysis.validator import validate_assessment_model

from .test_validator import build_valid_model

# Required objects in the canonical valid model that Property 16 targets. Every
# one of these is referenced elsewhere in the model, so dropping or corrupting it
# must break a required cross-reference and fail the assessment closed.
_EVIDENCE_IDS = ("ev-hosted", "ev-spec")
_DOMAIN_IDS = ("domain-hosted", "domain-kernel")


def _report_renderer(model: AssessmentModel) -> RenderedArtifact:
    """A trivial renderer; it must never be reached for an invalid model."""

    return RenderedArtifact(name="report.json", content=b"{}")


def _object_refs(findings: tuple[ValidationFinding, ...]) -> set[str]:
    return {str(ref) for finding in findings for ref in finding.object_refs}


def _drop_evidence(model: AssessmentModel, record_id: str) -> AssessmentModel:
    """Remove a required evidence record, leaving dangling references behind."""

    remaining = tuple(
        record for record in model.evidence_records if str(record.id) != record_id
    )
    return dataclasses.replace(model, evidence_records=remaining)


def _corrupt_evidence(
    model: AssessmentModel, record_id: str, salt: int
) -> AssessmentModel:
    """Corrupt a required evidence record so it cites a path outside inventory."""

    records = []
    for record in model.evidence_records:
        if str(record.id) == record_id:
            record = dataclasses.replace(
                record, source_path=f"corrupted/missing_{salt}.md"
            )
        records.append(record)
    return dataclasses.replace(model, evidence_records=tuple(records))


def _drop_domain(model: AssessmentModel, domain_id: str) -> AssessmentModel:
    """Remove a required capability domain that an assessment still references."""

    remaining = tuple(
        domain for domain in model.domains if str(domain.id) != domain_id
    )
    return dataclasses.replace(model, domains=remaining)


def _corrupt_domain(
    model: AssessmentModel, domain_id: str, salt: int
) -> AssessmentModel:
    """Corrupt a required domain so it references a nonexistent evidence record."""

    ghost = f"ev-ghost-{salt}"
    domains: list[CapabilityDomain] = []
    for domain in model.domains:
        if str(domain.id) == domain_id:
            new_ids = tuple({*(str(ref) for ref in domain.evidence_ids), ghost})
            domain = dataclasses.replace(domain, evidence_ids=new_ids)
        domains.append(domain)
    return dataclasses.replace(model, domains=tuple(domains))


@st.composite
def _broken_models(
    draw: st.DrawFn,
) -> tuple[AssessmentModel, str, str]:
    """Draw a mutated model plus the id of the object that was broken.

    Returns ``(model, offending_id, description)`` where ``offending_id`` is the
    identifier of the dropped/corrupted required object that must surface in the
    fail-closed findings.
    """

    base = build_valid_model()
    salt = draw(st.integers(min_value=0, max_value=1_000_000))
    kind = draw(st.sampled_from(("evidence", "domain")))
    action = draw(st.sampled_from(("drop", "corrupt")))

    if kind == "evidence":
        target = draw(st.sampled_from(_EVIDENCE_IDS))
        if action == "drop":
            return _drop_evidence(base, target), target, f"drop evidence {target}"
        return (
            _corrupt_evidence(base, target, salt),
            target,
            f"corrupt evidence {target}",
        )

    target = draw(st.sampled_from(_DOMAIN_IDS))
    if action == "drop":
        return _drop_domain(base, target), target, f"drop domain {target}"
    return _corrupt_domain(base, target, salt), target, f"corrupt domain {target}"


# Feature: nebula-universe-os-gap-analysis, Property 16: Required assessment
# objects fail closed - dropping or corrupting any required Evidence_Record or
# CapabilityDomain must fail validation with the affected object id and
# requirement references, and must publish no artifact (no partial valid output).
# **Validates: Requirements 9.7**
@given(broken=_broken_models())
@settings(max_examples=200, deadline=None, print_blob=True)
def test_required_objects_fail_closed(
    broken: tuple[AssessmentModel, str, str],
) -> None:
    model, offending_id, description = broken

    # Sanity: the unmutated base model is genuinely valid, so any failure below
    # is caused by the mutation and not a pre-existing defect.
    baseline = validate_assessment_model(build_valid_model())
    assert baseline.valid, f"base model must be valid, got {baseline.findings}"

    # (a) Validation fails closed for the mutated model.
    result = validate_assessment_model(model)
    assert not result.valid, f"{description} must invalidate the model"

    errors = tuple(
        finding
        for finding in result.findings
        if finding.severity is FindingSeverity.ERROR
    )
    assert errors, f"{description} must produce at least one error finding"

    # (b) The findings carry the affected object id AND requirement references.
    assert offending_id in _object_refs(result.findings), (
        f"{description}: offending id {offending_id!r} missing from findings "
        f"{[f.object_refs for f in result.findings]}"
    )
    # Every error finding cites at least one governing requirement, and the
    # findings that name the offending object do so as well (fail-closed
    # reporting is never anonymous).
    for finding in errors:
        assert finding.requirement_refs, f"{finding.code} lacks requirement refs"
    citing = [
        finding
        for finding in errors
        if offending_id in {str(ref) for ref in finding.object_refs}
    ]
    assert citing, f"{description}: no error finding cites {offending_id!r}"
    for finding in citing:
        assert finding.requirement_refs, (
            f"finding {finding.code} cites {offending_id!r} without requirement refs"
        )

    # (c) The publish gate emits no artifact and writes nothing (all-or-nothing).
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp) / "assessment-out"
        publish = publish_assessment(
            model, renderers=(_report_renderer,), output_dir=out_dir
        )
        assert not publish.published, f"{description} must not publish"
        assert publish.written_paths == (), "no files may be written on failure"
        assert not publish.artifacts, "no artifact may be rendered for invalid model"
        # No partial valid output: the output directory holds no report files
        # (the transaction returns before any file is committed).
        if out_dir.exists():
            assert list(out_dir.iterdir()) == [], "output dir must stay empty"

    # The publish validation mirrors the standalone validator: still invalid.
    assert not publish.validation.valid
