# Thin Host Bridge Replay

This example is the second real consumer of the extracted thin-host bridge contract from
`examples/thin_host_app_core/packages/bridge`.

It deliberately keeps the same counter app-core while swapping in a different host shape:

- `thin_host_app_core`
  - interactive host shell via `host_cxx`
- `thin_host_bridge_replay`
  - scripted replay/tape driver implemented entirely in Nebula

That keeps the bridge contract honest: the reusable part is the coarse command/event/snapshot seam,
not one specific console shim.

Run it with:

```bash
nebula run . --run-gate none
```
