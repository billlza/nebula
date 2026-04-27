# Local Ops Console UI

Preview forcing app for the first Nebula-owned UI direction.

This example uses the new `ui ... { view ... }` syntax to define a semantic UI tree and lowers that
tree through the preview `official/nebula-ui` JSON IR. It is intentionally not a Tauri sample.

Current V1 boundary:

- Nebula owns the UI declaration, state input, action identifiers, and stable JSON view tree.
- `headless` renders the same tree into deterministic text for CI and debugging.
- `headless::validate(...)` rejects malformed preview IR before render or dispatch.
- `headless::dispatch_action(...)` validates the same `Button.action` / `Input.action` contract
  used by native adapter smoke assets.
- `Input.accessibility_label` is required and appears in the JSON IR plus headless/native preview
  summaries.
- Guarded AppKit/GTK minimal-window smoke sources live under `official/nebula-ui/adapters`.
- The example prints JSON IR, headless render output, and the stable action dispatch marker so
  compiler, formatter, LSP, package, and adapter wiring can be validated before a full desktop
  adapter becomes the blocking path.

Run:

```bash
nebula run . --run-gate none
```
