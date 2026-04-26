# nebula-pqc-protocols

Preview official Nebula package for pilot-stage application-layer PQC protocol helpers.

Current surface in this repo wave:

- `pqc::kem`
  - `encapsulate_wire(public_key) -> Result<MlKem768WireEncapsulation, String>`
  - `MlKem768WireEncapsulation_envelope(self) -> MlKem768CiphertextEnvelope`
  - `MlKem768WireEncapsulation_shared_secret(self) -> MlKem768SharedSecret`
  - `MlKem768CiphertextEnvelope_format_version(self) -> Int`
  - `MlKem768CiphertextEnvelope_kem_algorithm(self) -> String`
  - `MlKem768CiphertextEnvelope_ciphertext_hex(self) -> String`
  - `MlKem768CiphertextEnvelope_as_json(self) -> Json`
  - `ml_kem_768_ciphertext_envelope_from_json(value: Json) -> Result<MlKem768CiphertextEnvelope, String>`
  - `ml_kem_768_ciphertext_envelope_from_json_text(text: String) -> Result<MlKem768CiphertextEnvelope, String>`
  - `decapsulate_wire(secret_key, envelope) -> Result<MlKem768SharedSecret, String>`
- `pqc::signed`
  - `signed_body(body_sha3_256_hex, signature_hex) -> SignedBody`
  - `sign_body(secret_key, body) -> Result<SignedBody, String>`
  - `SignedBody_body_sha3_256_hex(self) -> String`
  - `SignedBody_signature_hex(self) -> String`
  - `SignedBody_as_json(self) -> Json`
  - `signed_body_from_json(value) -> Result<SignedBody, String>`
  - `signed_body_from_json_text(text) -> Result<SignedBody, String>`
  - `verify_body(public_key, body, signature) -> Bool`
  - `verify_body_bytes(public_key, body, signature_bytes) -> Result<Bool, String>`
  - `verify_signed_body(public_key, body, signed_body) -> Result<Bool, String>`
- `pqc::channel`
  - `channel_signer(keypair) -> ChannelSigner`
  - `signed(signer) -> Option<ChannelSigner>`
  - `pinned_peer(public_key) -> Option<MlDsa65PublicKey>`
  - `initiate(peer_kem_public_key, signer) -> Result<ClientInit, String>`
  - `accept(local_kem_secret_key, signer, hello) -> Result<ServerAccept, String>`
  - `accept_pinned_initiator(local_kem_secret_key, signer, expected_initiator_sign_public_key, hello) -> Result<ServerAccept, String>`
  - `finalize(init, expected_peer_sign_public_key, message) -> Result<Session, String>`
  - `seal_next(session, plaintext, aad) -> Result<SealedMessage, String>`
  - `open_next(session, ciphertext, aad) -> Result<OpenedMessage, String>`
  - `Session_rekey(self) -> Result<Session, String>`

Recommended preview dependency shape from a Nebula repo checkout:

```toml
[dependencies]
pqc = { path = "/path/to/nebula/official/nebula-pqc-protocols" }
```

Release posture:

- repo-local preview / pilot surface
- not installed by Nebula binary release archives or install scripts

Design notes for this wave:

- The package focuses on application-level signed payload flows.
- `pqc::signed` is the current narrow authenticated-body contract for higher-level service and
  communication experiments: callers can sign a body, serialize the signature envelope as JSON, and
  verify that same envelope against the body on receipt.
- It also provides a ciphertext-only ML-KEM transport helper so callers do not need to treat
  `MlKem768Encapsulation.to_bytes()` as a wire format.
- `pqc::channel` keeps local session state separate from the explicit wire messages instead of
  pretending the shared secret or session state is itself a transport payload.
- Servers that know the expected client signing key should use
  `accept_pinned_initiator(...)`; plain `accept(...)` only verifies a self-contained signed hello
  when one is present and remains a looser preview helper for unauthenticated bootstrap experiments.
- It intentionally builds on `nebula-crypto` rather than introducing a new wire protocol runtime.
- HTTP/transport integration remains a higher-level concern; the package stays transport-agnostic and
  emits JSON+hex wire envelopes rather than private binary frames.

Current non-goals for this package revision:

- TLS handshake integration
- certificate or PKI semantics
- canonical HTTP signing specs
- transport/session state machines
