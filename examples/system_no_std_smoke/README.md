# System No-Std Smoke Fixture

This fixture intentionally avoids bundled `std::...` imports. It is used to prove the current
experimental system/no-std gate can check and build a tiny program through the hosted C++23 backend.

It does not prove freestanding execution, kernel suitability, driver support, or C++ standard
library independence.

Expected current commands:

```bash
nebula check examples/system_no_std_smoke --target system --no-std --panic abort
nebula build examples/system_no_std_smoke --target system --no-std --panic abort
nebula build examples/system_no_std_smoke --target freestanding --no-std --panic abort
nebula build examples/system_no_std_smoke --target x86_64-unknown-none --no-std --panic trap
```

The build emits hosted C++ and an executable artifact with runtime/target/panic markers.
