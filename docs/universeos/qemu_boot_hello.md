# QEMU Boot Hello Plan

This is a future implementation plan. It does not add kernel code, bootloader files, external
dependencies, or a passing boot test today.

## Boot Path Options

| Boot path | Strengths | Costs | Recommendation |
| --- | --- | --- | --- |
| Limine | Modern x86_64 boot protocol, simple config, good QEMU flow, practical higher-half options later | Adds a bootloader artifact and protocol dependency | Recommended first smoke path |
| Multiboot2 | Familiar GRUB-compatible path, simple early experiments | Older ecosystem defaults, GRUB setup can obscure artifact ownership | Keep as fallback/reference |
| UEFI | Closer to modern firmware and hardware boot flows | Requires PE/COFF and firmware services model before Nebula has freestanding runtime closure | Defer until after the QEMU serial hello |

Recommendation: use Limine for the first QEMU serial hello because it keeps the boot contract small
while avoiding premature UEFI runtime surface.

## Required Future Artifacts

The first implementation gate should produce:

- `kernel.o`: freestanding object for `x86_64-unknown-none`
- `linker.ld`: explicit sections, entry symbol, load address, alignment, and discard rules
- `nebula-boot.conf`: Limine boot config naming the kernel artifact
- `uos-hello.iso` or equivalent boot image
- QEMU command line with serial output captured
- expected serial output:

```text
nebula-uos-boot-hello
```

No hosted `std`, C++ standard library, hosted runtime header, filesystem, process, network, time, or
threading dependency may be reachable from the freestanding object.

## Future QEMU Command Shape

The final command should be checked into the smoke harness only when all artifacts exist. Planned
shape:

```sh
qemu-system-x86_64 \
  -M q35 \
  -m 256M \
  -no-reboot \
  -no-shutdown \
  -serial stdio \
  -display none \
  -cdrom build/uos-hello.iso
```

The harness should fail closed when:

- QEMU is not available and the gate is configured as required
- serial output does not contain exactly `nebula-uos-boot-hello`
- QEMU exits with an unexpected status
- the timeout expires
- hosted runtime or C++ standard library markers appear in the freestanding artifact

QEMU and Limine should be documented as optional future smoke dependencies until the repository
adds an explicit boot gate setup script.

## Gate Design

Planned gates:

- `UOS-BOOT-002`: object generation for `x86_64-unknown-none`
- `UOS-BOOT-003`: linker script and boot image assembly
- `UOS-BOOT-004`: QEMU serial hello

The gates must remain separate so object/backend regressions, linker-script regressions, and QEMU
environment regressions can be diagnosed independently.

## Non-Claims

This plan does not prove:

- bootability today
- kernel support
- driver support
- interrupts
- MMU
- scheduler
- syscall ABI
- allocator readiness
- process isolation
- production UniverseOS support

Even after a future QEMU hello passes, the result should remain an experiment until release review
promotes the relevant runtime, ABI, backend, and boot gates.
