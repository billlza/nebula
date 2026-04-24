# Security Policy

Nebula is developed in the open, but security-sensitive reports should be sent privately first.

Reporting:

- Please report vulnerabilities by opening a private GitHub security advisory or contacting the maintainer directly through the repository security contact path.
- Include reproduction steps, affected version or commit, and whether the issue impacts compiler correctness, package integrity, runtime safety, crypto, or TLS behavior.

Scope:

- compiler and CLI release artifacts
- package resolution and lock behavior
- runtime networking/HTTP behavior
- official crypto, TLS, service, observe, and PQC protocol packages

Response guidance:

- we prioritize correctness, safety, and honest scope over rushed feature work
- release notes should call out any security-relevant fixes in `1.0.x`
