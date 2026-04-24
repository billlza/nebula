# Third-Party Components

## BLAKE3

- Upstream: https://github.com/BLAKE3-team/BLAKE3
- Commit: `2e3727d292dcffa9ecf8058b4d9f01a18df517bf`
- License: dual `Apache-2.0` / `CC0-1.0`
- Vendored files:
  - `native/vendor/blake3/blake3.c`
  - `native/vendor/blake3/blake3_dispatch.c`
  - `native/vendor/blake3/blake3_portable.c`
  - `native/vendor/blake3/blake3_neon.c`
  - `native/vendor/blake3/blake3.h`
  - `native/vendor/blake3/blake3_impl.h`
- SHA256:
  - `blake3.c`: `b118ddf7cf9e6e5ef3fded72dcb1acf9dfdc4ea923cbe4605900ad6ee9afe1af`
  - `blake3_dispatch.c`: `134f21550138c0af6312925c988aeee35df287e4119e8ad1d206fccdb2238fe3`
  - `blake3_portable.c`: `2bc25b0dad67b4329d0b49cfa075ab2b0d04e424addbddc4e9c389c52a192524`
  - `blake3_neon.c`: `67e88d017a82df1b76d5f841e7b7215135a007486ac14c5be1407e1bca5201b0`
  - `blake3.h`: `49302a787f386be9c4b7816cd953105c7800e6109cd62ebcfa4f8b821b0b458c`
  - `blake3_impl.h`: `d388dca3574602c8849805ea3c8c0a12d082ac0e428756f305e111942f099af4`

The package uses a single-source shim (`native/blake3_all.c`) that keeps x86 on the portable path and enables NEON only on ARM64 hosts. That preserves correctness across Nebula's current `[native]` contract without pretending we already support asm/per-source SIMD tuning.

## OQS Mini / PQC Slice

- Upstream aggregation reference: https://github.com/open-quantum-safe/liboqs
- Upstream snapshot used for the curated subset: liboqs `0.13.0`
- This package intentionally vendors only the portable pieces needed for:
  - SHA3 / SHAKE support
  - ML-KEM-768 reference implementation
  - ML-DSA-65 reference implementation
- Vendored roots:
  - `native/vendor/oqs_mini/include/oqs/**`
  - `native/vendor/oqs_mini/pqclean_shims/**`
  - `native/vendor/oqs_mini/sha3/**`
  - `native/vendor/oqs_mini/mlkem-native_ml-kem-768_ref/**`
  - `native/vendor/oqs_mini/mldsa65_ref/**`
- License notes:
  - liboqs-derived SHA3 / shim / header subset: `MIT`
  - mlkem-native ML-KEM reference subset: dual `Apache-2.0` / `CC0-1.0`
  - ML-DSA reference subset: dual `Apache-2.0` / `CC0-1.0`
  - XKCP low-level Keccak plain64 files are public-domain / `CC0-1.0`

This is a curated primitive-only vendor slice. It intentionally does not claim TLS, PKI, or a full secure transport stack, and it keeps to portable reference code instead of auto-enabling architecture-specific asm/SIMD paths.

## mbedTLS ChaCha20-Poly1305 Subset

- Upstream project: https://github.com/Mbed-TLS/mbedtls
- Upstream version baseline: `3.6.4`
- License: `Apache-2.0 OR GPL-2.0-or-later`
- Vendored root:
  - `native/vendor/mbedtls/**`
- Usage in this package:
  - minimal `ChaCha20`
  - minimal `Poly1305`
  - `ChaCha20-Poly1305` AEAD
  - platform zeroize / constant-time helpers needed by that slice

This is a narrow communication-focused AEAD subset copied into `nebula-crypto` so the preview
package stays self-contained and does not depend on `nebula-tls` being present beside it.

The package builds this subset with `native/include/mbedtls/mbedtls_config.h`, a package-local
minimal config shadow that only enables the ChaCha20 / Poly1305 / ChaCha20-Poly1305 modules needed
by the preview AEAD surface.
