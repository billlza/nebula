# Nebula UI Adapter Contract

`nebula-ui.tree.v1` is the stable V1 boundary between Nebula UI source syntax and native renderers.
The current compiler lowering emits a root view node directly:

```json
{"schema":"nebula-ui.tree.v1","component":"Window","props":{},"children":[]}
```

Adapters should consume that root-view shape. The older `UiTree { root: ... }` wrapper helper is not
the native adapter contract.

V1 adapter responsibilities:

- consume a JSON tree with `schema`, `component`, `props`, and `children`
- map primitive components to native widgets where available
- reject malformed nodes instead of silently falling back; current validation accepts only
  `Window`, `Column`, `Row`, `Text`, `Button`, `Input`, and `Spacer`
- surface UI actions as `action_id + payload`; `Button.action` and `Input.action` are required
  in the current preview
- expose lookup-only action dispatch smoke with the stable
  `nebula-ui-action-dispatched=<id>` marker
- preserve `Input.accessibility_label`, which is required to be non-empty
- emit preview accessibility summaries for `Window`, `Button`, and `Input`
- emit ordered preview lifecycle markers for smoke-only host shell startup
- preserve the derived `nebula-ui.layout.v1` / `nebula-ui.render-list.v1` boundary as the
  renderer-facing preview pipeline; JSON tree consumption remains the external adapter boundary

Planned first native adapters:

- `appkit`: macOS desktop adapter
- `gtk4`: Linux desktop adapter
- `metal`: guarded macOS GPU submit smoke for the render-list pipeline

Current status:

- the semantic tree and benchmark path are implemented
- `headless` provides the default testable adapter inside the Nebula package
- AppKit and GTK4 minimal native smoke sources live under this directory, consume a narrow preview
  summary from `nebula-ui.tree.v1`, and are guarded by host platform/dependency checks
- Metal submit smoke validates the same tree and exercises command queue creation/submission, but
  does not yet render pixels or claim a complete GPU renderer
- the shared native preview helper reuses `runtime/nebula_runtime.hpp` JSON parsing rather than
  adding a separate adapter-local parser
- native `--smoke-dispatch-action` is a contract smoke, not a real clicked/callback runtime
- no mature renderer, style engine, animation, accessibility stack, packaging/update, or
  distribution claim is made yet
