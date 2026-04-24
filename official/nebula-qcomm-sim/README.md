# nebula-qcomm-sim

Experimental preview official Nebula package for simulation-only quantum communication labs.

Current surface in this repo wave:

- `qcomm::bb84::default_config(qubit_count: Int) -> SimulationConfig`
- `qcomm::bb84::with_sample_reveal(...) -> SimulationConfig`
- `qcomm::bb84::with_channel_flip_ppm(...) -> SimulationConfig`
- `qcomm::bb84::with_intercept_resend_ppm(...) -> SimulationConfig`
- `qcomm::bb84::with_classical_auth_failure(...) -> SimulationConfig`
- `qcomm::bb84::simulate(config) -> Result<SimulationReport, String>`
- `qcomm::bb84::simulate_seeded(config, seed: Bytes) -> Result<SimulationReport, String>`
- `qcomm::bb84::SimulationReport_as_json(self) -> Json`
- `qcomm::bb84::shared_key_bytes(report) -> Result<Bytes, String>`

Recommended preview dependency shape from a Nebula repo checkout:

```toml
[dependencies]
qcomm = { path = "/path/to/nebula/official/nebula-qcomm-sim" }
```

Release posture:

- experimental repo-local preview surface
- not installed by Nebula binary release archives or install scripts

Design notes for this wave:

- This package is simulation-only. It does not claim real QKD hardware integration.
- The native bridge only simulates BB84 measurement/noise/intercept-resend behavior and returns a report.
- Classical channel authentication is exercised on the Nebula side through the same
  `pqc::signed` authenticated-body envelope used by the broader preview PQC protocol slice, so the
  experiment line shares a concrete application-layer auth contract with the mainline PQC work.
