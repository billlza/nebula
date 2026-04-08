# Nebula Spec Index (v1.0.0)

This index keeps the language identity front-and-center: core semantics first, tooling second.

## 1. Read order

1. Language Core (what Nebula is)
2. Static Analysis (how Nebula assists)
3. Tooling/CLI (how to drive the compiler)
4. Experimental/Infra (engineering internals)

## 2. Layer map

- Language Core:
  `spec/language_core.md`, `spec/grammar.ebnf`, `spec/region_semantics.md`, `spec/rep_owner_model.md`
- Static Analysis:
  `spec/static_analysis.md`, `spec/escape_analysis.md`, `spec/safety_contract.md`,
  `spec/diagnostics.md`
- Tooling/CLI:
  `spec/tooling_cli.md` (compat entry: `spec/cli_contract.md`)
- Experimental/Infra:
  `spec/experimental_infra.md`

## 3. Language center

Nebula is defined by:
- explicit regions
- Rep x Owner semantics
- explicit unsafe boundary

Borrow/exclusivity (`NBL-T09x`) is a conservative safety assist layer, not the core language thesis.

## 4. Pipeline

`Source -> AST -> Typed AST -> NIR/CFG -> EscapeAnalysis -> RepOwner inference -> C++23 -> clang++`

## 5. Ownership of changes

- Language meaning changes: Language Core docs
- Analyzer and diagnostics behavior: Static Analysis docs
- User CLI behavior: Tooling/CLI docs
- cache/reuse/grouping/baseline internals: Experimental/Infra docs
