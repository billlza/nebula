# nebula-crypto Security Contract

This file makes the current secret-handling boundary explicit for the preview `nebula-crypto`
package.

It is intentionally narrow. It documents what the package does today, what it does not claim, and
what future secure-memory work should preserve.

## Current Contract

The current package exposes a mix of public values, ciphertext values, and secret-bearing values.

Public or non-secret wire values in the current surface include:

- `MlKem768PublicKey`
- `MlKem768Ciphertext`
- `MlDsa65PublicKey`
- `MlDsa65Signature`

Secret-bearing values in the current surface include:

- `ChaCha20Poly1305Key`
- `MlKem768KeyPair`
- `MlKem768SecretKey`
- `MlKem768SharedSecret`
- `MlKem768Encapsulation`
- `MlDsa65KeyPair`
- `MlDsa65SecretKey`

Current guarantees:

- Native bridge temporaries that hold secret material are zeroized before the bridge returns or
  fails.
- Bridge-local secret copies now flow through a package-local `SecretBytesOwner` seam that makes
  secret consumption and export explicit inside the native bridge and zeroizes bridge-owned copies
  on destruction.
- Nebula-side crypto values remain opaque runtime-managed handles rather than caller-constructible
  structs.
- Secret-bearing values are not automatically stringified or exposed as text by this package.

Current non-guarantees:

- There is no dedicated secure-memory runtime type yet.
- There is no promise of page locking, swap resistance, guarded heaps, or zeroize-on-drop for
  persistent Nebula-side secret handles.
- There is no process-wide secret tracking, tainting, or log redaction.
- The package-local bridge owner seam does not change the fact that persistent secret-bearing
  Nebula values are still ordinary runtime-managed `Bytes` under the hood once explicitly exported
  back into the language runtime.

## Secret Export Boundary

`to_bytes()` is the current explicit export boundary.

That boundary matters because some `to_bytes()` calls are ordinary interop helpers while others are
secret export operations.

Ordinary public/ciphertext exports:

- `MlKem768PublicKey.to_bytes()`
- `MlKem768Ciphertext.to_bytes()`
- `MlDsa65PublicKey.to_bytes()`
- `MlDsa65Signature.to_bytes()`

Secret export operations:

- `ChaCha20Poly1305Key.to_bytes()`
- `MlKem768KeyPair.to_bytes()`
- `MlKem768SecretKey.to_bytes()`
- `MlKem768SharedSecret.to_bytes()`
- `MlKem768Encapsulation.to_bytes()`
- `MlDsa65KeyPair.to_bytes()`
- `MlDsa65SecretKey.to_bytes()`

Important nuance:

- `MlKem768Encapsulation.to_bytes()` is a secret export because the current opaque value contains
  both ciphertext and shared secret, not just ciphertext.

Caller obligations after secret export:

- treat the resulting `Bytes` as caller-owned secret material
- avoid logging, tracing, or embedding it in config or env values
- avoid persisting it unless persistence is the explicit goal of the surrounding protocol
- prefer passing opaque values directly across package APIs when possible

## Secure-Memory Skeleton For Future Work

Future secure-memory work should tighten the current contract without pretending secret export can
be made invisible.

The intended direction for a future secure-memory model is:

- protected secret ownership should be distinct from ordinary `Bytes`
- secret export should remain explicit at the API boundary rather than becoming an implicit copy
- public/ciphertext exports should stay cheap and ordinary
- bridge-local secret temporaries should keep their current zeroization discipline
- package-local bridge owner seams should stay explicit and narrow rather than pretending they are
  the same thing as protected runtime secret storage
- secret-bearing runtime values should eventually move to a protected ownership model instead of
  plain runtime-managed opaque storage

The package should not evolve by quietly making every crypto value "kind of secret" or by
pretending every `to_bytes()` export is equally safe. A future upgrade should preserve the
distinction between:

- public/ciphertext interop
- secret ownership
- explicit secret export
