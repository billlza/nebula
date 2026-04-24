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
- reject malformed nodes instead of silently falling back
- surface UI actions as `action_id + payload`; `Button.action` is required in the current preview
- expose lookup-only action dispatch smoke with the stable
  `nebula-ui-action-dispatched=<id>` marker
- preserve accessibility labels when props provide them

Planned first native adapters:

- `appkit`: macOS desktop adapter
- `gtk4`: Linux desktop adapter

Current status:

- the semantic tree and benchmark path are implemented
- `headless` provides the default testable adapter inside the Nebula package
- AppKit and GTK4 minimal native smoke sources live under this directory, consume a narrow preview
  summary from `nebula-ui.tree.v1`, and are guarded by host platform/dependency checks
- the shared native preview helper reuses `runtime/nebula_runtime.hpp` JSON parsing rather than
  adding a separate adapter-local parser
- native `--smoke-dispatch-action` is a contract smoke, not a real clicked/callback runtime
- no mature renderer, style engine, animation, or distribution claim is made yet
