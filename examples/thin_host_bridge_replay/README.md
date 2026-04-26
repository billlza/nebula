# Thin Host Bridge Replay

This example is the second real consumer of the extracted thin-host bridge contract from
`official/nebula-thin-host-bridge`.

It deliberately keeps the same counter app-core while swapping in a different host shape:

- `thin_host_app_core`
  - interactive host shell via `host_cxx`
- `thin_host_bridge_replay`
  - scripted replay/tape driver implemented entirely in Nebula

That keeps the bridge contract honest: the reusable part is the coarse command/event/snapshot seam,
not one specific console shim.

The first replay section uses the same command tape as `thin_host_app_core`, so contract tests can
compare normalized `event:` and `render:` lines for host/replay parity. The second section exercises
negative paths for malformed JSON, schema mismatch, missing kind, unknown command, and invalid
state transition.

Run it with:

```bash
nebula run . --run-gate none
```
