# Generics policy

Nebula generics are intentionally explicit and monomorphized. The current 1.0 surface is meant to
support real reusable libraries without adding traits, runtime generic dispatch, or implicit
lifetime machinery.

## Surface syntax

Generic parameters are currently supported on `struct`, `enum`, and `fn` items:

```nebula
struct Pair<T, U> {
  left: T,
  right: U,
}

enum Result<T, E> {
  Ok(T)
  Err(E)
}

fn swap<T, U>(pair: Pair<T, U>) -> Pair<U, T> {
  Pair<U, T>(pair.right, pair.left)
}
```

Types may instantiate zero or more explicit type arguments:

```nebula
Pair<Int, String>
Result<Pair<Int, Int>, String>
```

Function calls do not yet support explicit call-site type arguments. Generic function
instantiation is inferred from value arguments and expected result context.

## Semantics (compiler contract)

- **Item-level generics only**: generic parameters belong to `struct`, `enum`, and `fn`.
- **Explicit type instantiation**: generic types must be written with the required number of type
  arguments.
- **Argument-driven function inference**: generic functions are instantiated from call arguments,
  with expected-type context used where the implementation supports it.
- **Invariant**: no variance rules are defined.
- **Monomorphized**: the compiler may emit specialized code for each instantiated type.
  - In the C++ backend bootstrap, this maps to C++ templates.

## Current intentional restrictions

- No traits or interface-constrained generics.
- No explicit call-site specialization syntax such as `foo<Int>(x)`.
- No runtime generic dispatch or erased generic containers.
- Generic `extern fn` declarations are rejected because the host ABI contract is not yet defined.

## Typechecker rules

- Using a generic type without the required type arguments is an error.
- Supplying type arguments to a non-generic type is an error.
- Generic function calls that do not provide enough information to infer all type parameters are an
  error.
