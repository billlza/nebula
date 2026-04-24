# Nebula UI AppKit Adapter Preview

Minimal macOS windowing proof for the future `nebula-ui.tree.v1` AppKit adapter.

Build smoke:

```bash
clang++ -std=c++20 -fobjc-arc \
  -I../common -iquote ../../../.. \
  minimal_window.mm \
  -framework Cocoa \
  -o /tmp/nebula-ui-appkit-smoke
/tmp/nebula-ui-appkit-smoke --smoke --tree-json /path/to/tree.json --smoke-dispatch-action targets.refresh
```

Interactive preview:

```bash
/tmp/nebula-ui-appkit-smoke --title "Nebula UI"
/tmp/nebula-ui-appkit-smoke --tree-json /path/to/tree.json
```

This is intentionally not linked into `official/nebula-ui` by default. It is a guarded native
adapter asset so headless CI and non-macOS hosts do not inherit AppKit framework requirements. It
only consumes a narrow preview summary from the Nebula UI tree.
