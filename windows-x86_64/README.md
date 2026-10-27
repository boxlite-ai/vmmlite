## TL;DR

A single-file Windows x64 WHP companion to vmmlite, with the same eight boot steps; native Windows boot validation is pending.

## Details

> [!WARNING]
> **For learning only. Do not use vmmlite in production.** The example models a limited PC
> and runs without host-process sandboxing. Its input checks do not make it suitable for
> untrusted workloads.

One virtual CPU, 512 MiB of RAM, a Linux bzImage, and the shared [init.c](../init.c).
COM1 carries the early kernel log; the virtio console carries `hvc0` output.
PID 1 prints `Hello from PID 1!` and reboots, ending the run.

## Run it

### Build the Windows executable

Install [Zig](https://ziglang.org/download/) and GNU Make, with both on `PATH`.
The following target works as a cross-build on macOS/Linux, or as a Windows x64
build. Zig supplies MinGW WHP headers and import libraries; a separate Windows SDK
is not needed for this build.

```sh
cd windows-x86_64
make build
```

The output is `vmmlite.exe`. It imports `WinHvPlatform.dll` and
`WinHvEmulation.dll` from Windows. The whole VMM is in [vmmlite.c](vmmlite.c), including the loader and device helpers.

### Prepare the Linux guest

On Linux/macOS, with Zig, Make and `cpio` available:

```sh
cd windows-x86_64
make guest
```

This builds `../init.c` as a static **Linux x86_64 ELF**, then puts `/init` into
`initramfs.cpio`. The Windows executable and Linux executable are different
targets, even though they share an x86_64 instruction set.

Use an x86_64 **bzImage**, not an ELF `vmlinux` or an ARM64 `Image`. The loader
requires boot protocol 2.12 or newer, a relocatable kernel, and a decompression
window that fits below the initramfs in 512 MiB. The relevant kernel options must
be built in (`=y`); this archive supplies no modules:

```text
CONFIG_64BIT=y
CONFIG_X86_64=y
CONFIG_RELOCATABLE=y
CONFIG_BLK_DEV_INITRD=y
CONFIG_BINFMT_ELF=y
CONFIG_X86_MPPARSE=y
CONFIG_X86_LOCAL_APIC=y
CONFIG_X86_IO_APIC=y
CONFIG_TTY=y
CONFIG_VIRTIO=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_SERIAL_EARLYCON=y
CONFIG_PRINTK=y
```

The kernel's built-in initramfs must provide `/dev/console`, as the usual default
initramfs does. A distro kernel with virtio-console available only as a module
will not work with this minimal archive.

An optional host-side check loads your real artifacts through the same loader:

```sh
make check-image KERNEL=/path/to/bzImage
```

This checks the image handoff and memory placement; it does not execute the guest.

### Run on Windows 11 x64

Enable CPU virtualization in firmware and **Windows Hypervisor Platform** in
Windows Features, then restart Windows. To enable the feature from an elevated
PowerShell session:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All
```

If Windows itself is a VM, its outer hypervisor must expose nested virtualization.
Copy `vmmlite.exe`, `bzImage`, `initramfs.cpio` and `test-boot.ps1` into one Windows
directory. Run in that directory:

```powershell
.\vmmlite.exe --probe
.\test-boot.ps1
```

For guest files elsewhere:

```powershell
.\test-boot.ps1 -Kernel C:\guests\bzImage -Initramfs C:\guests\initramfs.cpio -TimeoutSeconds 120
```

The harness drains stdout and stderr concurrently, kills a timed-out process,
and requires all three observations: a zero exit code, the PID 1 message, and a
nonzero virtio TX count followed by guest reset. It writes `boot.stdout.log`,
`boot.stderr.log` and, on success, `boot.evidence.json` with OS and artifact hashes.
Keep these files when reporting a native boot result.

The **expected** final guest message is:

```text
Hello from PID 1!
```

This is an acceptance marker, **not a captured WHP boot log**. The direct command
`vmmlite.exe bzImage initramfs.cpio` is also available; use the bounded harness for
verification because a guest panic can also request a reboot.

## How it works

[vmmlite.c](vmmlite.c) has eight numbered sections in `main()`, one per boot step,
matching the [Linux/KVM example](../linux-x86_64/README.md):

| § | Step | WHP calls / source |
| --- | --- | --- |
| 1 | A partition, with one processor and a local APIC; create the PIC, IOAPIC and PIT state | `WHvCreatePartition`, `WHvSetPartitionProperty`, `WHvSetupPartition`, `pc_create` |
| 2 | 512 MiB of host memory as guest RAM at address 0; create the console state | `VirtualAlloc`, `WHvMapGpaRange`, `console_create` |
| 3 | Validate the bzImage and initramfs placement, then copy the kernel payload to 1 MiB | `load_kernel` |
| 4 | The zero page: setup header, command line, initramfs and E820 map | `setup_zero_page` |
| 5 | An MP table: one CPU, one IOAPIC, ISA IRQ n → pin n | `setup_mptable` |
| 6 | A GDT and identity-mapped page tables for long mode | `setup_page_tables` |
| 7 | One vCPU in long mode at the 64-bit entry, RSI at the zero page | `WHvCreateVirtualProcessor`, `boot_registers` → `WHvSetVirtualProcessorRegisters` |
| 8 | The run loop: port IO, device MMIO, CPUID, interrupt delivery, and guest reset | `WHvRunVirtualProcessor`, WinHvEmulation callbacks, `WHvRequestInterrupt` |

Before `main()` come the device and boot helpers:

- `com1()`, the 16550 UART on IRQ 4, carries output until the virtio console registers.
- `console_register_read()` and `console_register_write()` serve the console's registers.
  A write of queue 1 to `QUEUE_NOTIFY` calls `console_transmit()`: read guest RAM, print
  the buffers, return them in the used ring, and raise IRQ 5 unless the driver suppresses it.
- The PIC, IOAPIC and PIT helpers supply devices the KVM example delegates to its host
  kernel. WHP supplies the local APIC. Every device runs on the VP thread; the timer
  helper only wakes it with `WHvCancelRunVirtualProcessor`.

[init.c](../init.c) stays at the repository root as shared guest code. It writes one
line, drains the console, and requests reboot. The [walkthrough](tutorial.md) follows
these same section numbers from CPU background to the full boot path.

### Deliberate limits

The PIT models binary modes 0, 2 and 3; other programming fails with a diagnostic.
The PIC uses fixed priority. The IOAPIC accepts fixed, lowest-priority and NMI
delivery, and uses the explicit MP-table mapping **ISA IRQ n → pin n**, including
IRQ 0 → pin 0. This differs from a conventional PC's PIT-to-pin-2 wiring.
The RTC reports a fixed valid date and generates no interrupts. The UART and
virtio console transmit only. These are contracts for this example,
not a general PC emulator.

Every device runs on the VP thread. The timer helper only cancels the blocking
WHP run call; it never reads device state or writes guest RAM. Virtio TX completes
synchronously while the single VP is stopped. This avoids the Linux example's
`ioeventfd`, `irqfd`, and separate console worker.

## Tested on

| Host | What ran | Result / boundary |
| --- | --- | --- |
| macOS ARM64, Zig 0.16.0 | Windows x64 cross-build | PE executable linked against WHP and WinHvEmulation; no Windows execution |
| macOS ARM64, native C compiler | Loader and device tests with ASan/UBSan | Existing loader, PIC/IOAPIC, PIT/UART and virtio cases exercised |
| macOS ARM64, Ubuntu `6.8.0-147-generic` bzImage | Host-side image loading | Kernel and initramfs placement checked; guest instructions not executed |
| Windows 11 x64 with WHP | Capability probe, PowerShell harness and Linux boot | **Not run yet** |

### Run the portable checks

On macOS/Linux with a C compiler supporting ASan and UBSan:

```sh
make check
```

`make check` runs the device/loader tests and builds the Windows executable.
Tests cover malformed boot headers and overlapping initialization windows,
PIC priority and cascade, IOAPIC edge/level behavior, PIT count latches and
calibration output, UART divisor access, virtio negotiation, descriptor bounds,
cycles, completion, interrupt acknowledgment, reset and 16-bit ring wrap.
They cannot establish WHP register acceptance, timer delivery, or guest boot.

## Learn more

- [The walkthrough](tutorial.md): machine diagrams, a Firecracker comparison, background, the boot sequence, code and verification
- [The original Linux/KVM example](../linux-x86_64/README.md)
- [WHP API](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/hypervisor-platform) and [instruction emulator](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-instruction-emulator/hypervisor-instruction-emulator)
- [Linux x86 boot protocol](https://docs.kernel.org/arch/x86/boot.html) and [virtio specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
- [Design and native acceptance](https://github.com/boxlite-ai/vmmlite/issues/13)

## License

[Apache-2.0](../LICENSE)
