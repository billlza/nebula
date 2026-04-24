# Nebula UI GTK4 Adapter Preview

Minimal Linux GTK4 windowing proof for the future `nebula-ui.tree.v1` adapter.

Build smoke:

```bash
c++ -std=c++20 \
  -I../common -iquote ../../../.. \
  minimal_window.cpp \
  $(pkg-config --cflags --libs gtk4) \
  -o /tmp/nebula-ui-gtk4-smoke
/tmp/nebula-ui-gtk4-smoke --smoke --tree-json /path/to/tree.json --smoke-dispatch-action targets.refresh
```

Interactive preview:

```bash
/tmp/nebula-ui-gtk4-smoke --title "Nebula UI"
/tmp/nebula-ui-gtk4-smoke --tree-json /path/to/tree.json
```

This is intentionally not linked into `official/nebula-ui` by default. It is a guarded native
adapter asset so non-Linux and GTK-less hosts do not inherit GTK runtime requirements. It does not
yet consume the full Nebula UI tree; it only consumes the narrow preview summary needed for the
minimal smoke window.
