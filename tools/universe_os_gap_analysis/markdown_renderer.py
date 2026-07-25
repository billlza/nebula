"""Deterministic narrative Markdown renderer (Task 11.3).

This module projects the single canonical
:class:`~tools.universe_os_gap_analysis.models.AssessmentModel` into the
human-readable narrative report ``assessment.md``. It is one of the three Task 11
renderers and conforms to the ``Renderer`` contract defined in
:mod:`~tools.universe_os_gap_analysis.model_builder`
(``Renderer = Callable[[AssessmentModel], RenderedArtifact]``), so it can be
handed straight to :func:`~tools.universe_os_gap_analysis.model_builder.publish_assessment`.

The renderer is a *lossless, additive-free projection*: it never introduces a
fact that is not already present in the canonical model. Everything it prints --
every revision field, inventory entry, evidence record, capability score, gap,
Hard-Gate, conflict, assumption, and non-claim -- comes from the model. The only
fixed text it adds is structural narration (section headings, the non-additive
disclaimer, and column labels), never new claims about the repository.

Required sections (Requirement 14.1, and the Task 11.3 acceptance list):

1. Executive conclusion (observed conclusions + recommendations, separated)
2. Assessment revision (three evidence axes kept distinct)
3. Source inventory
4. Current baseline (evidence grouped by ``Evidence_Status``)
5. Target model ``T0``-``T5``
6. Maturity rubric (0-5 ordinal meanings)
7. Capability matrix (one row per domain)
8. Gap register (one row per gap; observed facts separated from recommendations)
9. Mermaid Hard-Gate dependency DAG
10. Prioritized parallel roadmap (observed facts separated from recommendations)
11. Evidence conflicts
12. Trust assumptions
13. Non-claims
14. Unvalidated / unexecuted evidence disclosure

Every material conclusion cites a repository-relative path plus the smallest
stable anchor/case/gate identifier available (Requirement 14.4, 14.5). Observed
current facts are kept in typed sections separate from recommendations
(Requirement 14.7). The report states explicitly that capability maturity is
non-additive, is not progress, and is not a schedule (Requirement 3.7, 13.7).

The renderer is deterministic: the canonical model already sorts every
collection by stable identifier, and this module only ever iterates those sorted
collections (or the fixed ``TargetLevel`` / ``MaturityScore`` / ``EvidenceStatus``
enum orders), so identical models always render byte-for-byte identical output.
"""

from __future__ import annotations

from typing import Iterable

from .catalog import (
    MATURITY_RUBRIC,
    NON_ADDITIVE_MATURITY_STATEMENT,
    TARGET_LEVEL_DEFINITIONS,
    UNIVERSE_OS_DEFINITION,
)
from .model_builder import RenderedArtifact
from .models import (
    AssessmentModel,
    CapabilityAssessment,
    CapabilityDomain,
    EvidenceRecord,
    EvidenceStatus,
    ExecutionState,
    GapEntry,
    HardGate,
    LocationKind,
    SourceLocation,
    TargetLevel,
)

# The narrative artifact's stable file name.
ARTIFACT_NAME = "assessment.md"

# The explicit, non-additive / non-progress / non-schedule disclaimer. It is a
# fixed piece of structural narration (Requirement 3.7, 13.7) -- not a claim
# about the repository -- so it is safe for the renderer to always emit.
NON_ADDITIVE_DISCLAIMER = (
    f"{NON_ADDITIVE_MATURITY_STATEMENT} A capability score measures demonstrated "
    "capability at the bound revision; it is **not** a progress indicator and "
    "**not** a schedule or effort estimate. A passing prerequisite gate proves "
    "only that named gate's scope."
)


def render_markdown(model: AssessmentModel) -> RenderedArtifact:
    """Render the canonical model into the ``assessment.md`` artifact.

    Conforms to the ``Renderer`` callable contract so it can be passed directly
    to :func:`publish_assessment`. ``projected_ids`` is the exact set of
    canonical object identifiers the narrative references, so the publish gate's
    parity check can confirm the renderer introduced no foreign facts.
    """

    if not isinstance(model, AssessmentModel):
        raise TypeError("model must be an AssessmentModel")

    referenced: set[str] = set()
    text = markdown_report(model, referenced=referenced)
    return RenderedArtifact(
        name=ARTIFACT_NAME,
        content=text.encode("utf-8"),
        projected_ids=frozenset(referenced),
    )


def markdown_report(
    model: AssessmentModel, *, referenced: set[str] | None = None
) -> str:
    """Return the deterministic narrative Markdown report as a string.

    ``referenced``, when supplied, is populated with every canonical object
    identifier the report cites, so the caller can build ``projected_ids``.
    """

    if not isinstance(model, AssessmentModel):
        raise TypeError("model must be an AssessmentModel")
    refs = referenced if referenced is not None else set()

    domains_by_id = {str(domain.id): domain for domain in model.domains}
    records_by_id = {str(record.id): record for record in model.evidence_records}

    lines: list[str] = []
    lines.append("# Nebula Universe OS Gap Analysis")
    lines.append("")
    lines.append(f"> {NON_ADDITIVE_DISCLAIMER}")
    lines.append("")
    lines.append(
        "Observed current facts and recommendations are kept in separate, "
        "clearly labelled sections throughout this report."
    )
    lines.append("")

    _executive_conclusion(model, lines, refs, records_by_id)
    _assessment_revision(model, lines, refs)
    _source_inventory(model, lines, refs)
    _current_baseline(model, lines, refs)
    _target_model(model, lines)
    _maturity_rubric(lines)
    _capability_matrix(model, lines, refs, domains_by_id)
    _gap_register(model, lines, refs, domains_by_id)
    _hard_gate_graph(model, lines, refs)
    _prioritized_roadmap(model, lines, refs)
    _evidence_conflicts(model, lines, refs)
    _trust_assumptions(model, lines)
    _non_claims(model, lines)
    _unvalidated_evidence(model, lines, refs)

    # Root identity anchor (Requirement 14.4): the whole report is bound to the
    # repository root object.
    refs.add(str(model.revision.repository_root_id))

    return "\n".join(lines).rstrip("\n") + "\n"


# --------------------------------------------------------------------------- #
# Section renderers.                                                           #
# --------------------------------------------------------------------------- #


def _executive_conclusion(
    model: AssessmentModel,
    lines: list[str],
    refs: set[str],
    records_by_id: dict[str, EvidenceRecord],
) -> None:
    lines.append("## 1. Executive Conclusion")
    lines.append("")
    lines.append("### Observed facts")
    lines.append("")
    if model.observed_conclusions:
        for conclusion in model.observed_conclusions:
            refs.add(str(conclusion.id))
            citations = _citations_for(conclusion.evidence_ids, records_by_id, refs)
            suffix = f" {citations}" if citations else ""
            lines.append(f"- {_inline(conclusion.text)}{suffix}")
    else:
        lines.append("- No observed conclusions were recorded.")
    lines.append("")
    lines.append("### Recommendations")
    lines.append("")
    if model.recommendations:
        for recommendation in model.recommendations:
            refs.add(str(recommendation.id))
            gap_refs = _reference_tokens(recommendation.related_gap_ids, refs)
            suffix = f" (related gaps: {gap_refs})" if gap_refs else ""
            lines.append(f"- {_inline(recommendation.text)}{suffix}")
    else:
        lines.append("- No recommendations were recorded.")
    lines.append("")


def _assessment_revision(
    model: AssessmentModel, lines: list[str], refs: set[str]
) -> None:
    revision = model.revision
    refs.add(str(revision.repository_root_id))
    lines.append("## 2. Assessment Revision")
    lines.append("")
    lines.append(
        "The three evidence axes below are distinct and must never be "
        "substituted for one another."
    )
    lines.append("")
    lines.append("| Field | Value |")
    lines.append("| --- | --- |")
    rows = (
        ("Repository root", str(revision.repository_root_id)),
        ("Schema version", revision.schema_version),
        ("Commit", revision.commit_id),
        ("Branch", revision.branch),
        ("Version", revision.version),
        ("Describe", revision.describe),
        ("Tags", ", ".join(revision.tags) if revision.tags else "(none)"),
        ("Worktree clean", "yes" if revision.worktree_clean else "no"),
        ("Assessed at (UTC)", revision.assessed_at_utc.isoformat()),
        ("Fingerprint algorithm", revision.fingerprint_algorithm),
        ("Worktree fingerprint", revision.worktree_fingerprint),
        ("Tracked diff hash", revision.tracked_diff_hash),
        ("Untracked path-set hash", revision.untracked_path_set_hash),
    )
    for label, value in rows:
        lines.append(f"| {label} | {_cell(value)} |")
    lines.append("")
    axes = revision.evidence_axes
    if axes is not None:
        lines.append(
            f"- **Tagged release axis:** describe `{_inline(axes.tagged_release.describe)}`, "
            f"{len(axes.tagged_release.tags)} tag(s); proves only the tagged release scope."
        )
        lines.append(
            f"- **Committed revision axis:** commit `{axes.committed_revision.commit_id}` "
            f"on branch `{axes.committed_revision.branch}`; immutable committed content only."
        )
        clean = "clean" if axes.current_worktree.worktree_clean else "dirty"
        lines.append(
            f"- **Current worktree axis:** based on commit "
            f"`{axes.current_worktree.base_commit_id}`, worktree {clean}; used for this "
            "observation and never rendered as tagged-release fact."
        )
    if revision.excluded_paths:
        lines.append("")
        lines.append("Excluded paths (never product source):")
        for excluded in revision.excluded_paths:
            lines.append(
                f"- `{excluded.path}` -- {_inline(excluded.reason)} "
                f"(rule {excluded.rule_version})"
            )
    lines.append("")


def _source_inventory(
    model: AssessmentModel, lines: list[str], refs: set[str]
) -> None:
    lines.append("## 3. Source Inventory")
    lines.append("")
    if model.source_inventory:
        lines.append(
            "| Entry | Category | Path | Origin | Inspected | Execution | Anchors |"
        )
        lines.append("| --- | --- | --- | --- | --- | --- | --- |")
        for entry in model.source_inventory:
            refs.add(str(entry.id))
            anchors = ", ".join(entry.stable_anchors) if entry.stable_anchors else "(none)"
            lines.append(
                f"| {entry.id} | {entry.category.value} | `{entry.path}` | "
                f"{entry.revision_origin.value} | "
                f"{'yes' if entry.inspected else 'no'} | "
                f"{entry.execution_state.value} | {_cell(anchors)} |"
            )
    else:
        lines.append("No source inventory entries were recorded.")
    lines.append("")


def _current_baseline(
    model: AssessmentModel, lines: list[str], refs: set[str]
) -> None:
    lines.append("## 4. Current Baseline")
    lines.append("")
    lines.append(
        "Each accepted claim carries exactly one evidence status. Statuses are "
        "grouped below; every claim cites a repository-relative path and its "
        "smallest stable anchor."
    )
    lines.append("")
    by_status: dict[EvidenceStatus, list[EvidenceRecord]] = {}
    for record in model.evidence_records:
        by_status.setdefault(record.status, []).append(record)
    if not model.evidence_records:
        lines.append("No evidence records were recorded.")
        lines.append("")
        return
    for status in EvidenceStatus:
        records = by_status.get(status)
        if not records:
            continue
        lines.append(f"### {status.value}")
        lines.append("")
        for record in records:
            refs.add(str(record.id))
            citation = _cite(record.source_path, record.location)
            lines.append(
                f"- **{record.id}** ({record.evidence_kind.value}, "
                f"confidence {record.confidence.value}): {_inline(record.claim)} "
                f"-- {citation}"
            )
        lines.append("")


def _target_model(model: AssessmentModel, lines: list[str]) -> None:
    lines.append("## 5. Target Model (T0-T5)")
    lines.append("")
    lines.append(
        f"**Universe OS.** {UNIVERSE_OS_DEFINITION} The six target levels are "
        "strictly ordered; hosted adjacency (T0) never counts as OS substrate "
        "completion."
    )
    lines.append("")
    present = set(model.target_levels)
    lines.append("| Level | Title | Boundary | Definition |")
    lines.append("| --- | --- | --- | --- |")
    for definition in TARGET_LEVEL_DEFINITIONS:
        marker = "" if definition.level in present else " (absent)"
        lines.append(
            f"| {definition.level.value}{marker} | {definition.title} | "
            f"{definition.boundary.value} | {_cell(definition.definition)} |"
        )
    lines.append("")


def _maturity_rubric(lines: list[str]) -> None:
    lines.append("## 6. Maturity Rubric")
    lines.append("")
    lines.append("| Score | Meaning |")
    lines.append("| --- | --- |")
    for entry in MATURITY_RUBRIC:
        lines.append(f"| {int(entry.score)} | {_cell(entry.meaning)} |")
    lines.append("")
    lines.append(f"> {NON_ADDITIVE_DISCLAIMER}")
    lines.append("")


def _capability_matrix(
    model: AssessmentModel,
    lines: list[str],
    refs: set[str],
    domains_by_id: dict[str, CapabilityDomain],
) -> None:
    lines.append("## 7. Capability Matrix")
    lines.append("")
    lines.append(
        "One row per capability domain. Scores are ordinal 0-5 and non-additive."
    )
    lines.append("")
    lines.append(
        "| Domain | Target | Raw | Effective | Confidence | Status | "
        "Evidence | Next Hard-Gate |"
    )
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
    if model.assessments:
        for assessment in model.assessments:
            domain_id = str(assessment.domain_id)
            refs.add(domain_id)
            domain = domains_by_id.get(domain_id)
            name = domain.name if domain is not None else domain_id
            target = domain.target_level.value if domain is not None else "?"
            evidence = _reference_tokens(assessment.evidence_ids, refs) or "(none)"
            next_gate = str(assessment.next_hard_gate_id)
            refs.add(next_gate)
            lines.append(
                f"| {_cell(name)} ({domain_id}) | {target} | "
                f"{int(assessment.raw_score)} | {int(assessment.effective_score)} | "
                f"{assessment.confidence.value} | {assessment.evidence_status.value} | "
                f"{_cell(evidence)} | {next_gate} |"
            )
    else:
        lines.append("| (no assessments recorded) | | | | | | | |")
    lines.append("")


def _gap_register(
    model: AssessmentModel,
    lines: list[str],
    refs: set[str],
    domains_by_id: dict[str, CapabilityDomain],
) -> None:
    lines.append("## 8. Gap Register")
    lines.append("")
    ordered = sorted(model.gaps, key=_gap_priority_key)
    lines.append("### Observed facts")
    lines.append("")
    if ordered:
        lines.append(
            "| Gap | Title | Primary | Secondary | Domains | Status | Target | "
            "Severity | Priority | Observed fact |"
        )
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for gap in ordered:
            refs.add(str(gap.id))
            domains = _reference_tokens(gap.domain_ids, refs, known=domains_by_id)
            secondary = (
                ", ".join(item.value for item in gap.secondary_categories)
                if gap.secondary_categories
                else "(none)"
            )
            priority = (
                f"{gap.dependency_criticality}/{gap.safety_impact}/"
                f"{gap.claim_risk}/{gap.target_unblock_value}"
            )
            lines.append(
                f"| {gap.id} | {_cell(gap.title)} | {gap.primary_category.value} | "
                f"{_cell(secondary)} | {_cell(domains)} | {gap.current_status.value} | "
                f"{gap.target_level.value} | {gap.severity.value} | {priority} | "
                f"{_cell(gap.observed_fact)} |"
            )
        lines.append("")
        lines.append(
            "Priority is `dependency criticality / safety impact / claim risk / "
            "target-unblock value`, compared lexicographically then by stable id; "
            "the dimensions are never summed."
        )
    else:
        lines.append("No gaps were recorded.")
    lines.append("")
    lines.append("### Recommendations")
    lines.append("")
    if ordered:
        for gap in ordered:
            owner = _inline(gap.recommended_owner_area)
            acceptance = "; ".join(_inline(item) for item in gap.acceptance_evidence)
            lines.append(
                f"- **{gap.id}** (owner: {owner}): {_inline(gap.recommendation)} "
                f"Acceptance evidence: {acceptance}."
            )
    else:
        lines.append("- No gap recommendations were recorded.")
    lines.append("")


def _hard_gate_graph(
    model: AssessmentModel, lines: list[str], refs: set[str]
) -> None:
    lines.append("## 9. Hard-Gate Dependency Graph")
    lines.append("")
    lines.append(
        "Directed dependency DAG over Hard-Gates; an arrow `A --> B` means gate "
        "`B` depends on (is blocked by) gate `A`."
    )
    lines.append("")
    lines.append("```mermaid")
    lines.append("flowchart LR")
    if model.hard_gates:
        # Deterministic short node keys mapped from sorted gate ids.
        node_key = {
            str(gate.id): f"g{index}"
            for index, gate in enumerate(model.hard_gates)
        }
        for gate in model.hard_gates:
            refs.add(str(gate.id))
            key = node_key[str(gate.id)]
            label = _mermaid_label(f"{gate.id}: {gate.title}")
            lines.append(f'    {key}["{label}"]')
        for gate in model.hard_gates:
            dependent = node_key[str(gate.id)]
            for dependency_id in gate.dependency_ids:
                dep_key = node_key.get(str(dependency_id))
                if dep_key is not None:
                    lines.append(f"    {dep_key} --> {dependent}")
    else:
        lines.append("    empty[No Hard-Gates recorded]")
    lines.append("```")
    lines.append("")


def _prioritized_roadmap(
    model: AssessmentModel, lines: list[str], refs: set[str]
) -> None:
    lines.append("## 10. Prioritized Parallel Roadmap")
    lines.append("")
    lines.append(
        "The roadmap is a dependency frontier ordering of Hard-Gates, not a "
        "schedule. Independent workstreams are parallel branches that converge "
        "on explicit join gates."
    )
    lines.append("")
    ordered_gates = _dependency_ordered_gates(model.hard_gates)

    lines.append("### Observed facts")
    lines.append("")
    if ordered_gates:
        lines.append(
            "| Order | Gate | Target | Status | Maturity | Branch | Join gates | "
            "Depends on |"
        )
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for index, gate in enumerate(ordered_gates):
            refs.add(str(gate.id))
            branch = gate.parallel_branch if gate.parallel_branch else "(none)"
            joins = _reference_tokens(gate.join_gate_ids, refs) or "(none)"
            depends = _reference_tokens(gate.dependency_ids, refs) or "(none)"
            lines.append(
                f"| {index} | {gate.id} | {gate.target_level.value} | "
                f"{gate.status.value} | {int(gate.maturity_score)} | "
                f"{_cell(branch)} | {_cell(joins)} | {_cell(depends)} |"
            )
    else:
        lines.append("No Hard-Gates were recorded.")
    lines.append("")

    lines.append("### Recommendations")
    lines.append("")
    if ordered_gates:
        for gate in ordered_gates:
            owner = _inline(gate.owner_area)
            acceptance = "; ".join(_inline(item) for item in gate.acceptance_evidence)
            lines.append(
                f"- **{gate.id}** (owner: {owner}, target {gate.target_level.value}): "
                f"satisfy {_inline(gate.title)}. Acceptance evidence: {acceptance}."
            )
    else:
        lines.append("- No Hard-Gate recommendations were recorded.")
    lines.append("")


def _evidence_conflicts(
    model: AssessmentModel, lines: list[str], refs: set[str]
) -> None:
    lines.append("## 11. Evidence Conflicts")
    lines.append("")
    if model.conflicts:
        lines.append(
            "Conflicts are recorded losslessly with no inferred winner; every "
            "conflict forces low confidence."
        )
        lines.append("")
        for conflict in model.conflicts:
            refs.add(str(conflict.id))
            members = _reference_tokens(conflict.evidence_ids, refs)
            values = ", ".join(_inline(item) for item in conflict.incompatible_values)
            locations = "; ".join(
                _location_label(location) for location in conflict.locations
            )
            blocking = "blocking" if conflict.blocking else "non-blocking"
            lines.append(
                f"- **{conflict.id}** ({blocking}, claim `{_inline(conflict.claim_key)}`): "
                f"records {members}; incompatible values: {values}; "
                f"locations: {locations}; winner: none."
            )
    else:
        lines.append("No evidence conflicts were recorded.")
    lines.append("")


def _trust_assumptions(model: AssessmentModel, lines: list[str]) -> None:
    lines.append("## 12. Trust Assumptions")
    lines.append("")
    if model.assumptions:
        for assumption in model.assumptions:
            lines.append(f"- {_inline(assumption)}")
    else:
        lines.append("No trust assumptions were recorded.")
    lines.append("")


def _non_claims(model: AssessmentModel, lines: list[str]) -> None:
    lines.append("## 13. Non-Claims")
    lines.append("")
    lines.append(
        "These capabilities are explicitly **not** claimed; they persist until a "
        "corresponding accepted gate exists."
    )
    lines.append("")
    if model.non_claims:
        for non_claim in model.non_claims:
            lines.append(f"- {_inline(non_claim)}")
    else:
        lines.append("No non-claims were recorded.")
    lines.append("")


def _unvalidated_evidence(
    model: AssessmentModel, lines: list[str], refs: set[str]
) -> None:
    lines.append("## 14. Unvalidated / Unexecuted Evidence")
    lines.append("")
    lines.append(
        "Sources inspected but not validated by execution at the bound revision. "
        "An unexecuted source is disclosed here and never presented as a passing "
        "result."
    )
    lines.append("")
    unvalidated = [
        entry
        for entry in model.source_inventory
        if entry.execution_state is not ExecutionState.VALIDATED
    ]
    if unvalidated:
        lines.append("| Entry | Path | Execution state | Detail |")
        lines.append("| --- | --- | --- | --- |")
        for entry in unvalidated:
            refs.add(str(entry.id))
            detail = _inline(entry.execution_detail) if entry.execution_detail else "(none)"
            lines.append(
                f"| {entry.id} | `{entry.path}` | {entry.execution_state.value} | "
                f"{_cell(detail)} |"
            )
    else:
        lines.append("All inspected sources were validated by execution.")
    lines.append("")


# --------------------------------------------------------------------------- #
# Helpers.                                                                     #
# --------------------------------------------------------------------------- #


def _gap_priority_key(gap: GapEntry) -> tuple[int, int, int, int, str]:
    """Strict lexicographic gap priority key (most urgent first).

    Mirrors :func:`tools.universe_os_gap_analysis.roadmap.gap_priority_key`: the
    four heterogeneous dimensions are compared independently (higher is more
    urgent), then the stable id breaks ties. They are never summed.
    """

    return (
        -int(gap.dependency_criticality),
        -int(gap.safety_impact),
        -int(gap.claim_risk),
        -int(gap.target_unblock_value),
        str(gap.id),
    )


def _dependency_ordered_gates(gates: Iterable[HardGate]) -> tuple[HardGate, ...]:
    """Return gates in a deterministic topological order (deps before dependents).

    Only intra-model dependency edges are honoured; references to gates absent
    from the model are ignored for ordering. Falls back to stable-id order for
    any gates left over (e.g. if a cycle somehow reached the renderer), so the
    renderer never raises on an unexpected graph.
    """

    nodes = {str(gate.id): gate for gate in gates}
    indegree: dict[str, int] = {gid: 0 for gid in nodes}
    dependents: dict[str, list[str]] = {gid: [] for gid in nodes}
    for gid, gate in nodes.items():
        for dep in gate.dependency_ids:
            dep_id = str(dep)
            if dep_id in nodes:
                indegree[gid] += 1
                dependents[dep_id].append(gid)

    ready = sorted(gid for gid, degree in indegree.items() if degree == 0)
    order: list[str] = []
    while ready:
        current = ready.pop(0)
        order.append(current)
        newly_ready: list[str] = []
        for dependent in sorted(dependents.get(current, ())):
            indegree[dependent] -= 1
            if indegree[dependent] == 0:
                newly_ready.append(dependent)
        if newly_ready:
            ready = sorted(ready + newly_ready)
    # Append any gates not emitted (defensive; a validated graph is acyclic).
    for gid in sorted(nodes):
        if gid not in order:
            order.append(gid)
    return tuple(nodes[gid] for gid in order)


def _citations_for(
    evidence_ids: Iterable[str],
    records_by_id: dict[str, EvidenceRecord],
    refs: set[str],
) -> str:
    """Render inline citations for a set of evidence ids, resolving path+anchor."""

    citations: list[str] = []
    for evidence_id in evidence_ids:
        key = str(evidence_id)
        refs.add(key)
        record = records_by_id.get(key)
        if record is not None:
            citations.append(f"[{key}: {_cite(record.source_path, record.location)}]")
        else:
            citations.append(f"[{key}]")
    return " ".join(citations)


def _cite(source_path: str, location: SourceLocation) -> str:
    """Render a repository-relative path plus its smallest stable anchor."""

    return f"`{source_path}` ({_location_label(location)})"


def _location_label(location: SourceLocation) -> str:
    """Render the smallest stable anchor label for a source location."""

    value = _inline(location.value)
    kind = location.kind
    if kind is LocationKind.LINE_RANGE:
        return f"lines {value}"
    if kind is LocationKind.HEADING:
        return f'heading "{value}"'
    if kind is LocationKind.SYMBOL:
        return f"symbol `{value}`"
    if kind is LocationKind.CASE_ID:
        return f"case {value}"
    if kind is LocationKind.MANIFEST_KEY:
        return f"manifest key `{value}`"
    if kind is LocationKind.WORKFLOW_JOB:
        return f"workflow job {value}"
    return value


def _reference_tokens(
    reference_ids: Iterable[str],
    refs: set[str],
    *,
    known: dict[str, CapabilityDomain] | None = None,
) -> str:
    """Render a comma-separated list of referenced ids, tracking them in ``refs``."""

    tokens: list[str] = []
    for reference_id in reference_ids:
        key = str(reference_id)
        refs.add(key)
        if known is not None and key in known:
            tokens.append(f"{known[key].name} ({key})")
        else:
            tokens.append(key)
    return ", ".join(tokens)


def _mermaid_label(value: str) -> str:
    """Make text safe for a Mermaid node label (no quotes/brackets/newlines)."""

    flattened = _inline(value)
    return (
        flattened.replace("\\", " ")
        .replace('"', "'")
        .replace("[", "(")
        .replace("]", ")")
    )


def _cell(value: str) -> str:
    """Make text safe for a Markdown table cell (escape pipes, flatten newlines)."""

    return _inline(value).replace("|", "\\|")


def _inline(value: str) -> str:
    """Flatten newlines so a value stays on one Markdown line."""

    return " ".join(str(value).split())
