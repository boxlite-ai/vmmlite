# vmmlite on Linux ARM64

## TL;DR

A standalone KVM example boots an ARM64 Linux Image, prints the shared PID 1 greeting,
and exits when the guest requests PSCI reset. `macos/` remains the Apple Silicon example.

Read the [full walkthrough](docs/walkthrough.md) for the machine diagrams, ARM64 boot
contract, annotated source, and captured boot output.

## Read the example

Start with the nine numbered steps in [vmmlite.c](vmmlite.c), then read the helper
for the step you want to understand. The file follows the same teaching structure
as `macos/` and `linux-x86_64/`: an ownership sketch, device/register tables, and a
boot sequence whose order remains visible in `main()`.

| Question | Read |
| --- | --- |
| Where does the kernel go? | `guest()`, `load()`, `kernel_entry()` and steps 2–3. |
| Why are there two consoles? | `com1()` for early UART bytes; `console_kick()` for hvc0 buffers. |
| How does a guest write reach stdout? | Queue layout, register access, `console_thread()`, `connect_console()`. |
| Who owns interrupts and timers? | `prepare_dtb()` and steps 4–6; KVM supplies GICv3/timers. |
| What happens after reset? | Steps 8–9; stop and join the worker before releasing RAM. |

The [walkthrough](docs/walkthrough.md#2-the-code) follows this source map and includes
the complete files. Linux UAPI names identify virtio registers and bits; comments
explain the protocol, ownership, and ordering rather than restating each assignment.

## Build and run

Use a little-endian AArch64 Linux host with GICv3, PSCI support, and read/write access to
`/dev/kvm`. Virtual machines also need working nested virtualization. A cross-compiler
or a Docker container without usable KVM cannot establish that this example boots.

On Ubuntu/Debian:

```sh
sudo apt-get install build-essential libfdt-dev device-tree-compiler cpio curl python3
cd linux-aarch64
make kernel
make run
make test
```

The Makefile downloads and checks the SHA-256 of the same Linux 6.1.102 ARM64 fixture
used by `macos/`. Despite its upstream filename `vmlinux`, this fixture is a raw `Image`,
not ELF. Supply another compatible raw Image with `make run KERNEL=/path/to/Image`.
It needs built-in GICv3, architected timers, PSCI, initramfs, ELF, OF 8250, virtio-MMIO and virtio-console drivers.

The early log uses an MMIO 8250 UART, then Linux switches to the virtio console `hvc0`.
KVM owns GICv3 and the architected timers. `machine.dts` describes RAM, interrupts and
devices; the loader fills in initramfs bounds, CPU affinity and timer interrupt IDs.
The Image header determines its entry address. Reset/power-off use KVM system events.

The transmit queue uses KVM ioeventfd/irqfd, as in the x86 example. A worker drains
queue 1 while the vCPU runs. Its configuration is protected by a mutex; guest ring
publication uses acquire/release ordering. SPI 1 / INTID 33 is edge-triggered for
irqfd; the UART's SPI 0 / INTID 32 is level-triggered. The input queue is deliberately
unserved. This is enough for the demonstration, not a complete virtio implementation.

`make test` exercises production address/loader/UART code, then requires three real
Linux boots, a timed wakeup, 300 ordered guest writes and rejection of malformed files.
Logs are retained under `.build/`. A successful run must print `Hello from PID 1!`
once and `guest requested PSCI shutdown/reset`, then exit zero.

Verified on 2026-10-27: Ubuntu 24.04 AArch64, host kernel 6.8.0-138-generic, with
hardware-assisted nested KVM inside an Apple Virtualization.framework VM. All three
UART-to-hvc0 boots and the timer/output guest passed using the pinned Linux 6.1.102 Image. This
is actual `/dev/kvm` execution, not a bare-metal result or software CPU emulation.

The readability revision passed the same suite on 2026-10-28. Its compiled `.text`
section was byte-identical to the pre-refactor baseline with GCC `-O2`.

## Limits

For learning only. One vCPU, 512 MiB RAM, output only; no firmware, disks, network,
SMP or host sandbox. Bounds checks do not make this a production security boundary.
The standalone examples intentionally duplicate small device helpers to keep each
platform's boot path readable. See [the implementation design](https://github.com/boxlite-ai/vmmlite/issues/17).
