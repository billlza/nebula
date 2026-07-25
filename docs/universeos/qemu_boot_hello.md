# QEMU Boot Hello Plan

This is a future implementation plan for the remaining protocol/link/media/boot work. The separate
`UOS-BOOT-002` primitive object gate now exists, but this plan does not add kernel code,
linker/bootloader files, external boot dependencies, or a passing QEMU test today.

## Boot Path Options

| Boot path | Strengths | Costs | Recommendation |
| --- | --- | --- | --- |
| Limine | Modern x86_64 boot protocol, simple config, good QEMU flow, explicit higher-half contract | Adds a bootloader artifact and protocol dependency | Selected for the first smoke lane; its protocol/ABI candidate is pinned, while full `UOS-BOOT-001` tool closure remains planned |
| Multiboot2 | Familiar GRUB-compatible path, simple early experiments | Older ecosystem defaults, GRUB setup can obscure artifact ownership | Not selected; a future alternative would require its own contract rather than runtime fallback |
| UEFI | Closer to modern firmware and hardware boot flows | Requires PE/COFF and firmware services model before Nebula has freestanding runtime closure | Defer until after the QEMU serial hello |

Decision: use Limine for the first QEMU serial hello because it keeps the boot contract small while
avoiding premature UEFI runtime surface. There is no automatic Multiboot2 or UEFI fallback: a
Limine protocol, tool, or image failure must remain an explicit failure.

## Protocol And Supply-Chain Decision Boundary

`UOS-BOOT-001` remains planned until the repository pins and verifies a mutually compatible set of
all compiler, linker, bootloader, and image-tool inputs. The repository-owned protocol/ABI candidate
at `boot/uos-x86_64-limine-v1/contract.manifest` now fixes:

- Limine release tag `v12.3.2`, tag object
  `5e6ef2a0ae7afcd863639b78aee1dbb6cacf1b45`, and peeled commit
  `8c8a688776735b2b2d12683a032e442583d361db`
- the release bootstrap-pinned `limine-protocol` commit
  `5b9d13e557590d8eab93fa7449bdd1d7ed72ba8c`
- exact vendored `limine.h` and license byte counts, SHA-256 digests, and 0BSD license identity
- base revision 6, start/end request markers, explicit support check, image `_start`, versioned
  payload entry, high-half floor, 64 KiB minimum stack, and restricted x86-64 System V ABI

The candidate is strict but intentionally insufficient to promote the gate: exact supported
`clang`/`ld.lld`, Limine binary, QEMU, and image-assembly identities and their cross-compatibility
evidence remain absent. Build code must not download or substitute moving upstream branches. The
future linker layout, protocol adapter, toolchain, and Limine release must be validated together.

## Required Future Artifacts

The staged gates should produce:

- `UOS-BOOT-002`: `kernel.o`, a freestanding `ET_REL` object for `x86_64-unknown-none`
- `UOS-BOOT-003`: repository-owned `limine_protocol_adapter.o`, fixed `linker.ld`, and audited
  `kernel.elf`; this is a linked kernel ELF, not a boot image
- `UOS-BOOT-004`: pinned `nebula-boot.conf`, verified Limine artifacts, and deterministic
  `uos-hello.iso` or an explicitly selected raw-image format
- `UOS-BOOT-005`: bounded QEMU invocation, captured serial output, and exact expected output
- expected serial output:

```text
nebula-uos-boot-hello
```

No hosted `std`, C++ standard library, hosted runtime header, filesystem, process, network, time, or
threading dependency may be reachable from the freestanding object.

The vendored upstream `limine.h` includes `<stdint.h>`. The future adapter must not make host
headers reachable to bypass `-nostdinc`; it needs a repository-owned fixed-width integer ABI header
or an equivalently pinned generated definition, plus a compatibility test against the vendored
protocol declarations.

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

QEMU, Limine, `ld.lld`, and image-assembly tools remain unavailable future gate dependencies until
the repository adds their pinned manifests and explicit setup/verification path. A missing tool is
a gate failure, never a reason to silently use a different executable or hosted path.

## Gate Design

Separated gates:

- `UOS-BOOT-002`: experimental primitive object generation for `x86_64-unknown-none`
- `UOS-BOOT-003`: deterministic linked kernel ELF
- `UOS-BOOT-004`: version-pinned boot-media assembly
- `UOS-BOOT-005`: QEMU serial hello

The gates must remain separate so object/backend, linker/ELF-policy, bootloader/media, and QEMU
environment regressions can be diagnosed independently.

## Non-Claims

This plan does not prove:

- bootability today; an audited relocatable object is not a linked or bootable image
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
