# Universe Control Plane Example

This is a hosted UniverseOS control-plane skeleton. It demonstrates a small service registry,
desired-state transitions, and a minimal hosted status endpoint. It is not an OS kernel, process
supervisor, boot image, driver framework, freestanding runtime, syscall ABI, scheduler, interrupt
model, or MMU model.

## Layout

- `packages/core`: shared hosted registry model and state names
- `apps/ctl`: operator CLI
- `apps/service`: hosted service profile app with `/healthz`, `/readyz`, and `/v1/status`

## Local Commands

From the repository root:

```sh
nebula fetch examples/universe_control_plane
nebula check examples/universe_control_plane
nebula build examples/universe_control_plane/apps/ctl
nebula build examples/universe_control_plane/apps/service
```

Use an explicit state file while experimenting:

```sh
export UNIVERSE_STATE_PATH="$(pwd)/.tmp/universe-services.log"
nebula run examples/universe_control_plane/apps/ctl --run-gate none -- status
nebula run examples/universe_control_plane/apps/ctl --run-gate none -- register scheduler
nebula run examples/universe_control_plane/apps/ctl --run-gate none -- desired scheduler running
nebula run examples/universe_control_plane/apps/ctl --run-gate none -- list-services
```

Run the hosted service:

```sh
export NEBULA_BIND_HOST=127.0.0.1
export NEBULA_PORT=40520
export UNIVERSE_STATE_PATH="$(pwd)/.tmp/universe-services.log"
nebula run examples/universe_control_plane/apps/service --run-gate none
```

Then query:

```sh
curl http://127.0.0.1:40520/v1/status
```

## Current Behavior

The CLI appends registry events to a JSON Lines file:

```text
{"name":"scheduler","state":"registered"}
{"name":"scheduler","state":"desired_running"}
```

Supported CLI commands:

- `ctl status`
- `ctl list-services`
- `ctl register <name>`
- `ctl desired <name> running|stopped`

State names in the shared model:

- `registered`
- `desired_running`
- `desired_stopped`
- `observed_running`
- `observed_failed`

## Limitations

- No real process supervisor exists yet.
- No service discovery, leases, reconciliation loop, or worker execution exists yet.
- The registry is file-backed for the example. A future version may adopt
  `official/nebula-db-sqlite` once that preview package is intentionally pulled into this example.
- The service is hosted and uses the existing service profile.
- Freestanding/no-std/boot work remains blocked by system-profile, backend, ABI, runtime, and QEMU
  gates.
