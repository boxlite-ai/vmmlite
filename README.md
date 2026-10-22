# vmmlite

> [!WARNING]
> **For learning only. Do not use vmmlite in production.** It skips what a production VMM needs: it
> does not validate its input files or guest addresses, runs without a sandbox, and emulates only a
> serial port, a virtio console and a reset. To run code in a micro-VM, use a production VMM such as
> Firecracker or Cloud Hypervisor, or a sandbox such as [BoxLite](https://github.com/boxlite-ai/boxlite).

## TL;DR

265 lines of C, not counting comments, that boot a stock x86_64 Linux kernel on KVM to a PID 1 that
prints one line through a virtio console and reboots.

## Run it

You need x86_64 Linux with KVM, access to `/dev/kvm` (on Ubuntu, join the `kvm` group), and gcc
with static glibc and cpio (on Ubuntu, `sudo apt install gcc libc6-dev cpio`).

```sh
gcc -O2 -Wall -Wextra -pthread -o vmmlite vmmlite.c
gcc -O2 -Wall -Wextra -static -o init init.c
echo init | cpio -o -H newc > initramfs.cpio
sudo install -m 644 /boot/vmlinuz-$(uname -r) bzImage
./vmmlite bzImage initramfs.cpio
```

Trimmed output:

```text
[    0.000000] Command line: earlycon=uart8250,io,0x3f8 console=hvc0 panic=-1 virtio_mmio.device=4K@0xd0000000:5
[    0.047285] found SMP MP-table at [mem 0x0009fc00-0x0009fc0f]
[    0.331719] IOAPIC[0]: apic_id 1, version 17, address 0xfec00000, GSI 0-23
[    1.540248] virtio-mmio: Registering device virtio-mmio.0 at 0xd0000000-0xd0000fff, IRQ 5.
[    1.888762] serial8250: ttyS0 at I/O 0x3f8 (irq = 4, base_baud = 115200) is a 8250
[    1.959411] printk: legacy console [hvc0] enabled
[    1.964332] printk: legacy bootconsole [uart8250] disabled
[    3.619940] Run /init as init process
Hello from PID 1!
[    3.655297] reboot: Restarting system
```

The lines up to `bootconsole [uart8250] disabled` come through COM1; everything after it, PID 1's
line included, comes through the virtio console. vmmlite exits with status 0 when the guest
reboots. The kernel must take the 64-bit entry (Linux 3.8 or later) and build in the 8250 serial
console, MP table parsing, initramfs support, virtio-mmio with command-line devices and the virtio
console, as Ubuntu's kernels do.

## How it works

`vmmlite.c` has eight numbered sections, one per boot step:

| § | Step | KVM calls |
| --- | --- | --- |
| 1 | A VM, with an in-kernel PIC, IOAPIC and PIT, and the virtio console's eventfds | `KVM_CREATE_VM`, `KVM_CREATE_IRQCHIP`, `KVM_CREATE_PIT2`, `KVM_IOEVENTFD`, `KVM_IRQFD` |
| 2 | 512 MiB of host memory as guest RAM at address 0 | `KVM_SET_USER_MEMORY_REGION` |
| 3 | The bzImage's 64-bit kernel, copied to 1 MiB | |
| 4 | The zero page: setup header, command line, initramfs, E820 map | |
| 5 | An MP table: one CPU, one IOAPIC, the ISA IRQ routes | |
| 6 | A GDT and identity-mapped page tables for long mode | |
| 7 | One vCPU in long mode at the 64-bit entry, RSI at the zero page | `KVM_SET_CPUID2`, `KVM_SET_SREGS`, `KVM_SET_REGS` |
| 8 | The run loop: COM1, the virtio console's registers, and the keyboard controller reset that ends the VM | `KVM_RUN`, `KVM_IRQ_LINE` |

Before `main()` come the two devices:

- `com1()`, a 16550 UART on IRQ 4, which carries the kernel log until the virtio console registers.
- A virtio console at 0xd0000000 on IRQ 5. The guest's kick of its transmit queue wakes a device
  thread through an ioeventfd, and the thread interrupts the guest through an irqfd, so the vCPU
  never waits for the I/O. Firecracker, Cloud Hypervisor and libkrun wire their devices on KVM the
  same way.

`init.c` is PID 1. It writes one line to the console, waits for the console to send it, and reboots.

## Tested on

| Host | Guest kernel | Result |
| --- | --- | --- |
| QEMU with emulated AMD-V | Ubuntu `6.8.0-147-generic` | `Hello from PID 1!` through the virtio console, exit 0 |
| GitHub Actions `ubuntu-24.04` | the runner's own kernel | the [boot workflow](.github/workflows/boot.yml), on every push |

The 190-line version without the virtio console, which blog post #0.5 walks through, is the
[`part-0.5`](https://github.com/boxlite-ai/vmmlite/tree/part-0.5) tag.

## Learn more

- [Building a VMM from scratch](https://blog.boxlite.ai/building-a-vmm-from-scratch-0-why-boxlite-is-replacing-libkrun), the BoxLite blog series this repo accompanies
- [BoxLite VMM design: Boot path](https://github.com/boxlite-ai/boxlite/blob/main/docs/contributing/architecture/vmm/README.md#boot-path)
- [The Linux x86 boot protocol](https://docs.kernel.org/arch/x86/boot.html), [the KVM API](https://docs.kernel.org/virt/kvm/api.html) and [the virtio spec](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)

## License

[Apache-2.0](LICENSE)
