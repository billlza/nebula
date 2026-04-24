# nebula-ui

Preview official package for Nebula-owned semantic UI trees.

Current V1 shape:

- schema: `nebula-ui.tree.v1`
- core wrappers: `View`, `UiTree`
- adapter baseline: `headless::render(tree) -> Result<String, String>`
- action smoke baseline: `headless::dispatch_action(tree, action) -> Result<String, String>`
- primitive components: `Window`, `Column`, `Row`, `Text`, `Button`, `Input`, `Spacer`
- action contract: interactive components carry stable action identifiers in props; current smoke
  dispatch is lookup-only and returns `nebula-ui-action-dispatched=<id>`
- rendering contract: adapters consume the JSON tree, not source syntax directly

This package is a preview contract. It does not yet provide a complete renderer, style system,
animation model, accessibility stack, or mature AppKit/GTK runtime adapter. The first goal is to
stabilize the UI IR that native adapters can consume.

Native adapter assets:

- `adapters/appkit/minimal_window.mm`: macOS guarded AppKit smoke source
- `adapters/gtk4/minimal_window.cpp`: Linux guarded GTK4 smoke source
- `adapters/common/ui_tree_preview.hpp`: shared native preview decoder for the root-view
  `nebula-ui.tree.v1` JSON shape plus the same lookup-only action dispatch marker
