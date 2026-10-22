# vmmlite

> [!WARNING]
> **For learning only. Do not use vmmlite in production.** It skips what a production VMM needs: it
> does not validate its input files, runs without a sandbox, and emulates only a serial port and a
> reset. To run code in a micro-VM, use a production VMM such as Firecracker or Cloud Hypervisor, or
> a sandbox such as [BoxLite](https://github.com/boxlite-ai/boxlite).

## TL;DR

190 lines of C, not counting comments, that boot a stock x86_64 Linux kernel on KVM to a PID 1 that
prints one line and reboots.

## Run it

You need x86_64 Linux with KVM, access to `/dev/kvm` (on Ubuntu, join the `kvm` group), and gcc
with static glibc and cpio (on Ubuntu, `sudo apt install gcc libc6-dev cpio`).

```sh
gcc -O2 -Wall -Wextra -o vmmlite vmmlite.c
gcc -O2 -Wall -Wextra -static -o init init.c
echo init | cpio -o -H newc > initramfs.cpio
sudo install -m 644 /boot/vmlinuz-$(uname -r) bzImage
./vmmlite bzImage initramfs.cpio
```

About three seconds later, trimmed:

```text
[    0.000000] Command line: console=ttyS0 panic=-1
[    0.000454] found SMP MP-table at [mem 0x0009fc00-0x0009fc0f]
[    0.010610] IOAPIC[0]: apic_id 1, version 17, address 0xfec00000, GSI 0-23
[    0.765218] serial8250: ttyS0 at I/O 0x3f8 (irq = 4, base_baud = 115200) is a 8250
[    1.757880] Run /init as init process
Hello from PID 1!
[    1.766049] reboot: Restarting system
```

vmmlite exits with status 0 when the guest reboots. The kernel must take the 64-bit entry (Linux
3.8 or later) and build in the 8250 serial console, MP table parsing and initramfs support, as
Ubuntu's kernels do.

## How it works

`vmmlite.c` has eight numbered sections, one per boot step:

| § | Step | KVM calls |
| --- | --- | --- |
| 1 | A VM, with an in-kernel PIC, IOAPIC and PIT | `KVM_CREATE_VM`, `KVM_CREATE_IRQCHIP`, `KVM_CREATE_PIT2` |
| 2 | 512 MiB of host memory as guest RAM at address 0 | `KVM_SET_USER_MEMORY_REGION` |
| 3 | The bzImage's 64-bit kernel, copied to 1 MiB | |
| 4 | The zero page: setup header, command line, initramfs, E820 map | |
| 5 | An MP table: one CPU, one IOAPIC, the ISA IRQ routes | |
| 6 | A GDT and identity-mapped page tables for long mode | |
| 7 | One vCPU in long mode at the 64-bit entry, RSI at the zero page | `KVM_SET_CPUID2`, `KVM_SET_SREGS`, `KVM_SET_REGS` |
| 8 | The run loop: COM1 is the console, the keyboard controller reset ends the VM | `KVM_RUN`, `KVM_IRQ_LINE` |

`init.c` is PID 1. It writes one line to the console, waits for the UART to send it, and reboots.

## Tested on

| Host | Guest kernel | Result |
| --- | --- | --- |
| Google Cloud n2-standard-2, Intel, nested KVM | Ubuntu `7.0.0-1014-gcp` | `Hello from PID 1!`, exit 0 in 2.9 s |
| QEMU with emulated AMD-V | Ubuntu `6.8.0-147-generic` | `Hello from PID 1!`, exit 0 |
| GitHub Actions `ubuntu-24.04` | the runner's own kernel | the [boot workflow](.github/workflows/boot.yml), on every push |

## Learn more

- [Building a VMM from scratch](https://blog.boxlite.ai/building-a-vmm-from-scratch-0-why-boxlite-is-replacing-libkrun), the BoxLite blog series this repo accompanies
- [BoxLite VMM design: Boot path](https://github.com/boxlite-ai/boxlite/blob/main/docs/contributing/architecture/vmm/README.md#boot-path)
- [The Linux x86 boot protocol](https://docs.kernel.org/arch/x86/boot.html) and [the KVM API](https://docs.kernel.org/virt/kvm/api.html)

## License

[Apache-2.0](LICENSE)
