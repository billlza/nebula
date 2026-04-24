# Nebula VS Code Extension

This extension provides a VS Code integration for Nebula.

Current features:

- language registration for `.nb`
- syntax highlighting
- incremental document sync plus diagnostics, hover, go-to-definition, references, document
  symbols, workspace symbols, signature help, rename, code actions, semantic tokens, and LSP
  completion through `nebula lsp`
- hover now also surfaces async explain metadata plus service/TLS/HTTP2 transport debug contracts
  for `RequestContext.transport_debug()` helper calls, including stable HTTP/2 phase-event
  `classification.reason` / `classification.detail` metadata
- whole-document formatting through `nebula fmt`
- command palette entries for:
  - `Nebula: Explain Symbol`
  - `Nebula: New Project`
  - `Nebula: Restart Language Server`
- fallback keyword completion only when the language server is unavailable or an older binary does
  not advertise `completionProvider`

Configuration:

- `nebula.binaryPath`: path to the `nebula` CLI binary

Development:

```bash
cd tooling/vscode
npm install
npm run compile
```
