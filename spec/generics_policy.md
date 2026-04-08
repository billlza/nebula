# Generics policy (v1.0.0)

v0.1 generics are intentionally minimal and explicit to keep the bootstrap compiler small.

## Surface syntax

Only **enums** are generic in v0.1:

```
enum Option<T> {
  Some(T)
  None(Void)
}
```

Types can instantiate a single type argument:

```
Option<Int>
```

## Semantics (compiler contract)

- **Single-parameter**: exactly one type parameter per enum.
- **Invariant**: no variance rules are defined in v0.1.
- **Monomorphized**: the compiler may emit specialized code for each instantiated type.
  - In the C++ backend bootstrap, this is typically mapped to C++ templates.

## Typechecker rules (v0.1)

- Using a generic enum without an argument (e.g. `Option`) is an error.
- Supplying type arguments to non-generic types (e.g. `Foo<Int>`) is an error.

