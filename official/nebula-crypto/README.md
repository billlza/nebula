# nebula-crypto

Preview official Nebula package for low-level crypto primitives with zero default cost for
non-importing projects.

Current surface in this repo wave:

- `crypto::rand::random_bytes(len: Int) -> Result<Bytes, String>`
- `crypto::kdf::blake3_derive_key(context: String, material: Bytes, out_len: Int) -> Result<Bytes, String>`
- `crypto::hash::blake3(data: Bytes) -> Bytes`
- `crypto::hash::sha3_256(data: Bytes) -> Bytes`
- `crypto::hash::sha3_512(data: Bytes) -> Bytes`
- `crypto::hash::hex(data: Bytes) -> String`
- `crypto::hash::from_hex(text: String) -> Result<Bytes, String>`
- `crypto::mac::hmac_sha256_hex(key: String, data: Bytes) -> Result<String, String>`
- `crypto::mac::verify_hmac_sha256_hex(key: String, data: Bytes, expected_hex: String) -> Result<Bool, String>`
- `crypto::aead`
  - `chacha20_poly1305_key_from_bytes(bytes: Bytes) -> Result<ChaCha20Poly1305Key, String>`
  - `key.to_bytes() -> Bytes`
  - `seal(key, nonce, aad, plaintext) -> Result<Bytes, String>`
  - `open(key, nonce, aad, ciphertext) -> Result<Bytes, String>`
- `crypto::pqc.kem`
  - `ml_kem_768_keypair() -> Result<MlKem768KeyPair, String>`
  - `encapsulate(public_key: MlKem768PublicKey) -> Result<MlKem768Encapsulation, String>`
  - `decapsulate(secret_key: MlKem768SecretKey, ciphertext: MlKem768Ciphertext) -> Result<MlKem768SharedSecret, String>`
  - validated `*_from_bytes(...)` / `value.to_bytes()` helpers
- `crypto::pqc.sign`
  - `ml_dsa_65_keypair() -> Result<MlDsa65KeyPair, String>`
  - `sign(secret_key: MlDsa65SecretKey, message: Bytes) -> Result<MlDsa65Signature, String>`
  - `verify(public_key: MlDsa65PublicKey, message: Bytes, signature: MlDsa65Signature) -> Bool`
  - validated `*_from_bytes(...)` / `value.to_bytes()` helpers

Recommended preview dependency shape from a Nebula repo checkout:

```toml
[dependencies]
crypto = { path = "/path/to/nebula/official/nebula-crypto" }
```

Release posture:

- repo-local preview / pilot surface
- not installed by Nebula binary release archives or install scripts
- detailed secret-handling boundary documented in `SECURITY_CONTRACT.md`

Current host support:

- macOS
- Linux

Current non-goals for this package revision:

- TLS / HTTPS
- PKI / certificates
- Windows host support

Design notes for this wave:

- PQC is shipped as a curated, portable primitive slice: ML-KEM-768 and ML-DSA-65.
- The symmetric side is intentionally minimal and communication-oriented: one domain-separated KDF
  and one AEAD, not an algorithm buffet.
- The package intentionally vendors a narrow reference subset rather than pretending we already have a full TLS or certificate stack.
- The vendored AEAD slice is compiled with a package-local minimal `mbedtls/mbedtls_config.h` shadow instead of inheriting the broader TLS package profile.
- Runtime-backed crypto values are exposed as opaque Nebula types; direct construction is rejected and persistence/interop should go through `to_bytes()` / validated `*_from_bytes(...)`.
- `MlKem768Encapsulation` is local opaque state for the caller that performed encapsulation; its
  `to_bytes()` value includes both ciphertext and shared secret, so it is not a ciphertext-only
  wire format.
- Secret material is not yet backed by a dedicated secure-memory runtime type; imported projects get the primitive surface, not a full secret-zeroization story.
- The native bridge now routes bridge-owned secret copies through a package-local `SecretBytesOwner`
  seam with zeroize-on-destroy behavior before explicitly exporting back to ordinary runtime
  `Bytes`; persistent Nebula-side secret values are still plain runtime-managed opaque values, not
  dedicated secure-memory handles.

Secret lifecycle boundary for this wave:

- Bridge-local secret temporaries are zeroized before native calls return or fail.
- Bridge-local secret copies now pass through a package-local owner seam before they are consumed
  by secret-bearing native operations or explicitly exported back into ordinary runtime `Bytes`.
- Opaque Nebula secret values such as `MlKem768SecretKey`, `MlKem768SharedSecret`, and
  `MlDsa65SecretKey` are type-safe runtime-managed handles, but they are not yet protected by a
  dedicated secure-memory ownership model.
- `to_bytes()` exports are deliberate escape hatches for interop / persistence and should be
  treated as caller-managed secret copies outside this package's zeroization boundary.
- Serialized secrets written to files, logs, env vars, or network payloads are entirely outside
  the current contract; this package does not claim automatic scrubbing, secure paging, or
  process-wide secret lifecycle management.
- A future secure-memory contract should tighten the ownership model for persistent secrets; this
  preview package is intentionally explicit that it does not provide that guarantee yet.
- `SECURITY_CONTRACT.md` is the stronger design note for classifying secret-bearing values,
  `to_bytes()` export boundaries, and the minimum invariants future secure-memory work should
  preserve.
