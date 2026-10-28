# vmmlite on macOS (Apple Silicon)

> [!WARNING]
> **For learning only. Do not use vmmlite in production.** It has no sandbox and models
> only enough hardware to boot this example. Input checks do not make it safe for untrusted guests.

## TL;DR

269 nonblank lines of C, excluding comment-only lines, that boot ARM64 Linux with
Hypervisor.framework to a PID 1 that prints one line through a virtio console and reboots.
The feature scope matches the [Linux/KVM example](../linux-x86_64/README.md).

## Run it

You need Apple Silicon, macOS 15+ for the GIC APIs, Xcode Command Line Tools, `make`,
`cpio`, `dtc`/libfdt, and a compiler that produces static ARM64 Linux executables.
Python 3 is needed only for tests. Install the extra tools:

```sh
brew install dtc
brew install filosottile/musl-cross/musl-cross
```

From the repository root:

```sh
make build:macos-arm64
make kernel:macos-arm64
make run:macos-arm64
```

The root [Makefile](../Makefile) builds and signs the Mac VMM, compiles [machine.dts](machine.dts), and compiles
[../init.c](../init.c) for Linux. The hypervisor entitlement permits the framework calls;
it does not sandbox the VMM. The Mac compiler cannot build Linux's PID 1.
Generated files and logs live in `.build/macos-arm64/` at the repository root.

`make kernel:macos-arm64` downloads Firecracker's public ARM64 Linux 6.1.102 test Image and checks its
SHA-256. This is an uncompressed ARM64 Image despite the download's `vmlinux` name.
It is a reproducible fixture, not a production kernel recommendation. To use another
compatible Image: `make run:macos-arm64 KERNEL=/absolute/path/to/Image`.

Trimmed output:

```text
[    0.000000] earlycon: uart8250 at MMIO 0x0000000009000000 (options '')
[    0.000000] psci: PSCIv0.2 detected in firmware.
[    0.000000] Kernel command line: earlycon=uart8250,mmio,0x09000000 console=hvc0 panic=-1
[    0.000000] GICv3: CPU0: found redistributor 0 region 0:0x0000000010000000
[    0.000000] arch_timer: cp15 timer(s) running at 24.00MHz (virt).
[    0.026749] 9000000.serial: ttyS0 at MMIO 0x9000000 (irq = 13, base_baud = 115200) is a 8250
[    0.027561] printk: console [hvc0] enabled
[    0.027698] printk: bootconsole [uart8250] disabled
[    0.029745] Run /init as init process
Hello from PID 1!
[    0.029811] reboot: Restarting system
vmmlite: guest requested PSCI shutdown/reset
```

The early log uses the UART; everything after `bootconsole [uart8250] disabled`, including
PID 1's line, uses virtqueue 1. Linux's allocated IRQ number 13 is not the GIC interrupt ID:
the UART uses INTID 32, and the virtio console uses INTID 33. Timestamps and Linux IRQ
allocation can vary. The VMM exits zero on PSCI reset or power-off.

## How it works

[vmmlite.c](vmmlite.c) puts the device helpers before `main()`, followed by eight boot steps:

| § | Step | Framework calls |
| --- | --- | --- |
| 1 | A VM and its GICv3 distributor and redistributor | `hv_vm_create`, `hv_gic_create` |
| 2 | 512 MiB of host memory as guest RAM at 0x40000000 | `hv_vm_map` |
| 3 | An uncompressed Image at its header's requested offset | |
| 4 | Initramfs and DTB; the tree carries the command line | |
| 5 | Patch initramfs bounds, GIC regions and timer interrupt IDs | `hv_gic_get_*` |
| 6 | One vCPU, affinity 0, matching the device tree | `hv_vcpu_create` |
| 7 | EL1h, MMU off, interrupts masked; PC at Image, x0 at DTB | `hv_vcpu_set_reg`, `hv_vcpu_set_sys_reg` |
| 8 | Run: UART, virtio MMIO, debug-lock traps and PSCI reset | `hv_vcpu_run`, `hv_gic_set_spi` |

Before `main()` come the two devices:

- `com1()`: the same transmit-only 8250 register subset as Linux, at MMIO 0x09000000.
- `console_register_read()`, `console_register_write()` and `console_kick()`: a modern
  virtio console at 0x0a000000. Queue 1 prints and returns buffers; receive setup and kicks
  are ignored, as in the Linux example. This is intentionally not full virtio conformance.

Follow one output buffer in the source: Linux publishes a descriptor head in the available
ring, writes queue number 1 to `QUEUE_NOTIFY`, and `console_kick()` prints the bytes.
The VMM publishes a used-ring entry before raising INTID 33; Linux then reclaims the
buffer. The register tables and diagrams next to each helper describe this same path.

macOS has no KVM ioeventfd/irqfd path. Each notify exits to this process, which handles it
while the sole vCPU is stopped, then raises a GIC SPI. A worker would add synchronization
without removing that exit. GIC delivery and ARM timer handling stay in the framework.

The shared `init.c` is unchanged: write, drain, reboot. ARM uses a device tree, MMU-off
entry and PSCI where x86 uses boot tables, long-mode preparation and an i8042 reset.
There is no disk, network, keyboard input, second vCPU or guest firmware.

## Tested on

| Host | Guest kernel | Result |
| --- | --- | --- |
| Apple M5 Pro, macOS 27.0.1 | ARM64 Linux 6.1.102 | UART → hvc0 → PID 1 → PSCI, exit 0 |

`make test:macos-arm64` runs native loader/queue/UART checks, three real boots, timer wakeup and 300
ordered console writes, plus malformed-input checks. Logs are under `.build/macos-arm64/`.
The macOS 15 API minimum has not been runtime-tested. libfdt must also support the target
OS: the Homebrew library on the tested host was built for macOS 26 and emits a linker
warning against the 15 deployment target. Use a compatible libfdt for an older host.

## Learn more

- [Linux ARM64 boot protocol](https://docs.kernel.org/arch/arm64/booting.html)
- [Apple Hypervisor.framework](https://developer.apple.com/documentation/hypervisor/apple-silicon)
- [Virtio specification, split rings, MMIO and console](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
- [Exact guest kernel configuration](https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.11/aarch64/vmlinux-6.1.102.config)

## License

[Apache-2.0](../LICENSE)
