# Nebula Hosted Registry

This directory contains the hosted registry service and client that ship with Nebula release
artifacts.

Current capabilities:

- immutable versioned package publish over HTTP
- bearer-token protected API
- package list and search endpoints
- package archive download and local mirror sync
- `fetch` helper that mirrors remote exact-version dependencies into a local registry root, then runs `nebula fetch`
- client-side request timeout control via `--timeout-seconds` / `NEBULA_REGISTRY_TIMEOUT_SECONDS`
- archive validation that rejects unsafe paths and non-file/directory entries

Service:

```bash
python3 tooling/registry/server.py serve \
  --root /tmp/nebula-registry \
  --host 127.0.0.1 \
  --port 8080 \
  --token dev-token
```

Client:

```bash
python3 tooling/registry/client.py --server http://127.0.0.1:8080 --token dev-token push examples/hello_api
python3 tooling/registry/client.py --server http://127.0.0.1:8080 --token dev-token list
python3 tooling/registry/client.py --server http://127.0.0.1:8080 --token dev-token search hello
python3 tooling/registry/client.py --server http://127.0.0.1:8080 --token dev-token fetch /path/to/project
```

Installed-binary path:

- release archives carry these helpers under `tooling/registry`
- install prefixes place these helpers under `share/nebula/registry`
- installed `nebula` binaries can call the bundled client through a helper-backed path:
  - `nebula publish ... --registry-url URL`
  - `nebula fetch ... --registry-url URL`
  - `nebula update ... --registry-url URL`
- you can also invoke the installed helpers manually when needed:

```bash
python3 "$PREFIX/share/nebula/registry/server.py" serve --root /tmp/nebula-registry --host 127.0.0.1 --port 8080 --token dev-token
python3 "$PREFIX/share/nebula/registry/client.py" --server http://127.0.0.1:8080 --token dev-token list
```

Design notes:

- The hosted registry stores the same package tree shape as the local registry.
- The client preserves existing `nebula.lock` semantics by mirroring remote packages into a local registry root before calling `nebula fetch`.
- Package publish still goes through the existing `nebula publish` staging flow, then uploads the staged package tree as an archive.
- The bundled helper requires Python 3.11+ because it uses `tomllib`; Nebula preflights that
  requirement for `--registry-url` workflows and reports an explicit error when the helper runtime
  is missing or too old. Set `PYTHON` when the compatible interpreter is not the default command
  on the host.

Current non-goals:

- multi-tenant auth or user accounts
- delta sync
- package signatures
- registry-side dependency solving
- teaching the core resolver about HTTP sources directly
