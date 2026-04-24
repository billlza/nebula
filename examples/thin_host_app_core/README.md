# Thin Host + App Core

This preview example demonstrates the current recommended desktop/native direction for Nebula:

- the host owns the shell, event loop, and rendering boundary
- Nebula owns the app state and event-to-state transitions
- the reusable contract now lives in repo-local path packages under `packages/bridge` and
  `packages/counter_core`

Today the host is a tiny C++ shim wired through `host_cxx`; in a future desktop lane the same split
would map to a real WinUI/AppKit/GTK shell instead of this console-backed stand-in.

The same extracted bridge contract is now also consumed by:

- `examples/thin_host_bridge_replay`
  - a scripted replay/tape driver that reuses the same command/event/snapshot seam without the
    interactive `host_cxx` shell

If you want to take this route seriously, read:

- `docs/thin_host_app_shell.md`

The short version is:

- keep rendering, accessibility, navigation, animation, and design tokens in the host
- keep domain state and event-to-state transitions in Nebula
- keep the host<->core boundary coarse-grained so performance stays predictable
- prefer snapshot/view-model rendering over tiny per-widget round trips
- make the bridge contract explicit: commands in, lifecycle events out, snapshots queried for render

Run it with:

```bash
nebula run . --run-gate none
```

Expected output:

```text
host-shell-begin
event:{"schema":"thin-host-bridge.event.v1","kind":"booted","payload":{"counter":0,"can_decrement":false,"status":"idle","last_command":"boot"},"terminal":false}
render:{"schema":"thin-host-bridge.snapshot.v1","screen":"counter","payload":{"counter":0,"can_decrement":false,"status":"idle","last_command":"boot"}}
event:{"schema":"thin-host-bridge.event.v1","kind":"increment_applied","payload":{"counter":1,"can_decrement":true,"status":"active","last_command":"increment"},"terminal":false}
render:{"schema":"thin-host-bridge.snapshot.v1","screen":"counter","payload":{"counter":1,"can_decrement":true,"status":"active","last_command":"increment"}}
event:{"schema":"thin-host-bridge.event.v1","kind":"increment_applied","payload":{"counter":2,"can_decrement":true,"status":"busy","last_command":"increment"},"terminal":false}
render:{"schema":"thin-host-bridge.snapshot.v1","screen":"counter","payload":{"counter":2,"can_decrement":true,"status":"busy","last_command":"increment"}}
event:{"schema":"thin-host-bridge.event.v1","kind":"decrement_applied","payload":{"counter":1,"can_decrement":true,"status":"active","last_command":"decrement"},"terminal":false}
render:{"schema":"thin-host-bridge.snapshot.v1","screen":"counter","payload":{"counter":1,"can_decrement":true,"status":"active","last_command":"decrement"}}
event:{"schema":"thin-host-bridge.event.v1","kind":"increment_applied","payload":{"counter":2,"can_decrement":true,"status":"busy","last_command":"increment"},"terminal":false}
render:{"schema":"thin-host-bridge.snapshot.v1","screen":"counter","payload":{"counter":2,"can_decrement":true,"status":"busy","last_command":"increment"}}
event:{"schema":"thin-host-bridge.event.v1","kind":"quit_requested","payload":{"counter":2,"can_decrement":true,"status":"busy","last_command":"quit"},"terminal":true}
render:{"schema":"thin-host-bridge.snapshot.v1","screen":"counter","payload":{"counter":2,"can_decrement":true,"status":"busy","last_command":"quit"}}
host-shell-end
```

What this sample is trying to demonstrate:

- the host sends coarse command envelopes into Nebula
- Nebula emits compact lifecycle event envelopes back to the host
- Nebula returns one compact snapshot envelope when the host queries render state
- the host still owns how that snapshot is displayed
