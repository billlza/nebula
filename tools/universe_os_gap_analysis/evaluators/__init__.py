"""Declarative capability evaluators for Universe OS gap analysis.

Each evaluator consumes the evidence/claim-guard layers and produces domain
drafts plus classified gap candidates for a bounded slice of the capability
model. Evaluators are intentionally split into separate modules so independent
tasks can land side by side without sharing (or duplicating) a base class; a
later reconciliation task may lift shared draft types into this package if a
common base emerges.

This ``__init__`` is deliberately minimal (no re-exports) so parallel evaluator
work does not contend over a single shared surface. Import the concrete
evaluator you need from its module, e.g.
``tools.universe_os_gap_analysis.evaluators.memory_concurrency_safety``.
"""

from __future__ import annotations
