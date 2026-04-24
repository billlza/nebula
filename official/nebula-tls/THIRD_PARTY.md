# Third-Party Components

`nebula-tls` currently vendors mbedTLS source under `native/vendor/mbedtls`.

- mbedTLS
  - upstream project: [https://github.com/Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls)
  - license: Apache-2.0 OR GPL-2.0-or-later
  - local license copy: `native/vendor/mbedtls/LICENSE`

- Mozilla CA certificate bundle, distributed via curl CA extract
  - source page: [https://curl.se/docs/caextract.html](https://curl.se/docs/caextract.html)
  - pinned source asset: `https://curl.se/ca/cacert-2026-03-19.pem`
  - local source copy: `native/vendor/cacert.pem`
  - upstream license: MPL 2.0
  - pinned SHA256: `b6e66569cc3d438dd5abe514d0df50005d570bfc96c14dca8f768d020cb96171`

This package currently uses a curated client-focused mbedTLS subset plus a pinned bundled default CA root bundle.

Default-root refresh discipline:

1. Replace `native/vendor/cacert.pem` with the newly pinned curl/Mozilla extract.
2. Update the `pinned source asset` line above if the upstream asset name changed.
3. Run `python3 scripts/sync_tls_root_bundle.py --write` from the repo root.
4. Re-run `python3 scripts/sync_tls_root_bundle.py --check` and the TLS contract tests before merge.
