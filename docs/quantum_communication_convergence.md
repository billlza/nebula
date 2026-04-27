# Quantum Communication Convergence

Nebula's quantum-communication differentiation is the software layer around QKD and PQC, not a
claim that pure software implements the physical quantum channel.

Current direction:

- `official/nebula-qkd` is the real-integration preview package for ETSI GS QKD 014 v1.1.1-style
  KME/KMS key delivery.
- `official/nebula-qcomm-sim` remains the simulation-only BB84 lab package.
- `official/nebula-pqc-protocols` remains the application-layer PQC helper package aligned with
  NIST PQC primitives.

`nebula-qkd` owns:

- provider-facing configuration for KME authority, local SAE, peer SAE, request size, and transport
  posture
- KME target resolution from `http(s)://host[:port][/prefix]` with explicit scheme/`use_tls` and
  host/port drift rejection
- ETSI GS QKD 014 v1.1.1-style paths for status, `enc_keys`, and `dec_keys`
- narrow key-container wire JSON shaped as `keys[].key_ID` and `keys[].key`
- JSON request/response decoding for key containers and key ID requests
- deterministic mock key delivery for local contract tests without hardware
- a cleartext HTTP lab client for local/dev KME contract tests only
- a narrow outbound mTLS adapter preview that reuses `official/nebula-tls` client identity support
  for HTTPS KME requests
- structured error codes that are useful for audit and operator diagnostics

Boundaries:

- It does not implement optical hardware, detectors, synchronization, trusted-node relay, or a
  physical QKD network.
- It does not provide a security certification and is not a QKD-TLS GA contract.
- Production QKD key-delivery deployments normally require mutually authenticated TLS; this remains
  an explicit deployment requirement rather than an implicit fallback.
- The built-in live transports are intentionally narrow: cleartext HTTP is local/dev lab only, and
  the mTLS adapter is a preview HTTPS client path rather than QKD-TLS GA, certificate lifecycle
  management, or a security certification.
- Vendor private SDKs should be adapter layers above the provider contract, not dependencies of the
  preview package.

Reference lines:

- ETSI GS QKD 014 v1.1.1: REST-based QKD key delivery between SAE and KME/KMS.
- ITU-T Y.3803: key management concepts for QKD networks.
- NIST FIPS 203/204/205: PQC key-establishment and signature standards used by Nebula's PQC line.
