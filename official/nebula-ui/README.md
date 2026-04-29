# nebula-ui

Preview official package for Nebula-owned semantic UI trees.

Current V1 shape:

- schema: `nebula-ui.tree.v1`
- core wrappers: `View`, `UiTree`
- adapter validation baseline: `headless::validate(tree) -> Result<Bool, String>`
- adapter baseline: `headless::render(tree) -> Result<String, String>`
- action smoke baseline: `headless::dispatch_action(tree, action) -> Result<String, String>`
- primitive components: `Window`, `Column`, `Row`, `Text`, `Button`, `Input`, `Spacer`
- action contract: interactive components carry stable action identifiers in props; current smoke
  dispatch is lookup-only and returns `nebula-ui-action-dispatched=<id>`
- accessibility contract: `Input.accessibility_label` must be non-empty and is preserved by
  headless/native preview summaries; preview summaries also expose `Window` as `window` and
  `Button.text` as the button accessible name
- window lifecycle preview: native/headless adapter smoke can emit ordered lifecycle markers for
  `boot`, `window-created`, `window-shown`, `window-focused`, and `rendered`
- renderer pipeline preview: `headless::layout(tree)` derives deterministic `nebula-ui.layout.v1`
  bounds, `headless::render_list(layout)` derives `nebula-ui.render-list.v1` display commands,
  `headless::hit_test(layout, x, y)` resolves stable action ids, and `headless::patch(old, new)`
  emits `ui.patch.v1` smoke operations for small state changes
- typed fast path preview: internal benchmark/workload helpers can use `TypedUiViewModel`,
  `TypedActionSlot`, `TypedActionIndex`, and `TypedUiLayout` for action dispatch, hit-test, and
  patch classification without JSON parse/stringify in the hot path. The older
  `FastPreviewTree`/`FastPreviewLayout`/`FastActionIndex` helpers remain compatibility wrappers for
  current Local Ops benchmarks, while the external adapter contract remains the JSON
  `nebula-ui.tree.v1` tree.
- GPU backend preview: a guarded macOS Metal submit smoke can submit an empty command buffer after
  validating the same UI tree contract; the always-available null/headless path remains the test and
  benchmark baseline
- malformed tree contract: headless and native preview adapters reject unsupported schemas,
  unknown components, missing/non-array `children`, empty `Button.action`, empty `Input.action`,
  and empty `Input.accessibility_label`
- JSON wire performance contract: native headless action dispatch and action-summary validation walk
  runtime `JsonValue` object/array indexes first, while keeping `nebula-ui.tree.v1` JSON as the
  external adapter format
- rendering contract: adapters consume the JSON tree, not source syntax directly

This package is a preview contract. It does not yet provide a complete renderer, style system,
animation model, text shaping/IME, accessibility stack, app packaging/update flow, or mature native
runtime adapter. The first goal is to stabilize the UI IR plus Nebula-owned layout/render-list
pipeline that native adapters can consume.

Native adapter assets:

- `adapters/appkit/minimal_window.mm`: macOS guarded AppKit smoke source
- `adapters/gtk4/minimal_window.cpp`: Linux guarded GTK4 smoke source
- `adapters/metal/submit_smoke.mm`: macOS guarded Metal command-queue submit smoke source
- `adapters/common/ui_tree_preview.hpp`: shared native preview decoder for the root-view
  `nebula-ui.tree.v1` JSON shape plus the same lookup-only action dispatch marker
